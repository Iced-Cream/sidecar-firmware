// pi/engine/bridge.cpp - see bridge.h: the calls the Circle-side files make, onto core/generic, core/screen,
// core/clock, core/proc, core/kv, core/browse and core/devmidi. Hosted (the core's flags).
#include "bridge.h"

#include <string.h>

#include "browse/browse.h"
#include "clock/clock.h"
#include "code/code.h"
#include "lib/sha256.h"
#include "devmidi/devmidi.h"
#include "device/device.h"
#include "dsp/dsp.h"
#include "fmt/fmt.h"
#include "generic/generic.h"
#include "hooks.h"
#include "kv/link.h"
#include "locks/locks.h"
#include "msamp/cardq.h"
#include "platform.h"
#include "proc/proc.h"
#include "route/route.h"
#include "screen/screen.h"
#include "spsc.h"
#include "store/store.h"

namespace core {
void link_reset(const char* why);   // core/generic/generic.cpp: the clock, the slots and the listeners reset
}

namespace bridge {

static volatile bool s_restart;
static void device_restart() { s_restart = true; }
bool take_restart() { bool r = s_restart; s_restart = false; return r; }

static void name_changed(const char* name, uint32_t id);
static bool s_card;
static const core::code::Board* s_code_board;
void set_code_board(const core::code::Board* b) { s_code_board = b; }
bool begin(void (*to_host)(const uint8_t*, int)) {
  core::hooks.to_rytm = to_host;
  core::hooks.device_restart = device_restart;
  s_card = core::gen::begin();
  core::kv::link::attach_generic();   // 51 STORE records (the card core) and the Pi-side locks (core/kv, core/locks)
  core::kv::link::on_name(name_changed);
  // the DSP library's slots (core/dsp): one arena from the heap at boot, a fixed MB per slot, so PLACE never allocates
  const size_t n = (size_t)core::dsp::kSlots * core::dsp::kSlotBytes;
  uint8_t* a = new uint8_t[n + 64];
  bool dsp_ok = core::dsp::begin((void*)(((uintptr_t)a + 63) & ~(uintptr_t)63), n);
  core::log("dsp: %d blocks in the library, %d slots x %u KB %s", core::dsp::library_size(), core::dsp::kSlots,
            (unsigned)(core::dsp::kSlotBytes >> 10), dsp_ok ? "ready" : "NOT ready");
  // shipped code (core/code): the code arena (64 KB pages, the board makes them executable per blob) and the card
  // core's two blob buffers, from the heap at boot
  const size_t ca = core::code::kArenaBytes, u = core::code::kUnit;
  uint8_t* c = new uint8_t[ca + u];
  uint8_t* ba = new uint8_t[core::code::kMaxBlob];
  uint8_t* bb = new uint8_t[core::code::kMaxBlob];
  bool code_ok = c && ba && bb &&
                 core::code::begin((void*)(((uintptr_t)c + u - 1) & ~(uintptr_t)(u - 1)), ca, ba, bb, s_code_board);
  core::log("code: %u KB arena %s", (unsigned)(ca >> 10), code_ok ? "ready" : "NOT ready (no shipped code)");
  return s_card;
}
void sysex(const uint8_t* d, int len) { core::gen::sysex(d, len); }
bool idle_save() {
  core::kv::link::host_step(8);   // the record store's answers to the Rytm (this core sends to it)
  return core::gen::idle_save();
}

// Popups for the Rytm's screen from core 0 (the stats tick's temperature warning): queued here, shown by the control
// core's tick(), which owns the screen. One producer, one consumer; a full queue drops the newest (offered again).
namespace {
constexpr int kLine = 16;
struct PopupMsg { char l1[kLine + 1], l2[kLine + 1]; };
Spsc<PopupMsg, 4> g_popup_q;
}  // namespace
void rui_popup(const char* line1, const char* line2) {
  PopupMsg m;
  sniprintf(m.l1, sizeof m.l1, "%s", line1 ? line1 : "");
  sniprintf(m.l2, sizeof m.l2, "%s", line2 ? line2 : "");
  g_popup_q.push(m);
}
uint32_t rui_popups() { return core::screen::status_popups(); }

// The network's address for the connect splash (core 0's, as it changes): handed over the same way
namespace {
struct AddrMsg { char ip[24]; };
Spsc<AddrMsg, 2> g_addr_q;
}  // namespace
void screen_address(const char* ip) {
  AddrMsg a;
  sniprintf(a.ip, sizeof a.ip, "%s", ip ? ip : "");
  g_addr_q.push(a);
}

void tick() {
  PopupMsg m;
  while (g_popup_q.pop(&m)) core::screen::popup(m.l1, m.l2[0] ? m.l2 : nullptr);
  AddrMsg a;
  bool have = false;
  while (g_addr_q.pop(&a)) have = true;   // the newest counts
  if (have) core::screen::set_address(a.ip);
  core::gen::tick(core::now_ms());
}

// The pairing code on the Rytm's screen: 30 s, over an open page (a client's) as well
bool pair_popup(const char* line) { return core::screen::popup("SIDECAR PAIR CODE", line, core::screen::kPopupMaxMs, true); }
void pair_popup_end() { core::screen::popup_end(); }
uint32_t rytm_hellos() { return core::screen::hellos(); }
bool rytm_greeted() { return core::screen::greeted(); }

void rytm_resync() { core::screen::resync(); }
void rytm_gone() { core::link_reset("the Rytm's USB MIDI removed"); }

void card_worker(bool on) { core::cardq::set_worker(on); }
int card_service(int n) {
  int k = 0;
  while (k < n && core::cardq::service()) ++k;
  if (k < n) k += core::kv::link::card_step(n - k);   // 51 STORE ops on the card (core/kv, this core's own)
  if (k < n) k += core::browse::card_step();           // a folder's listing for the Rytm (core/browse)
  return k;
}
bool card_busy() { return core::cardq::busy(); }

void devmidi_plugged(int src, bool on, const char* name) { core::devmidi::plugged(src, on, name); }
void devmidi(int src, uint8_t st, uint8_t d1, uint8_t d2) { core::devmidi::midi(src, st, d1, d2); }
void devmidi_set_forward(void (*f)(uint8_t, uint8_t, uint8_t)) { core::devmidi::set_forward(f); }

int store_json(char* out, int cap, unsigned long heap_free) {
  uint32_t fo = 0, fn = core::fmt::unwritten(&fo), co = core::cardq::oldest_ms();
  int cn = core::cardq::pending();
  int n = sniprintf(out, cap,
                    "\"store\":{\"files\":%d,\"bytes\":%lu,\"ready\":%s,\"unwritten\":%lu,\"oldestMs\":%lu,\"held\":%lu,"
                    "\"heapFree\":%lu}",
                    core::store::present(), (unsigned long)core::store::bytes(), core::store::ready() ? "true" : "false",
                    (unsigned long)((cn > 0 ? (uint32_t)cn : 0) + fn), (unsigned long)(fo > co ? fo : co),
                    (unsigned long)core::fmt::post_held(), heap_free);
  return n > 0 && n < cap ? n : 0;
}
// The project's name and ID as the companion sends them (51 STORE's 04 NAME, core/kv), logged as they change
const char* project() { return core::kv::link::project_name(); }
static void name_changed(const char* name, uint32_t id) { core::log("project: \"%s\" (id %08lx)", name, (unsigned long)id); }
// A name for JSON: quotes, backslashes and control characters as '?'
static void json_name(char* q, int cap, const char* s) {
  int j = 0;
  for (; s && *s && j < cap - 1; ++s) q[j++] = (*s == '"' || *s == '\\' || *s < 32) ? '?' : *s;
  q[j] = 0;
}
// The guard's fault and overrun lines name a voice's machine: none renders in this engine
const char* machine_name(int) { return "-"; }
uint32_t machine_gen(int) { return 0; }
uint32_t to_host_dropped() { return core::to_rytm_dropped(); }

// /status's generic members: the router's counts, the Rytm's clock, the record store (kv) and the locks, the processor
// slots, the remote screen
int generic_json(char* out, int cap) {
  const core::gen::Stats g = core::gen::stats();
  const core::clock::Stats c = core::clock::stats();
  const core::proc::Stats p = core::proc::stats();
  const core::screen::Stats s = core::screen::stats();
  const core::browse::Stats bs = core::browse::stats();
  const core::devmidi::Stats ds = core::devmidi::stats();
  const core::kv::link::Stats ls = core::kv::link::stats();
  const core::kv::link::StoreStats ks = core::kv::link::store_stats();
  const core::locks::Stats lk = core::locks::stats();
  uint32_t pid = 0;
  char pname[core::kv::link::kNameMax + 1];
  json_name(pname, sizeof pname, core::kv::link::project_name(&pid));
  char devs[160];
  int dk = 0;
  devs[0] = 0;
  for (int i = 0; i < core::devmidi::kSrcs; ++i) {
    const char* nm = core::devmidi::present(i) ? core::devmidi::name(i) : nullptr;
    char q[core::devmidi::kNameLen + 1];
    json_name(q, sizeof q, nm);
    int w = sniprintf(devs + dk, sizeof devs - dk, "%s%s%s%s", i ? "," : "", nm ? "\"" : "", nm ? q : "null", nm ? "\"" : "");
    if (w < 0 || w >= (int)sizeof devs - dk) break;
    dk += w;
  }
  char slots[256];
  int k = 0;
  slots[0] = 0;
  for (int i = 0; i < core::proc::kSlots; ++i) {
    const core::proc::Proc* q = core::proc::slot(i);
    const char* nm = q ? q->name() : nullptr;
    int w = sniprintf(slots + k, sizeof slots - k, "%s%s%s%s", i ? "," : "", nm ? "\"" : "", nm ? nm : "null", nm ? "\"" : "");
    if (w < 0 || w >= (int)sizeof slots - k) break;
    k += w;
  }
  int n = sniprintf(out, cap,
                    "\"generic\":{\"project\":\"%s\",\"projectId\":%lu,\"messages\":%lu,\"dropped\":%lu,\"lastDropped\":%u,\"hellos\":%lu,\"foreign\":%lu},"
                    "\"clock\":{\"transport\":%d,\"pattern\":%d,\"queued\":%d,\"bpm10\":%d,\"ticks\":%lu,\"states\":%lu,\"bad\":%lu},"
                    "\"proc\":{\"slots\":[%s],\"params\":%lu,\"dropped\":%lu,\"blocks\":%lu},"
                    "\"screen\":{\"open\":%s,\"client\":%s%s%s,\"caps\":%d,\"hellos\":%lu,\"opens\":%lu,\"frames\":%lu,"
                    "\"popups\":%lu,\"bad\":%lu},"
                    "\"devmidi\":{\"devices\":[%s],\"thru\":%s,\"messages\":%lu,\"forwarded\":%lu,\"toSlots\":%lu},"
                    "\"browse\":{\"asks\":%lu,\"listings\":%lu,\"folders\":%d,\"leftOut\":%lu,\"errors\":%lu}",
                    pname, (unsigned long)pid, (unsigned long)g.messages, (unsigned long)g.dropped, (unsigned)g.last_dropped,
                    (unsigned long)g.hellos, (unsigned long)g.foreign, (int)core::clock::transport(), (int)core::clock::pattern(),
                    (int)core::clock::queued(), (int)(core::clock::bpm() * 10.0 + 0.5), (unsigned long)c.ticks, (unsigned long)c.states,
                    (unsigned long)c.bad, slots, (unsigned long)p.params, (unsigned long)p.dropped,
                    (unsigned long)p.blocks, core::screen::open() ? "true" : "false",
                    core::screen::client() ? "\"" : "", core::screen::client() ? core::screen::client()->name : "null",
                    core::screen::client() ? "\"" : "", core::screen::caps(), (unsigned long)core::screen::hellos(),
                    (unsigned long)s.opens, (unsigned long)s.frames, (unsigned long)core::screen::popups(),
                    (unsigned long)s.bad, devs, core::devmidi::thru() ? "true" : "false", (unsigned long)ds.messages,
                    (unsigned long)ds.forwarded, (unsigned long)ds.to_slots, (unsigned long)bs.asks,
                    (unsigned long)bs.listings, bs.folders, (unsigned long)bs.left_out, (unsigned long)bs.errors);
  if (n <= 0 || n >= cap) return 0;
  int m = sniprintf(out + n, cap - n,
                    ",\"kv\":{\"project\":%lu,\"records\":%d,\"hellos\":%lu,\"reads\":%lu,\"writes\":%lu,"
                    "\"keeps\":%lu,\"refused\":%lu,\"lastRefused\":%d,\"in\":%lu,\"out\":%lu,\"names\":%lu,"
                    "\"droppedIn\":%lu,\"droppedOut\":%lu},"
                    "\"locks\":{\"sets\":%d,\"ops\":%lu,\"bad\":%lu,\"values\":%lu}",
                    (unsigned long)ks.project, ks.records, (unsigned long)ks.hellos, (unsigned long)ks.reads,
                    (unsigned long)ks.writes, (unsigned long)ks.keeps, (unsigned long)ks.refused, ks.last_refused,
                    (unsigned long)ls.in, (unsigned long)ls.out, (unsigned long)ls.names, (unsigned long)ls.dropped_in,
                    (unsigned long)ls.dropped_out, lk.sets, (unsigned long)lk.ops, (unsigned long)lk.bad,
                    (unsigned long)lk.values);
  if (m <= 0 || m >= cap - n) return 0;
  n += m;
  // the route (core/route): counts, then each open point as [point, owner, src, dst, gain]
  const core::route::Stats rs = core::route::stats();
  m = sniprintf(out + n, cap - n, ",\"route\":{\"open\":%d,\"routed\":%lu,\"moved\":%lu,\"closed\":%lu,\"refused\":%lu,"
                "\"lastRefused\":%d,\"gains\":%lu,\"bad\":%lu,\"timed\":%lu,\"late\":%lu,\"resyncs\":%lu,"
                "\"lead\":%d,\"spread\":%d,\"points\":[", rs.open, (unsigned long)rs.routed,
                (unsigned long)rs.moved, (unsigned long)rs.closed, (unsigned long)rs.refused,
                rs.refused ? rs.last_refused : -1, (unsigned long)rs.gains, (unsigned long)rs.ops_bad,
                (unsigned long)rs.timed, (unsigned long)rs.late, (unsigned long)rs.resyncs, rs.lead, rs.spread);
  if (m <= 0 || m >= cap - n) return 0;
  n += m;
  bool first = true;
  for (int i = 0; i < core::route::kPoints; ++i) {
    const core::route::Point p = core::route::point(i);
    if (!p.open) continue;
    m = sniprintf(out + n, cap - n, "%s[%d,%u,%u,%u,%u]", first ? "" : ",", i, (unsigned)p.owner, (unsigned)p.src,
                  (unsigned)p.dst, (unsigned)p.gain);
    if (m <= 0 || m >= cap - n) return 0;
    n += m;
    first = false;
  }
  m = sniprintf(out + n, cap - n, "]}");
  if (m <= 0 || m >= cap - n) return 0;
  n += m;
  // the DSP library's slots (core/dsp): counts, then each used slot as [slot, owner, "block", state]
  const core::dsp::Stats ds2 = core::dsp::stats();
  m = sniprintf(out + n, cap - n, ",\"dsp\":{\"used\":%d,\"placed\":%lu,\"freed\":%lu,\"refused\":%lu,"
                "\"lastRefused\":%d,\"infos\":%lu,\"bad\":%lu,\"slots\":[", ds2.used, (unsigned long)ds2.placed,
                (unsigned long)ds2.freed, (unsigned long)ds2.refused, ds2.refused ? ds2.last_refused : -1,
                (unsigned long)ds2.infos, (unsigned long)ds2.ops_bad);
  if (m <= 0 || m >= cap - n) return 0;
  n += m;
  first = true;
  for (int i = 0; i < core::dsp::kSlots; ++i) {
    const core::dsp::SlotView v = core::dsp::slot(i);
    if (v.state == core::dsp::kEmpty) continue;
    m = sniprintf(out + n, cap - n, "%s[%d,%u,\"%s\",%d]", first ? "" : ",", i, (unsigned)v.owner, v.block, (int)v.state);
    if (m <= 0 || m >= cap - n) return 0;
    n += m;
    first = false;
  }
  m = sniprintf(out + n, cap - n, "]}");
  if (m <= 0 || m >= cap - n) return 0;
  n += m;
  // shipped code (core/code): counts, the card cache, then each loaded blob as ["NAME", "sha prefix", owner, users, bytes]
  const core::code::Stats cs = core::code::stats();
  m = sniprintf(out + n, cap - n, ",\"code\":{\"offers\":%lu,\"fast\":%lu,\"loaded\":%lu,\"unloaded\":%lu,"
                "\"refused\":%lu,\"lastRefused\":%d,\"cardBlobs\":%lu,\"cardBytes\":%lu,\"blobs\":[",
                (unsigned long)cs.offers, (unsigned long)cs.fast, (unsigned long)cs.loaded, (unsigned long)cs.unloaded,
                (unsigned long)cs.refused, cs.refused ? cs.last_refused : -1, (unsigned long)cs.card_blobs,
                (unsigned long)cs.card_bytes);
  if (m <= 0 || m >= cap - n) return 0;
  n += m;
  core::code::BlobView bv;
  for (int i = 0; core::code::blob(i, &bv); ++i) {
    char hx[17];
    core::lib::hex(bv.hash, 8, hx);
    m = sniprintf(out + n, cap - n, "%s[\"%s\",\"%s\",%d,%d,%lu]", i ? "," : "", bv.name, hx, bv.owner, bv.users,
                  (unsigned long)bv.bytes);
    if (m <= 0 || m >= cap - n) return 0;
    n += m;
  }
  m = sniprintf(out + n, cap - n, "]}");
  return m > 0 && m < cap - n ? n + m : 0;
}

// The console's generic commands (core 3): gen (the router), clock (the Rytm's clock), route (the audio route), proc (the
// slots), dsp / dsp? (the DSP library's slots / the library), rui (the screen; rui=on|off asks the Rytm to open / close
// the page)
bool generic_command(const char* p, char* out, int cap) {
  if (!strcmp(p, "gen")) {
    const core::gen::Stats g = core::gen::stats();
    sniprintf(out, cap, "gen: project \"%s\", messages %lu, dropped %lu (last %02x), hellos %lu, foreign %lu",
              project(), (unsigned long)g.messages, (unsigned long)g.dropped, (unsigned)g.last_dropped,
              (unsigned long)g.hellos, (unsigned long)g.foreign);
    return true;
  }
  if (!strcmp(p, "clock")) {
    const core::clock::Stats c = core::clock::stats();
    sniprintf(out, cap, "clock: transport %d, pattern %d (queued %d), %d.%d BPM, ticks %lu states %lu bad %lu",
              (int)core::clock::transport(), (int)core::clock::pattern(), (int)core::clock::queued(),
              (int)core::clock::bpm(), (int)(core::clock::bpm() * 10.0) % 10, (unsigned long)c.ticks,
              (unsigned long)c.states, (unsigned long)c.bad);
    return true;
  }
  if (!strcmp(p, "kv")) {
    const core::kv::link::StoreStats k = core::kv::link::store_stats();
    const core::locks::Stats l = core::locks::stats();
    sniprintf(out, cap, "kv: project %08lx, %d records, hellos %lu reads %lu writes %lu keeps %lu refused %lu (last %d); "
              "locks: %d sets, ops %lu bad %lu values %lu", (unsigned long)k.project, k.records,
              (unsigned long)k.hellos, (unsigned long)k.reads, (unsigned long)k.writes, (unsigned long)k.keeps,
              (unsigned long)k.refused, k.last_refused, l.sets, (unsigned long)l.ops, (unsigned long)l.bad,
              (unsigned long)l.values);
    return true;
  }
  if (!strcmp(p, "route")) {   // the route's points: "3 SD>MAIN L 0 dB (owner 6)"
    const core::route::Stats r = core::route::stats();
    int n = sniprintf(out, cap, "route: %d open, routed %lu moved %lu closed %lu refused %lu (last %d) gains %lu bad %lu",
                      r.open, (unsigned long)r.routed, (unsigned long)r.moved, (unsigned long)r.closed,
                      (unsigned long)r.refused, r.refused ? r.last_refused : -1, (unsigned long)r.gains,
                      (unsigned long)r.ops_bad);
    for (int i = 0; i < core::route::kPoints && n > 0 && n < cap; ++i) {
      const core::route::Point q = core::route::point(i);
      if (!q.open) continue;
      int w = sniprintf(out + n, cap - n, "; %d %s>%s gain %u (owner %u)", i, core::route::in_name(q.src),
                        core::route::out_name(q.dst), (unsigned)q.gain, (unsigned)q.owner);
      if (w < 0) break;
      n += w;
    }
    return true;
  }
  if (!strcmp(p, "proc")) {
    const core::proc::Stats s = core::proc::stats();
    int n = sniprintf(out, cap, "proc: params %lu dropped %lu blocks %lu, slots", (unsigned long)s.params,
                      (unsigned long)s.dropped, (unsigned long)s.blocks);
    for (int i = 0; i < core::proc::kSlots && n > 0 && n < cap; ++i) {
      const core::proc::Proc* q = core::proc::slot(i);
      int w = sniprintf(out + n, cap - n, " %d:%s", i, q ? q->name() : "-");
      if (w < 0) break;
      n += w;
    }
    return true;
  }
  if (!strcmp(p, "dsp")) {   // the DSP library's slots: "0 DELAY (owner 6)", a fading one "3 FILTER (owner 6, fading)"
    const core::dsp::Stats d = core::dsp::stats();
    int n = sniprintf(out, cap, "dsp: %d used, placed %lu freed %lu refused %lu (last %d) infos %lu bad %lu",
                      d.used, (unsigned long)d.placed, (unsigned long)d.freed, (unsigned long)d.refused,
                      d.refused ? d.last_refused : -1, (unsigned long)d.infos, (unsigned long)d.ops_bad);
    static const char* const kState[] = {"", "", ", fading", ", releasing"};
    for (int i = 0; i < core::dsp::kSlots && n > 0 && n < cap; ++i) {
      const core::dsp::SlotView v = core::dsp::slot(i);
      if (v.state == core::dsp::kEmpty) continue;
      int w = sniprintf(out + n, cap - n, "; %d %s (owner %u%s)", i, v.block, (unsigned)v.owner, kState[v.state & 3]);
      if (w < 0) break;
      n += w;
    }
    return true;
  }
  if (!strcmp(p, "code")) {   // shipped code: "code: 1 loaded ...; TGAIN 47005289 (owner 9, 1 slot)"
    const core::code::Stats c = core::code::stats();
    int n = sniprintf(out, cap, "code: %d loaded, offers %lu fast %lu refused %lu (last %d), card %lu blobs %lu KB",
                      core::code::loaded(), (unsigned long)c.offers, (unsigned long)c.fast, (unsigned long)c.refused,
                      c.refused ? c.last_refused : -1, (unsigned long)c.card_blobs, (unsigned long)(c.card_bytes >> 10));
    core::code::BlobView bv;
    for (int i = 0; n > 0 && n < cap && core::code::blob(i, &bv); ++i) {
      char hx[9];
      core::lib::hex(bv.hash, 4, hx);
      int w = sniprintf(out + n, cap - n, "; %s %s (owner %d, %d slots)", bv.name, hx, bv.owner, bv.users);
      if (w < 0) break;
      n += w;
    }
    return true;
  }
  if (!strcmp(p, "dsp?")) {   // the library: each block and its params
    int n = sniprintf(out, cap, "dsp library:");
    for (int i = 0; i < core::dsp::library_size() && n > 0 && n < cap; ++i) {
      const core::dsp::BlockInfo* b = core::dsp::library(i);
      int w = sniprintf(out + n, cap - n, "%s %s", i ? ";" : "", b->name);
      for (int k = 0; k < b->nparams && w >= 0 && n + w < cap; ++k)
        w += sniprintf(out + n + w, cap - n - w, " %d:%s", k, b->params[k].name);
      if (w < 0) break;
      n += w;
    }
    return true;
  }
  if (!strcmp(p, "usba=thru") || !strcmp(p, "usba=off")) {
    core::devmidi::set_thru(p[5] == 't');
    sniprintf(out, cap, "usba: thru %s", core::devmidi::thru() ? "on" : "off");
    return true;
  }
  if (!strcmp(p, "usba")) {
    const core::devmidi::Stats d = core::devmidi::stats();
    int n = sniprintf(out, cap, "usba: thru %s, messages %lu forwarded %lu to slots %lu released %lu ignored %lu, devices",
                      core::devmidi::thru() ? "on" : "off", (unsigned long)d.messages, (unsigned long)d.forwarded,
                      (unsigned long)d.to_slots, (unsigned long)d.released, (unsigned long)d.ignored);
    for (int i = 0; i < core::devmidi::kSrcs && n > 0 && n < cap; ++i) {
      int w = sniprintf(out + n, cap - n, " %d:%s", i + 1, core::devmidi::present(i) ? core::devmidi::name(i) : "-");
      if (w < 0) break;
      n += w;
    }
    return true;
  }
  if (!strcmp(p, "browse")) {
    const core::browse::Stats b = core::browse::stats();
    sniprintf(out, cap, "browse: asks %lu, listings %lu (%lu entries, %lu messages), superseded %lu, folders %d, left out "
              "%lu, errors %lu", (unsigned long)b.asks, (unsigned long)b.listings, (unsigned long)b.entries,
              (unsigned long)b.messages, (unsigned long)b.superseded, b.folders, (unsigned long)b.left_out,
              (unsigned long)b.errors);
    return true;
  }
  if (!strcmp(p, "rui=on") || !strcmp(p, "rui=off")) {
    core::screen::request(p[5] == 'n');
    sniprintf(out, cap, "rui: page %s requested%s", p[5] == 'n' ? "open" : "close",
              core::screen::client() ? "" : " (no client: the Rytm's page closes again)");
    return true;
  }
  if (!strcmp(p, "rui")) {
    const core::screen::Stats s = core::screen::stats();
    sniprintf(out, cap, "rui: %s, client %s, caps %d, hellos %lu, opens %lu closes %lu, frames %lu tiles %lu bytes %lu "
              "alive %lu, keys %lu encs %lu, splashes %lu popups %lu, bad %lu",
              core::screen::open() ? "open" : "closed", core::screen::client() ? core::screen::client()->name : "none",
              core::screen::caps(), (unsigned long)s.hellos, (unsigned long)s.opens, (unsigned long)s.closes,
              (unsigned long)s.frames, (unsigned long)s.tiles, (unsigned long)s.bytes, (unsigned long)s.keepalives,
              (unsigned long)s.keys, (unsigned long)s.encs, (unsigned long)s.splashes,
              (unsigned long)core::screen::popups(), (unsigned long)s.bad);
    return true;
  }
  return false;
}

}  // namespace bridge

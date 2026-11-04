// pi/engine/bridge.cpp - see bridge.h: the calls the Circle-side files make, onto core/generic, core/screen,
// core/clock, core/proc, core/kv, core/browse and core/devmidi. Hosted (the core's flags).
#include "bridge.h"

#include <string.h>

#include "browse/browse.h"
#include "clock/clock.h"
#include "devmidi/devmidi.h"
#include "device/device.h"
#include "fmt/fmt.h"
#include "generic/generic.h"
#include "hooks.h"
#include "kv/link.h"
#include "msamp/cardq.h"
#include "platform.h"
#include "proc/proc.h"
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
bool begin(void (*to_host)(const uint8_t*, int)) {
  core::hooks.to_rytm = to_host;
  core::hooks.device_restart = device_restart;
  s_card = core::gen::begin();
  core::kv::link::attach_generic();   // 51 STORE records (the card core) and the Pi-side locks (core/kv, core/locks)
  core::kv::link::on_name(name_changed);
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

// /status's generic members: the router's counts, the Rytm's clock, the processor slots, the remote screen
int generic_json(char* out, int cap) {
  const core::gen::Stats g = core::gen::stats();
  const core::clock::Stats c = core::clock::stats();
  const core::proc::Stats p = core::proc::stats();
  const core::screen::Stats s = core::screen::stats();
  const core::browse::Stats bs = core::browse::stats();
  const core::devmidi::Stats ds = core::devmidi::stats();
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
  char slots[96];
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
                    "\"generic\":{\"project\":\"%s\",\"projectId\":%lu,\"messages\":%lu,\"dropped\":%lu,\"lastDropped\":%u,\"hellos\":%lu},"
                    "\"clock\":{\"transport\":%d,\"pattern\":%d,\"bpm10\":%d,\"ticks\":%lu,\"states\":%lu,\"bad\":%lu},"
                    "\"proc\":{\"slots\":[%s],\"params\":%lu,\"dropped\":%lu,\"blocks\":%lu},"
                    "\"screen\":{\"open\":%s,\"client\":%s%s%s,\"caps\":%d,\"hellos\":%lu,\"opens\":%lu,\"frames\":%lu,"
                    "\"popups\":%lu,\"bad\":%lu},"
                    "\"devmidi\":{\"devices\":[%s],\"thru\":%s,\"messages\":%lu,\"forwarded\":%lu,\"toSlots\":%lu},"
                    "\"browse\":{\"asks\":%lu,\"listings\":%lu,\"folders\":%d,\"leftOut\":%lu,\"errors\":%lu}",
                    pname, (unsigned long)pid, (unsigned long)g.messages, (unsigned long)g.dropped, (unsigned)g.last_dropped,
                    (unsigned long)g.hellos, (int)core::clock::transport(), (int)core::clock::pattern(),
                    (int)(core::clock::bpm() * 10.0 + 0.5), (unsigned long)c.ticks, (unsigned long)c.states,
                    (unsigned long)c.bad, slots, (unsigned long)p.params, (unsigned long)p.dropped,
                    (unsigned long)p.blocks, core::screen::open() ? "true" : "false",
                    core::screen::client() ? "\"" : "", core::screen::client() ? core::screen::client()->name : "null",
                    core::screen::client() ? "\"" : "", core::screen::caps(), (unsigned long)core::screen::hellos(),
                    (unsigned long)s.opens, (unsigned long)s.frames, (unsigned long)core::screen::popups(),
                    (unsigned long)s.bad, devs, core::devmidi::thru() ? "true" : "false", (unsigned long)ds.messages,
                    (unsigned long)ds.forwarded, (unsigned long)ds.to_slots, (unsigned long)bs.asks,
                    (unsigned long)bs.listings, bs.folders, (unsigned long)bs.left_out, (unsigned long)bs.errors);
  return n > 0 && n < cap ? n : 0;
}

// The console's generic commands (core 3): gen (the router), clock (the Rytm's clock), proc (the slots), rui (the
// screen; rui=on|off asks the Rytm to open / close the page)
bool generic_command(const char* p, char* out, int cap) {
  if (!strcmp(p, "gen")) {
    const core::gen::Stats g = core::gen::stats();
    sniprintf(out, cap, "gen: project \"%s\", messages %lu, dropped %lu (last %02x), hellos %lu", project(),
              (unsigned long)g.messages, (unsigned long)g.dropped, (unsigned)g.last_dropped, (unsigned long)g.hellos);
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

// pi/engine/bridge.cpp - see bridge.h. Hosted (the core's flags).
#include "bridge.h"

#include <math.h>
#include <string.h>

#include "engine.h"
#include "lfo.h"
#include "heap_buffers.h"
#include "notes.h"
#include "slots.h"
#include "ui/extra.h"
#include "ui/remote.h"

namespace bridge {

static_assert(kVoices == core::kVoices && kBlock == core::kBlock && kMachines == core::kMachines, "bridge sizes");

static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// path written whole or not at all: a power loss mid-write leaves only path.tmp (removed on the next boot), never
// a short file that exists() would then keep. fill(f) writes the content and is false on a short write. Few, large
// writes: the Pi's File::write syncs on every call.
template <typename Fill>
static bool write_whole(core::Fs& fs, const char* path, Fill fill) {
  char tmp[64];
  sniprintf(tmp, sizeof tmp, "%s.tmp", path);
  core::File f;
  if (!f.open(tmp, core::kWrite)) return false;
  bool ok = fill(f);
  f.close();
  if (ok && fs.rename(tmp, path)) return true;
  fs.remove(tmp);
  return false;
}

bool selftest_content() {
  static const char kWav[] = "/Samples/pi-test/sine440.wav", kPool[] = "/.rytm/pools/_default.txt";
  core::Fs fs;
  if (!fs.begin()) return false;
  fs.remove("/Samples/pi-test/sine440.wav.tmp");   // a write cut short by a power loss
  fs.remove("/.rytm/pools/_default.txt.tmp");
  if (!fs.exists(kWav)) {
    fs.mkdir("/Samples/pi-test");
    bool ok = write_whole(fs, kWav, [](core::File& f) {
      const uint32_t frames = 96000;
      uint8_t h[44];
      memcpy(h, "RIFF", 4); put32(h + 4, 36 + frames * 2); memcpy(h + 8, "WAVEfmt ", 8);
      put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 1); put32(h + 24, 48000); put32(h + 28, 96000);
      put16(h + 32, 2); put16(h + 34, 16); memcpy(h + 36, "data", 4); put32(h + 40, frames * 2);
      if (f.write(h, sizeof h) != sizeof h) return false;
      static int16_t buf[9600];   // 0.2 s a write: 10 writes (static: once, at boot)
      for (uint32_t i = 0; i < frames; i += 9600) {
        for (int k = 0; k < 9600; ++k) buf[k] = (int16_t)(12000.0f * sinf(6.2831853f * 440.0f * (i + k) / 48000.0f));
        if (f.write(buf, sizeof buf) != sizeof buf) return false;
      }
      return true;
    });
    core::log(ok ? "selftest: wrote %s" : "selftest: could not write %s", kWav);
    if (!ok) return false;
  }
  if (!fs.exists("/.rytm/project") && !fs.exists(kPool)) {
    fs.mkdir("/.rytm/pools");
    bool ok = write_whole(fs, kPool, [](core::File& f) { return f.write("pi-test/sine440.wav\n", 20) == 20; });
    core::log(ok ? "selftest: pool _default slot 1 = pi-test/sine440.wav" : "selftest: could not write %s", kPool);
  }
  return true;
}

#ifdef CORE_HEAP_BUFFERS
unsigned long heap_bytes() { return core::heap_buffers_bytes(); }
void heap_place(void* mem) { core::heap_buffers_place(mem); }
#else
unsigned long heap_bytes() { return 0; }
void heap_place(void*) {}
#endif

bool begin(void (*to_host)(const uint8_t*, int)) {
  core::hooks.to_rytm = to_host;
  return core::engine_begin();
}
void sysex(const uint8_t* d, int len) { core::engine_sysex(d, len); }
uint32_t render(int16_t (*block)[kVoices], int v0, int v1, uint64_t* mach) {
  return core::render_voices(block, kBlock, 0, v0, v1, mach);
}
bool idle_prepare() { return core::idle_prepare(); }
bool idle_save() { return core::idle_save(); }
void idle_stream() { core::idle_stream(); }
void tick() {
  if (!core::hooks.slot_ask) core::slots_1d(5000);   // the user machines (core/user/um.cpp) send their own 1D
  core::ui::remote_tick(core::now_ms());   // nothing while the Rytm's remote page is closed
  core::lfo::tick(core::now_us());        // the Sidecar's LFOs (core/lfo.h): once a millisecond at most
}
void lfo_midi(uint8_t st, uint8_t d1, uint8_t d2) { core::lfo::midi_in(st, d1, d2); }
bool lfo_command(const char* p, char* out, int cap) { return core::lfo::command(p, out, cap); }
int lfo_json(char* out, int cap) { return core::lfo::json(out, cap); }
void rui_status(int ob_running, uint32_t underruns, int c1, int c2, int c3, const RuiMore* more) {
  core::ui::RemoteStatus s = {};
  s.ob = (int8_t)ob_running, s.underruns = underruns, s.cpu[0] = (int8_t)c1, s.cpu[1] = (int8_t)c2, s.cpu[2] = (int8_t)c3;
  if (more) {
    s.rec = (uint8_t)more->rec, s.rec_secs = more->rec_secs, s.eth = (uint8_t)more->eth, s.wifi = (uint8_t)more->wifi;
    memcpy(s.ip, more->ip, sizeof s.ip);
    memcpy(s.user, more->user, sizeof s.user);
  }
  core::ui::remote_status(s);
}
void rui_rec(const RuiRec& r) {
  core::ui::RecInfo i = {};
  i.state = (uint8_t)r.state, i.secs = r.secs, i.free_mb = r.free_mb, i.total_mb = r.total_mb, i.need_kbps = r.need_kbps;
  i.in_mask = (uint16_t)r.in_mask;
  i.ch_select = r.ch_select != 0;
  memcpy(i.take, r.take, sizeof i.take);
  memcpy(i.error, r.error, sizeof i.error);
  core::ui::remote_rec(i);
}
int rui_rec_take() { return core::ui::remote_rec_take(); }
unsigned rui_rec_toggles() { return core::ui::remote_rec_toggles(); }
void rui_play(const RuiPlay& p) {
  core::ui::PlayInfo i = {};
  i.state = (uint8_t)p.state, i.level = (uint8_t)p.level, i.ch_i = (uint8_t)p.ch_i, i.channels = (uint8_t)p.channels;
  i.take_i = (uint8_t)p.take_i, i.takes = (uint8_t)(p.takes > 255 ? 255 : p.takes), i.pos_ms = p.pos_ms, i.len_ms = p.len_ms;
  memcpy(i.take, p.take, sizeof i.take);
  memcpy(i.channel, p.channel, sizeof i.channel);
  memcpy(i.error, p.error, sizeof i.error);
  core::ui::remote_play(i);
}
bool rui_play_take(int* toggle, int* channel, int* take, int* level) {
  core::ui::PlayInput in;
  bool any = core::ui::remote_play_take(&in);
  *toggle = in.toggle, *channel = in.channel, *take = in.take, *level = in.level;
  return any;
}
bool tap_on() { return core::ui::tap_on(); }
void tap_in(const int32_t* f, int n, int stride) { core::ui::tap_in(f, n, stride); }
bool fxin_command(const char* p, char* out, int cap) { return core::fxin::command(p, out, cap); }
int fxin_json(char* out, int cap) { return core::fxin::json(out, cap); }

bool rui_command(const char* p) {
  if (strcmp(p, "rui=on") == 0 || strcmp(p, "rui=off") == 0) {
    core::ui::remote_request(p[5] == 'n');
    core::log("rui: page %s requested", p[5] == 'n' ? "open" : "close");
    return true;
  }
  if (strncmp(p, "rui=srclog=", 11) == 0) {   // 1027: each 29 / 2A and SRC view commit logged
    core::ui::remote_src_log(p[11] == '1');
    core::log("rui: SRC view log %s", p[11] == '1' ? "on" : "off");
    return true;
  }
  if (strcmp(p, "rui") != 0) return false;
  const core::ui::RemoteStats& s = core::ui::remote_stats();
  core::log("rui: %s, page %s%u, opens %lu closes %lu, frames %lu tiles %lu rects %lu bytes %lu alive %lu, keys %lu encs %lu "
            "edits %lu, widgets %lu, bad %lu, draw max %lu us mean %lu us",
            core::ui::remote_open() ? "open" : "closed", s.page == 1 ? "voice " : s.page ? "SYSTEM" : "SIDECAR", s.page == 1 ? s.voice + 1u : 0u,
            (unsigned long)s.opens, (unsigned long)s.closes, (unsigned long)s.frames, (unsigned long)s.tiles,
            (unsigned long)s.rects, (unsigned long)s.bytes, (unsigned long)s.keepalives, (unsigned long)s.keys,
            (unsigned long)s.encs, (unsigned long)s.edits, (unsigned long)s.widgets, (unsigned long)s.bad,
            (unsigned long)s.draw_us_max, (unsigned long)(s.frames ? s.draw_us_sum / s.frames : 0));
  // the Rytm's 33s (rui1025+): how many came and each track's voice byte (7f not a Sidecar machine, -- none yet): once
  // one came the voice pages (JUNO, 6-OP FM graphics ...) follow these alone
  static const char kHex[] = "0123456789abcdef";
  char t[12 * 3 + 1];
  for (int i = 0; i < 12; ++i) {
    int vb = core::ui::remote_track_machine(i);
    t[3 * i] = vb < 0 ? '-' : kHex[(vb >> 4) & 15], t[3 * i + 1] = vb < 0 ? '-' : kHex[vb & 15], t[3 * i + 2] = ' ';
  }
  t[12 * 3] = 0;
  core::log("rui: machines (33) %lu, tracks %s", (unsigned long)s.machines, t);
  core::log("rui: SRC view 29s %lu, 2As %lu, frames %lu", (unsigned long)s.src_views, (unsigned long)s.src_vals,
            (unsigned long)s.src_frames);
  return true;
}
// core::voices_init resets daisy, fm and user but not the dsps, dd and juno flags (zero at boot): the boot state
// back for every voice, so the bench's last machine (dd) does not stay on voice 0. Pi-side, the core is shared.
static void voices_boot_state() {
  core::voices_init();
  for (int i = 0; i < kVoices; ++i) core::v[i].dsps = core::v[i].grain = core::v[i].dd = core::v[i].juno = false;
  core::ui::remote_forget_voices();   // the bench's reports were ours, not the Rytm's: no voice page from them
}
void reinit_voices() { voices_boot_state(); }

static void bench_out(const uint8_t* d, int n) { core::engine_sysex(d, n); }

uint64_t bench(const char* machine, uint32_t* worst_block, int* peak) {
  core::NoteAdapter na(bench_out);
  na.SetMachine(0, machine);
  na.Channel(0x90, 48, 100);
  while (core::idle_prepare()) {}   // DSP SAMPLE: its head read before it plays
  static int16_t blk[kBlock][kVoices];
  uint64_t mach[kMachines] = {0}, sum = 0;
  uint32_t worst = 0;
  int pk = 0;
  for (int b = 0; b < 4000; ++b) {
    core::refill_neediest(4 * kBlock);
    uint32_t t = core::render_voices(blk, kBlock, 0, 0, 1, mach);
    sum += t;
    if (t > worst) worst = t;
    for (int s = 0; s < kBlock; ++s) pk = blk[s][0] > pk ? blk[s][0] : -blk[s][0] > pk ? -blk[s][0] : pk;
    if (b == 2000) na.Channel(0x90, 55, 100);   // a second note halfway (a retrigger, as a pattern would)
  }
  na.Channel(0x80, 55, 0);
  voices_boot_state();
  *worst_block = worst;
  if (peak) *peak = pk;
  return sum;
}
const char* machine_name(int vi) {
  const core::V& x = core::v[vi & 7];
  return x.user >= 0 ? "USER" : x.fm ? "FM" : x.juno ? "JUNO" : x.grain ? "GRAIN" : x.dsps ? "DSPS" : x.daisy ? "DAISY" : x.dd ? "DD" : "PLAITS";
}
const char* stat_name(int k) {
  static const char* const n[kMachines] = {"fm", "juno", "dsps", "daisy", "dd", "plaits", "user"};
  return k >= 0 && k < kMachines ? n[k] : "?";
}
int card_folders() { return core::card_ok ? core::cards.folders() : 0; }
const char* project() { return core::project; }
int pool_slots() { return core::npool; }
uint32_t to_host_dropped() { return core::to_rytm_dropped(); }

}  // namespace bridge

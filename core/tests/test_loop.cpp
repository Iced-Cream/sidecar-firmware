// core/tests/test_loop.cpp - the master looper (core/loop, spec-050 section 3): the MAIN ring and its frame map (Task 19),
// the grabs (20), playback (21), resample / stop / auto-mute / LIVE VIA (22), calibration and console (25).
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "core.h"
#include "hooks.h"
#include "mod/mod.h"
#include "mod/rytm_out.h"
#include "mod/target.h"
#include "fmt/fmt.h"
#include "msamp/cardq.h"
#include "song/song.h"
#include "loop/loop.h"
#include "loop/loop_ring.h"

using namespace core;

// ---- Task 19: the ring
static const uint32_t kCap = 1u << 16;   // frames of a test ring (the Pi's is 4,194,304)

// a source of audio: frame k's L = k + 1 (never 0, so silence shows), R = -(k + 1)
static void blocks(loop::Ring& r, uint64_t from, uint64_t to, int jit, uint64_t arrive0, uint64_t lost_at = ~0ull, unsigned lost = 0) {
  uint64_t k = from;
  uint32_t rng = 7;
  while (k < to) {
    unsigned l = k == lost_at ? lost : 0;   // `lost` frames of silence come before this block
    k += l;
    int32_t b[14];
    for (int i = 0; i < 7; ++i) b[2 * i] = (int32_t)(k + i + 1), b[2 * i + 1] = -(int32_t)(k + i + 1);
    rng = rng * 1103515245u + 12345u;
    uint64_t arr = arrive0 + k + 6 + (jit ? (rng >> 8) % (uint32_t)(jit + 1) : 0);   // the block's last frame arrives (+ jitter)
    r.write(b, 7, arr, l);
    k += 7;
  }
}

static void test_ring_gap_cost() {
  // H1: a stream that comes back after hours writes no silence: a pause longer than the ring touches no memory but the block
  loop::Ring r;
  std::vector<int32_t> mem(2 * (size_t)kCap, 0x55555555);
  r.init(mem.data(), kCap);
  int32_t b[14] = {};
  for (int i = 0; i < 7; ++i) b[2 * i] = 1000 + i, b[2 * i + 1] = 1000 + i;
  r.write(b, 7, 6 + 500, 0);
  uint64_t w0 = r.written();
  r.write(b, 7, 6 + 500 + 7 + 10 * 3600 * 48000ull, 0);   // ten hours later
  size_t touched = 0;
  for (int32_t x : mem)
    if (x != 0x55555555) ++touched;
  CHECK(touched <= 2 * 14 + 2);   // the two blocks, nothing else (the old code wrote the whole ring)
  CHECK(r.written() > w0 + 7 + (uint64_t)kCap);
  std::vector<int32_t> c(2 * 70);
  uint64_t end = r.written();
  CHECK(r.copy_idx(end - 70, 70, c.data()) == 70 && c[0] == 0 && c[2 * 62] == 0 && c[2 * 63] == 1000);
  printf("  ring: a pause of ten hours costs no stores\n");
}

static void test_ring() {
  static int32_t mem[2 * kCap];
  loop::Ring r;
  r.init(mem, kCap);
  // wrap_keeps_last_87s (scaled): write 1.5 rings' worth; a frame older than the ring is gone, a recent one is there
  blocks(r, 0, kCap + kCap / 2, 0, 1000);
  uint32_t idx;
  uint64_t w = r.written();
  CHECK(w >= kCap + kCap / 2);
  CHECK(!r.frame_at(1000 + 100, &idx));                 // host frame of ring frame ~100: overwritten
  CHECK(r.frame_at(1000 + w - kCap / 2, &idx));          // half a ring ago: there
  CHECK(mem[2 * idx] == (int32_t)(w - kCap / 2 + 1));
  // map_ignores_jitter: arrivals jittered +0..12 frames: frame_at exact to a frame
  loop::Ring j;
  static int32_t mem2[2 * kCap];
  j.init(mem2, kCap);
  blocks(j, 0, 20000, 12, 5000);
  for (uint64_t f : {6000ull, 9999ull, 15000ull}) {
    CHECK(j.frame_at(5000 + f, &idx));
    CHECK(mem2[2 * idx] == (int32_t)(f + 1));
  }
  // gap_written_as_silence: 70 lost frames are zeros at those indices, the map stays linear, gap_in says so
  loop::Ring g;
  static int32_t mem3[2 * kCap];
  g.init(mem3, kCap);
  blocks(g, 0, 1400, 0, 100, 693, 70);   // 70 frames lost before the block at 693 + 70 = 763
  CHECK(g.written() == 1400 + 70 - 70 + 0 || g.written() >= 1400);
  {   // zeros at 693..762 for every reader (not written: stale memory sits there), linear again after, the same map
    std::vector<int32_t> c(2 * 80);
    CHECK(g.copy_idx(690, 80, c.data()) == 80);
    CHECK(c[2 * 2] == 693 && c[2 * 3] == 0 && c[2 * 72] == 0 && c[2 * 73] == 764);
    CHECK(g.copy_idx(763, 10, c.data()) == 10 && c[0] == 764);
    int32_t l, r;
    g.frame(700, &l, &r);
    CHECK(l == 0 && r == 0);
    g.frame(1000, &l, &r);
    CHECK(l == 1001);
  }
  CHECK(g.gap_in(100 + 650, 200) && !g.gap_in(100 + 100, 100) && !g.gap_in(100 + 900, 200));
  // stream_restart_keeps_map_linear: the Rytm's stream stops for 3 s (the host's frames go on) and comes back
  loop::Ring q;
  static int32_t mem4[2 * (1 << 18)];
  q.init(mem4, 1 << 18);
  blocks(q, 0, 7000, 0, 500);                 // index k arrives at 500 + k + 6 (offset 500)
  uint64_t before = q.written();
  {
    uint64_t k = before;
    for (; k < before + 7000; k += 7) {       // ... then the next block arrives 144000 frames later
      int32_t b[14];
      for (int i = 0; i < 7; ++i) b[2 * i] = (int32_t)(k + 144000 + i + 1), b[2 * i + 1] = 0;
      q.write(b, 7, 500 + k + 144000 + 6, 0);
    }
  }
  CHECK(q.frame_at(500 + 3000, &idx) && mem4[2 * idx] == 3001);                // before the pause: as it was
  CHECK(q.frame_at(500 + before + 144000 + 70, &idx) && mem4[2 * idx] == (int32_t)(before + 144000 + 71));   // after: the same line
  CHECK(q.gap_in(500 + before + 100, 100) && !q.gap_in(500 + 3000, 100));
  {   // H1: the 144000-frame pause was not written (bounded work in the completion): stale memory there, zeros for readers
    std::vector<int32_t> c(2 * 64);
    CHECK(q.copy_idx(before + 1000, 64, c.data()) == 64 && c[0] == 0 && c[2 * 63] == 0);
  }
  // records_during_playback: no pause exists: writes always advance
  uint64_t w0 = g.written();
  blocks(g, w0, w0 + 700, 0, 100);
  CHECK(g.written() == w0 + 700);
  // copy: a stretch by host frame, and refused once overwritten
  std::vector<int32_t> out(2 * 50);
  CHECK(j.copy(5000 + 9000, 50, out.data()) == 50 && out[0] == 9001 && out[2 * 49] == 9050);
  CHECK(j.copy(5000 + 30, 50, out.data()) == 50);   // (the ring is far from full: there)
  printf("  ring: ok\n");
}

// ---- the looper's fixture: a fake sequencer clock (origin, per16, lat), the ring fed as the Rytm's IN would, ticks every ms
static const uint32_t kRingFrames = 4194304;
static const int64_t kD = 1000;                 // host frame = ring index + kD (a block's last frame arrives that late)
static uint64_t g_now = 0;                      // seq::frames()
static double g_origin = 10000, g_per16 = 6000;   // 120 BPM
static int g_lat = 384;
static bool g_beats = true;
static uint16_t g_muted = 0;
static uint64_t g_fed = 0;
static int32_t (*g_sig)(uint64_t) = nullptr;   // the source's L at ring index k (null: the ramp k + 1)                      // ring indices written so far
static uint32_t g_us = 1000000;
static uint64_t g_fake = 10000000;   // core's now_us / now_ms (the Rytm-out sender's spacing)
static std::vector<uint8_t> g_store;
static int32_t* g_ring = nullptr;
static bool env_beat(double* o, double* p, int* l) {
  *l = g_lat;
  if (!g_beats) return false;
  *o = g_origin, *p = g_per16;
  return true;
}
static uint64_t env_frames() { return g_now; }
static uint16_t env_muted() { return g_muted; }
static double bar() { return 16 * g_per16; }
static double line(double k, double g = 0) { return g_origin + g_lat + k * (g ? g : bar()); }   // when bar line k's audio arrives
static void feed() {   // the ring up to host frame g_now: block b's last frame (index k + 6) arrives at k + 6 + kD
  while ((int64_t)(g_fed + 6 + kD) <= (int64_t)g_now) {
    int32_t b[14];
    for (int i = 0; i < 7; ++i) b[2 * i] = g_sig ? g_sig(g_fed + i) : (int32_t)(g_fed + i + 1), b[2 * i + 1] = -b[2 * i];
    loop::in_block(b, 7, g_fed + 6 + kD, 0);
    g_fed += 7;
  }
}
static void fresh(uint64_t store_bytes = 48u << 20) {
  static loop::Env env;
  env.beat_map = env_beat, env.frames = env_frames, env.muted = env_muted;
  loop::set_env(&env);
  if (!g_ring) g_ring = new int32_t[2 * (size_t)kRingFrames];
  g_store.assign(store_bytes, 0);
  loop::init(g_ring, kRingFrames, g_store.data(), store_bytes);
  loop::reset();
  host_fake_time(true, g_fake);
  g_sig = nullptr, g_now = 0, g_fed = 0, g_us = 1000000, g_origin = 10000, g_per16 = 6000, g_lat = 384, g_beats = true, g_muted = 0;
}
static void to(uint64_t h) {   // the clock to host frame h, a tick a ms on the way
  while (g_now + 48 <= h) {
    g_now += 48, g_us += 1000, g_fake += 1000;
    host_fake_time(true, g_fake);
    mod::rytm_out_tick(now_ms());
    feed();
    loop::tick(g_us);
  }
  g_now = h;
  feed();
  loop::tick(g_us);
}
static void jump(uint64_t h) {   // the clock on without a tick (the control loop busy elsewhere)
  g_now = h;
  feed();
}
static void run_ms(int ms) { to(g_now + 48u * ms); }
static int32_t srcval(double host_frame) { return (int32_t)((int64_t)host_frame - kD + 1); }   // the ring's L at that arrival frame
// what out_mix puts on F[.][0] for frame h (a block of 12 starting at h, frame 0)
static int32_t played_at(uint64_t h) {
  int32_t F[12][12] = {};
  loop::out_mix(F, 12, h);
  return F[0][0];
}

// ---- Task 20: grabs
static void test_grabs() {
  // grab_ends_on_last_bar_line: pressed a third into a bar, forty bars in
  fresh();
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(5);
  CHECK(loop::grabs() == 1);
  const loop::Grab* g = loop::grab_at(0);
  CHECK(g->frames == (uint32_t)bar() && g->half_bars == loop::k1);
  CHECK(g->start == (uint64_t)line(39));   // bars [39, 40): ends on the last bar line (40)
  run_ms(50);
  CHECK(g->copied && g->pcm[0] == srcval(line(39)) && g->pcm[2 * (g->frames - 1)] == srcval(line(40) - 1));
  CHECK(g->pcm[-2 * loop::kPre] == srcval(line(39) - loop::kPre));
  CHECK(g->bpm > 119.9f && g->bpm < 120.1f);

  // starts_at_next_bar_line: sample 0 is rendered at origin + 41 x bar (lat early), nothing before it
  uint64_t s0 = (uint64_t)(g_origin + 41 * bar());
  CHECK(loop::state() == loop::kWait);
  for (uint64_t h = s0 - 24; h < s0; h += 12) CHECK(played_at(h) == 0);
  CHECK(played_at(s0 + 63) == g->pcm[2 * 63]);   // (a start fades in over 64 frames: unity from the 64th)
  to(s0 + 200);
  CHECK(loop::state() == loop::kPlaying);

  // half_bar_grid: the grid is the half bar: the grab ends on the last half-bar line, 1 half bar long
  fresh();
  to((uint64_t)(line(40.5) + bar() / 8));
  CHECK(loop::grab(loop::kHalf) == loop::kOk);
  run_ms(60);
  g = loop::grab_at(0);
  CHECK(loop::grabs() == 1 && g->frames == (uint32_t)(bar() / 2) && g->start == (uint64_t)line(40));
  CHECK(played_at((uint64_t)(g_origin + 41 * bar()) + 63) == g->pcm[2 * 63]);   // the next half-bar line (E = 81 half bars, +1)

  // grab_waits_for_bar_audio: 2 ms after a bar line the line's audio has not come yet: wait, then take that bar
  fresh();
  to((uint64_t)(g_origin + 40 * bar()) + 96);   // 2 ms after the render-domain line; its audio is lat (8 ms) later
  CHECK(loop::grab(loop::k1) == loop::kOk);
  CHECK(loop::pending() && loop::grabs() == 0);
  run_ms(4);
  CHECK(loop::pending() && loop::grabs() == 0);
  run_ms(8);
  CHECK(!loop::pending() && loop::grabs() == 1);
  CHECK(loop::grab_at(0)->start == (uint64_t)line(39));   // bars 39: ends on line 40 itself

  // grab_refused_stopped
  fresh();
  to((uint64_t)line(40));
  g_beats = false;
  CHECK(loop::grab(loop::k1) == loop::kErrStopped);
  char t[48];
  CHECK(std::string(loop::err_text(loop::kErrStopped, loop::k1, t, sizeof t)) == "PLAY FIRST");
  CHECK(loop::stats().refused == 1);

  // grab_refused_when_ring_too_short: the press-time rule of spec 3.3, 16 bars at 45 BPM (bar 256000 frames, 5.33 s)
  g_per16 = 16000;
  fresh();
  g_per16 = 16000;
  to((uint64_t)(line(17) + bar() * 3 / 4));
  CHECK(loop::grab(loop::k16) == loop::kErrTooLong);
  CHECK(std::string(loop::err_text(loop::kErrTooLong, loop::k16, t, sizeof t)) == "16 BARS NEEDS 47 BPM");
  CHECK(loop::grab(loop::k8) == loop::kOk);   // 8 bars at 45 BPM: fine
  fresh();
  g_per16 = 16000;
  to((uint64_t)(line(17) + bar() / 16));
  CHECK(loop::grab(loop::k16) == loop::kOk);   // pressed early in the bar: accepted
  fresh();
  g_per16 = 6000 * 120 / 48;   // 48 BPM
  to((uint64_t)(line(17) + bar() * 99 / 100));
  CHECK(loop::grab(loop::k16) == loop::kOk);   // any point: accepted
  fresh();
  g_per16 = 6000 * 120 / 30;   // 8 bars at 30 BPM
  to((uint64_t)(line(17) + bar() * 99 / 100));
  CHECK(loop::grab(loop::k8) == loop::kOk);
  printf("  grabs: bar line, start, half bar, waits, refusals ok\n");
}

static void test_store() {
  // store_drops_oldest: a store of 4 MB holds two 1-bar grabs (768 KB each), the 9th grab drops the oldest, at most 8 held
  fresh(8u << 20);
  to((uint64_t)line(10));
  uint32_t first_id = 0;
  for (int i = 0; i < 10; ++i) {
    CHECK(loop::grab(loop::k1) == loop::kOk);
    run_ms(40);
    if (i == 0) first_id = loop::grab_at(0)->id;
    to((uint64_t)(g_origin + (11 + i) * bar() + bar() / 2 + g_lat));
  }
  CHECK(loop::grabs() == 8);                              // 8 x 768 KB fits 8 MB: the 9th and 10th dropped the oldest
  CHECK(loop::grab_at(7)->id == first_id + 2);
  CHECK(loop::stats().dropped == 2);
  // a store that holds fewer: 2 MB, the third 1-bar grab (768 KB) drops the first
  fresh(2u << 20);
  to((uint64_t)line(10));
  for (int i = 0; i < 3; ++i) {
    CHECK(loop::grab(loop::k1) == loop::kOk);
    run_ms(40);
    to((uint64_t)(g_origin + (11 + i) * bar() + bar() / 2 + g_lat));
  }
  CHECK(loop::grabs() == 2 && loop::stats().dropped == 1);
  // grab_bigger_than_budget_refused: a 2-bar grab (1.5 MB) into 1 MB; nothing dropped for it
  fresh(1u << 20);
  to((uint64_t)line(10));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(40);
  CHECK(loop::grab(loop::k2) == loop::kErrNoRoom);
  char t[48];
  CHECK(std::string(loop::err_text(loop::kErrNoRoom, loop::k2, t, sizeof t)) == "NO ROOM");
  CHECK(loop::grabs() == 1 && loop::stats().dropped == 0);

  // copy_in_pieces: a 16-bar grab at 120 BPM (12.3 MB), never more than 256 KB in a tick
  fresh(32u << 20);
  to((uint64_t)(line(40) + bar() / 4));
  CHECK(loop::grab(loop::k16) == loop::kOk);
  run_ms(2);
  CHECK(loop::grabs() == 1 && !loop::grab_at(0)->copied);
  run_ms(100);
  CHECK(loop::grab_at(0)->copied && loop::stats().worst_copy_bytes <= loop::kPieceBytes && loop::stats().worst_copy_bytes > 0);
  CHECK(loop::stats().copies == 1);

  // plays_from_ring_until_copied: the frames are the ring's while the copy runs, the same after it
  fresh(32u << 20);
  to((uint64_t)(line(40) + bar() / 4));
  CHECK(loop::grab(loop::k16) == loop::kOk);
  const loop::Grab* g = loop::grab_at(0);
  uint64_t s0 = (uint64_t)(g_origin + 41 * bar());
  CHECK(g && !g->copied);
  jump(s0 + 10);
  CHECK(!g->copied);
  int32_t early = played_at(s0 + 200);
  CHECK(early == srcval(line(40 - 16) + 200));   // the ring's own frames (the grab starts 16 bars back)
  run_ms(120);
  CHECK(g->copied && played_at(s0 + 200) == early);

  // copy_stays_ahead_of_writer: at 47 BPM a 16-bar grab barely fits; a tick a second (the writer 1.5 x the copy) overtakes it
  fresh(48u << 20);
  g_per16 = 6000 * 120 / 47;
  to((uint64_t)(line(20) + bar() / 16));
  CHECK(loop::grab(loop::k16) == loop::kOk);
  for (int i = 0; i < 400 && loop::grabs(); ++i) {
    g_now += 48000, g_us += 1000;
    feed();
    loop::tick(g_us);
  }
  CHECK(loop::grabs() == 0 && loop::stats().abandoned == 1);

  // tempo_change_ignored: the loop keeps its length when the tempo moves
  fresh();
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  g = loop::grab_at(0);
  s0 = (uint64_t)(g_origin + 41 * bar());
  const uint32_t blen = g->frames;
  to(s0 + 100);
  g_per16 = 9000;
  g_origin += 777;
  to(s0 + blen + 200);
  CHECK(played_at(s0 + blen + 100) == g->pcm[2 * 100]);

  // transport_stop_plays_on: the loop plays on free; a new grab is refused (PLAY FIRST)
  g_beats = false;
  run_ms(10);
  CHECK(loop::state() == loop::kPlaying && played_at(s0 + 2 * blen + 100) == g->pcm[2 * 100]);
  CHECK(loop::grab(loop::k1) == loop::kErrStopped);

  // ring_records_during_playback: the ring keeps taking blocks while a loop plays
  uint64_t w0 = g_fed;
  run_ms(100);
  CHECK(g_fed > w0 && loop::state() == loop::kPlaying);
  printf("  store: drops, budget, pieces, ring-until-copied, overtaken ok\n");
}

// ---- Task 21: playback
static int32_t sine997(uint64_t k) { return (int32_t)(1e9 * sin(2 * M_PI * 997.0 * (double)k / 48000.0)); }
static void started(int hb = loop::k1, uint32_t* len = nullptr) {   // a grab pressed a third into bar 40, playing from bar 41's line
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(hb) == loop::kOk);
  run_ms(60);
  if (len) *len = loop::grab_at(0)->frames;
  to((uint64_t)(g_origin + 41 * bar()) + 500);
}
static void test_playback() {
  // seam_crossfaded: a 997 Hz sine does not fit the loop's length; across the end / start the step stays within the
  // signal's own slope (1.3e8 a frame) plus the crossfade's (2A / 64 = 3.1e7), where a plain wrap would jump up to 2e9
  fresh();
  g_sig = sine997;
  uint32_t len;
  started(loop::k1, &len);
  uint64_t s0 = (uint64_t)(g_origin + 41 * bar());
  double worst = 0;
  int32_t prev = played_at(s0 + len - 200);
  for (uint64_t h = s0 + len - 199; h < s0 + len + 200; ++h) {
    int32_t x = played_at(h);
    worst = fmax(worst, fabs((double)x - prev));
    prev = x;
  }
  CHECK(worst < 1.7e8);
  // (the loop's own frames away from the seam are the source's)
  CHECK(std::abs(played_at(s0 + 3000) - loop::grab_at(0)->pcm[2 * 3000]) <= 128);   // (float: 24 bits of mantissa)

  // lev_unity_at_100: LEV 100 is the source as it is; 50 half; 0 nothing; 127 1.27
  fresh();
  started(loop::k1, &len);
  s0 = (uint64_t)(g_origin + 41 * bar());
  const int32_t* pcm = loop::grab_at(0)->pcm;
  loop::set_lev(100);
  CHECK(played_at(s0 + 5000) == pcm[2 * 5000]);
  loop::set_lev(50);
  run_ms(20);   // (a level change slews over 10 ms)
  CHECK(std::abs(played_at(s0 + 30000) - pcm[2 * 30000] / 2) <= 2);
  loop::set_lev(0);
  run_ms(20);
  CHECK(played_at(s0 + 40000) == 0);
  loop::set_lev(127);
  run_ms(20);
  CHECK(std::abs((double)played_at(s0 + 50000) - 1.27 * pcm[2 * 50000]) < 2000);

  // bal_curve (spec 3.8): 0: loop 0, own 1; 64: both 1; 127: loop 1, own 0; equal power at 32 / 96
  CHECK(fabs(loop::loop_gain(0)) < 1e-6 && fabs(loop::own_gain(0) - 1) < 1e-6);
  CHECK(fabs(loop::loop_gain(64) - 1) < 1e-6 && fabs(loop::own_gain(64) - 1) < 1e-6);
  CHECK(fabs(loop::loop_gain(127) - 1) < 1e-6 && fabs(loop::own_gain(127)) < 1e-6);
  CHECK(fabs(loop::loop_gain(32) * loop::loop_gain(32) + 0.5 - 1) < 0.01 && fabs(loop::own_gain(32) - 1) < 1e-6);   // (the loop: 1/2 power, own full)
  CHECK(fabs(loop::own_gain(96) * loop::own_gain(96) - 0.5) < 0.03 && fabs(loop::loop_gain(96) - 1) < 1e-6);
  // ... and in the mix: the Sidecar's own output (every channel of F) falls with BAL while the loop plays
  fresh();
  g_sig = nullptr;
  started(loop::k1, &len);
  s0 = (uint64_t)(g_origin + 41 * bar());
  pcm = loop::grab_at(0)->pcm;
  loop::set_bal(127);
  run_ms(30);
  int32_t F[12][12];
  for (auto& r : F)
    for (int& c : r) c = 100000000;
  loop::out_mix(F, 12, s0 + 60000);
  CHECK(F[11][5] == 0 && F[11][2] == 0);                              // the own channels silent at BAL 127
  CHECK(F[11][0] == pcm[2 * (60000 + 11)] && F[11][1] == pcm[2 * (60000 + 11) + 1]);   // the loop alone (its own was 0)
  loop::set_bal(0);
  run_ms(30);
  for (auto& r : F)
    for (int& c : r) c = 100000000;
  loop::out_mix(F, 12, s0 + 61000);
  CHECK(F[11][5] == 100000000 && F[11][0] == 100000000);              // BAL 0: no loop

  // cut_fades_10ms: a CUT at once fades the loop over 480 frames, then nothing; the state goes back to the ring
  fresh();
  g_sig = nullptr;
  started(loop::k1, &len);
  s0 = (uint64_t)(g_origin + 41 * bar());
  pcm = loop::grab_at(0)->pcm;
  uint64_t t = g_now;
  loop::stop(true, true);
  CHECK(loop::state() == loop::kFading);
  uint64_t at = t + 96;   // the margin the audio thread needs
  CHECK(played_at(at - 1) == pcm[2 * (at - 1 - s0)]);
  double prevv = 1;
  for (int i = 0; i < 480; i += 40) {
    int32_t x = played_at(at + i);
    double gain = (double)x / pcm[2 * (at + i - s0)];
    CHECK(gain <= prevv + 1e-6 && gain > -1e-6);
    prevv = gain;
  }
  CHECK(played_at(at + 480) == 0 && played_at(at + 2000) == 0);
  to(at + 600);
  CHECK(loop::state() == loop::kRing);
  printf("  playback: seam, LEV, BAL, cut ok\n");
}


// ---- Task 22: resample, stop / fade back, auto-mute, LIVE VIA
using Msg = std::vector<uint8_t>;
static std::vector<Msg> g_wire;
static void wire(const uint8_t* m, int n) { g_wire.emplace_back(m, m + n); }
static int wires(const Msg& m) {
  int n = 0;
  for (const Msg& o : g_wire) n += o == m;
  return n;
}
static void test_resample_stop_mute() {
  hooks.to_rytm = wire;
  // resample_instant_in_phase: a grab while a loop plays takes the last N bars at once, replaces the loop at the next
  // bar line (a 64-frame crossfade), and its start is a whole number of loops after the old one's
  fresh();
  uint32_t len;
  started(loop::k1, &len);
  uint64_t old_start, s0 = (uint64_t)(g_origin + 41 * bar());
  uint32_t old_len;
  CHECK(loop::playing_info(&old_start, &old_len) && old_start == s0 && old_len == len);
  played_at(s0 + 1000);   // (the audio thread runs: it has the first loop)
  const int32_t* first_pcm = loop::grab_at(0)->pcm;   // (the store's copy stays where it is as newer grabs shift the list)
  to((uint64_t)(line(43) + bar() / 4));
  CHECK(loop::state() == loop::kPlaying);
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  CHECK(loop::grabs() == 2 && loop::stats().resamples == 1);
  const loop::Grab* g2 = loop::grab_at(0);
  CHECK(g2->start == (uint64_t)line(42));   // the last bar, as the ring has it (loop + live), taken at once
  uint64_t ns;
  uint32_t nl;
  CHECK(loop::playing_info(&ns, &nl) && ns == (uint64_t)(g_origin + 44 * bar()) && (ns - old_start) % len == 0);
  CHECK(played_at(ns - 200) == first_pcm[2 * ((ns - 200 - old_start) % len)]);   // still the old loop until the bar line
  CHECK(played_at(ns + 100) == g2->pcm[2 * 100]);                              // the new one after the crossfade

  // stop_fade_and_cut (the FADE setting): quantised to the next bar line, a FADE of 1 bar lasts a bar; FUNC (now) at once
  fresh();
  started(loop::k1, &len);
  s0 = (uint64_t)(g_origin + 41 * bar());
  loop::set_fade(loop::kFade1);
  to(s0 + 20000);
  loop::stop(false, false);
  uint64_t line42 = (uint64_t)(g_origin + 42 * bar());
  const int32_t* pcm = loop::grab_at(0)->pcm;
  CHECK(played_at(line42 - 200) == pcm[2 * ((line42 - 200 - s0) % len)]);   // full level until the line
  int32_t mid = played_at(line42 + len / 2);
  double gm = (double)mid / pcm[2 * ((line42 + len / 2 - s0) % len)];
  CHECK(gm > 0.45 && gm < 0.55);                                            // halfway down halfway through the bar
  CHECK(played_at(line42 + len + 100) == 0);
  to(line42 + len + 100);
  CHECK(loop::state() == loop::kRing);
  // a stop before the loop has started: it never starts
  fresh();
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  loop::stop(true, false);   // (the start is the next bar line, so is the stop's)
  CHECK(loop::state() == loop::kRing);

  // automute_default_off / live_via_default_off: a fresh state sends nothing
  fresh();
  g_wire.clear();
  g_muted = 0x005;
  started(loop::k1, &len);
  run_ms(12);
  CHECK(g_wire.empty());

  // automute_mutes_only_unmuted: tracks 1 and 3 muted before: one MUTE op sets the other ten; the fade back unmutes those
  fresh();
  mod::rytm_out_reset();
  mod::rytm_host_cmd(true);
  loop::set_auto_mute(true);
  g_muted = 0x005;
  g_wire.clear();
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  CHECK(g_wire.empty());   // (not before the loop starts)
  s0 = (uint64_t)(g_origin + 41 * bar());
  to(s0 + 500);
  CHECK(g_wire.size() == 1 && g_wire[0] == Msg({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 1, 0x7A, 0x1F, 0, 0, 0xF7}));   // set 0xFFA
  g_muted = 0xFFF;   // the Rytm reports them muted
  to(s0 + 20000);
  g_wire.clear();
  loop::stop(true, false);
  to((uint64_t)(g_origin + 42 * bar()) - 100);
  CHECK(g_wire.empty());
  to((uint64_t)(g_origin + 42 * bar()) + 100);
  CHECK(g_wire.size() == 1 && g_wire[0] == Msg({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 1, 0, 0, 0x7A, 0x1F, 0xF7}));   // clr 0xFFA
  // with the caps off: CC 94 on each track's channel instead
  fresh();
  mod::rytm_out_reset();
  loop::set_auto_mute(true);
  g_muted = 0x005;
  g_wire.clear();
  started(loop::k1, &len);
  CHECK(g_wire.size() == 10 && wires({0xB1, 94, 1}) == 1 && wires({0xB2, 94, 1}) == 0 && wires({0xB0 | 11, 94, 1}) == 1);

  // user_mute_during_loop_kept: the user unmutes track 4 meanwhile: the looper no longer owns it
  fresh();
  mod::rytm_out_reset();
  mod::rytm_host_cmd(true);
  loop::set_auto_mute(true);
  g_muted = 0x005;
  started(loop::k1, &len);
  g_muted = 0xFFF;
  run_ms(10);
  g_muted = 0xFF7;   // track 4 (bit 3) unmuted by hand
  run_ms(10);
  g_wire.clear();
  loop::stop(true, true);
  run_ms(10);
  CHECK(g_wire.size() == 1 && g_wire[0] == Msg({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 1, 0, 0, 0x72, 0x1F, 0xF7}));   // clr 0xFF2

  // live_via_perf: BAL 64: 127, 96: 63, 127: 0 on the macro asked for; nothing before it is asked for
  fresh();
  mod::rytm_out_reset();
  mod::rytm_host_cmd(true);
  loop::set_live_via(3);
  CHECK(loop::live_perf(64) == 127 && loop::live_perf(96) == 63 && loop::live_perf(127) == 0 && loop::live_perf(0) == 127);
  g_wire.clear();
  started(loop::k1, &len);
  run_ms(12);
  CHECK(wires({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 3, 3, 127, 0xF7}) == 1);
  loop::set_bal(96);
  run_ms(10);
  run_ms(12);
  CHECK(wires({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 3, 3, 63, 0xF7}) == 1);
  loop::set_bal(127);
  run_ms(10);
  run_ms(12);
  CHECK(wires({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 3, 3, 0, 0xF7}) == 1);
  loop::stop(true, true);   // the live comes back with the loop's end
  run_ms(30);
  run_ms(12);
  CHECK(wires({0xF0, 0x7D, 0x52, 0x54, mod::kHostCmd, 1, 3, 3, 127, 0xF7}) == 2);
  hooks.to_rytm = nullptr;
  host_fake_time(false, 0);
  printf("  resample, stop, auto-mute, live via ok\n");
}


// ---- Task 25: calibration, console, targets, settings
static uint64_t g_click_c0 = 0;
static int g_rtt = 0;
static int32_t click_sig(uint64_t k) {   // the clicks as the Rytm hands them back: g_rtt frames after they were rendered
  for (int c = 0; c < 3; ++c) {
    int64_t kc = (int64_t)(g_click_c0 + (uint64_t)c * 9600 + (uint64_t)g_rtt) - kD;
    if ((int64_t)k >= kc && (int64_t)k < kc + 24) return 6000000;
  }
  return 0;
}
static std::string cmd(const char* c) {
  char o[600];
  CHECK(loop::command(c, o, sizeof o));
  return o;
}
static void test_cal_console() {
  hooks.to_rytm = wire;
  // cal_finds_click: a simulated round trip of 282 + 30 frames against the sequencer's latency (g_lat = 282): post_lat 30
  fresh();
  g_lat = 282;
  g_rtt = 282 + 30;
  g_sig = click_sig;
  to((uint64_t)line(10));
  loop::cal_start();
  CHECK(loop::cal_wanted());
  uint64_t first;
  int n;
  uint32_t gap;
  CHECK(loop::cal_info(&first, &n, &gap) && n == 3);
  g_click_c0 = first;
  // the OUT hook renders the clicks on POST 0 / 1 (and only there) at their frames
  int32_t F[12][12] = {};
  loop::out_mix(F, 12, first - 4);
  CHECK(F[3][0] == 0 && F[4][0] != 0 && F[4][1] == F[4][0] && F[4][2] == 0);
  // ... and the IN hook finds them: run the clock past the last click
  to(first + 3 * 9600 + 6000);
  CHECK(!loop::cal_wanted() && loop::post_lat() == 30 && loop::stats().cal_ok == 1);
  CHECK(std::string(loop::last_message()) == "POST LAT 30");
  // no click heard: refused, post_lat kept
  fresh();
  g_sig = nullptr;
  to((uint64_t)line(10));
  loop::set_post_lat(5);
  loop::cal_start();
  to(g_now + 14400 + 3 * 9600 + 6000);
  CHECK(loop::post_lat() == 5 && loop::stats().cal_fail == 1 && std::string(loop::last_message()) == "CAL: NO CLICK HEARD");

  // cal with the transport stopped: refused, post_lat kept (the sequencer's latency is not there to compare with)
  fresh();
  g_sig = click_sig;
  g_lat = 282, g_rtt = 312;
  to((uint64_t)line(10));
  loop::set_post_lat(7);
  loop::cal_start();
  CHECK(loop::cal_info(&first, &n, &gap));
  g_click_c0 = first;
  g_beats = false;
  to(first + 3 * 9600 + 6000);
  CHECK(loop::post_lat() == 7 && std::string(loop::last_message()) == "CAL: PLAY FIRST");

  // console_lines (spec 3.12)
  fresh();
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(cmd("loop").find("loop: ring,") == 0 && cmd("loop").find("0 grabs") != std::string::npos);
  CHECK(cmd("loop=grab:1").find("grab 1 bars") != std::string::npos);
  run_ms(60);
  CHECK(loop::grabs() == 1);
  CHECK(cmd("loop=grab:0.5").find("grab 1/2 bars") != std::string::npos);
  run_ms(60);
  CHECK(loop::grabs() == 2 && loop::grab_at(0)->half_bars == 1);
  CHECK(cmd("loop=grab:3").find("loop=grab:L") != std::string::npos);
  CHECK(cmd("loop=play:1").find("play grab 1") != std::string::npos);
  {   // by id: a grab landing meanwhile does not move what is meant (L6)
    uint32_t id1 = loop::grab_at(1)->id;
    CHECK(cmd(("loop=play:#" + std::to_string(id1)).c_str()).find("play grab 1") != std::string::npos);
    CHECK(cmd("loop=play:#9999").find("no grab") != std::string::npos);
    CHECK(cmd("loop=keep:#9999").find("NO GRAB") != std::string::npos);
  }
  CHECK(cmd("loop=bal:90").find("bal 90") != std::string::npos && loop::bal() == 90);
  CHECK(cmd("loop=lev:80").find("lev 80") != std::string::npos && loop::lev() == 80);
  CHECK(cmd("loop=fade:2").find("fade 2 bars") != std::string::npos && loop::fade() == loop::kFade2);
  CHECK(cmd("loop=fade:1/4").find("fade 1/4 bars") != std::string::npos && loop::fade() == loop::kFadeQuarter);
  CHECK(cmd("loop=mute:on").find("auto-mute on") != std::string::npos && loop::auto_mute());
  CHECK(cmd("loop=mute:off").find("off") != std::string::npos && !loop::auto_mute());
  CHECK(cmd("loop=live:perf5").find("PERF 5") != std::string::npos && loop::live_via() == 5);
  CHECK(cmd("loop=live:off").find("off") != std::string::npos && loop::live_via() == 0);
  CHECK(cmd("loop=keep:0").find("COPYING") == std::string::npos);   // (copied by now)
  loop::KeepJob job;
  CHECK(loop::keep_take(&job) && job.frames == loop::grab_at(0)->frames);
  CHECK(!loop::grab_at(0)->kept);
  loop::keep_finished(job.id, true, "take_001");
  loop::tick(g_us);   // core 3 applies it
  CHECK(loop::grab_at(0)->kept && std::string(loop::last_message()) == "KEPT take_001");
  CHECK(cmd("loop=keep:0").find("ALREADY KEPT") != std::string::npos);
  CHECK(cmd("loop=keep:7").find("NO GRAB") != std::string::npos);
  CHECK(cmd("loop=stop:cut:now").find("cut, at once") != std::string::npos);
  CHECK(cmd("loop=stop").find("fade, at the next bar line") != std::string::npos);
  CHECK(cmd("loop=cal").find("calibrating") != std::string::npos && loop::cal_wanted());
  g_beats = false;
  CHECK(cmd("loop=grab:1").find("PLAY FIRST") != std::string::npos);
  CHECK(std::string(loop::last_message()) == "PLAY FIRST");
  char js[1200];
  CHECK(loop::json(js, sizeof js) > 0 && strstr(js, "\"loop\":{\"on\":1") && strstr(js, "\"grabs\":[{") && strstr(js, "\"kept\":1"));
  CHECK(!loop::command("looper", js, sizeof js) && !loop::command("lo", js, sizeof js));

  {   // the status text without floats (the Pi's sniprintf has none): exact
    fresh();
    started(loop::k1);
    char jb[1500];
    CHECK(loop::json(jb, sizeof jb) > 0);
    CHECK(strstr(jb, "\"bpm\":120.0,") && strstr(jb, "\"bars\":1.0") && !strstr(jb, "e+"));
    const char* bp = strstr(jb, "\"bar\":");
    CHECK(bp && bp[6] >= '0' && bp[6] <= '1' && bp[7] == '.' && bp[8] >= '0' && bp[11] == ',');   // d.ddd
    std::string c = cmd("loop");
    CHECK(c.find(" pos 0.") != std::string::npos || c.find(" pos 1.") != std::string::npos);
    CHECK(c.find("/1,") != std::string::npos);
  }

  // bal_is_a_target: LOOP:BAL / LOOP:LEV are modulation targets; a value replaces the knob, a release gives it back
  fresh();
  mod::Tid tb = 0, tl = 0;
  CHECK(mod::parse("LOOP:BAL", &tb) && mod::parse("LOOP:LEV", &tl) && mod::valid(tb) && mod::valid(tl));
  mod::Range r = mod::range(tb);
  CHECK(r.lo == 0 && r.hi == 127);
  mod::reset();
  loop::set_bal(40);
  loop::set_lev(100);
  mod::offset(0, tb, 1.0f, 30.0f);   // an LFO's offset of +30 around the knob
  mod::end_tick();
  CHECK(loop::bal_eff() == 70 && loop::bal() == 40);
  mod::begin_tick();
  mod::end_tick();   // nothing refers to it any more: released
  CHECK(loop::bal_eff() == 40);
  CHECK(loop::own_gain(loop::bal_eff()) == 1.0f);

  // settings_persist: the card's loop.txt carries them; a line from elsewhere is kept
  fresh();
  loop::set_bal(96), loop::set_lev(77), loop::set_fade(loop::kFade4), loop::set_auto_mute(true), loop::set_live_via(7), loop::set_post_lat(-12);
  CHECK(loop::dirty());
  char tmpl[] = "/tmp/core_loop_XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  card_ok = true;
  system((std::string("mkdir -p ") + tmpl + "/.rytm; printf 'other=1\\nbal=1\\n' > " + tmpl + "/.rytm/loop.txt").c_str());
  loop::load();   // (an older file with other keys: bal=1 read, our dirty state replaced by the file's)
  CHECK(loop::bal() == 1 && !loop::dirty());
  loop::set_bal(96), loop::set_lev(77), loop::set_fade(loop::kFade4), loop::set_auto_mute(true), loop::set_live_via(7), loop::set_post_lat(-12);
  CHECK(loop::save());
  for (int i = 0; i < 2000 && fmt::post_busy(); ++i) {
    fmt::post_step();
    host_card_core(0);
    while (cardq::service()) {
    }
    host_card_core(3);
  }
  loop::reset();
  CHECK(loop::bal() == 64 && !loop::auto_mute());
  loop::load();
  CHECK(loop::bal() == 96 && loop::lev() == 77 && loop::fade() == loop::kFade4 && loop::auto_mute() && loop::live_via() == 7 && loop::post_lat() == -12);
  CHECK(!loop::save());   // nothing changed: nothing written
  hooks.to_rytm = nullptr;
  host_fake_time(false, 0);
  printf("  calibration, console, targets, settings ok\n");
}


// ---- the hooks' cost (spec 3.11: well under 1 us a 7-frame block on the Pi 4); here on the Mac, idle and with a loop playing
static void test_cost() {
  fresh();
  started(loop::k1);
  using clk = std::chrono::steady_clock;
  int32_t b[14] = {};
  int32_t F[7][12] = {};
  const int N = 1000000;
  auto t0 = clk::now();
  uint64_t arr = g_fed + kD;
  for (int i = 0; i < N; ++i) loop::in_block(b, 7, arr += 7, 0);
  double in_ns = std::chrono::duration<double, std::nano>(clk::now() - t0).count() / N;
  uint64_t s0 = (uint64_t)(g_origin + 41 * bar());
  t0 = clk::now();
  for (int i = 0; i < N; ++i) loop::out_mix(F, 7, s0 + 5000 + 7 * (uint64_t)(i % 10000));
  double play_ns = std::chrono::duration<double, std::nano>(clk::now() - t0).count() / N;
  CHECK(F[3][0] != 0);   // (the loop was really playing)
  loop::stop(true, true);
  to(g_now + 20000);
  t0 = clk::now();
  for (int i = 0; i < N; ++i) loop::out_mix(F, 7, s0 + 5000 + 7 * (uint64_t)(i % 10000));
  double idle_ns = std::chrono::duration<double, std::nano>(clk::now() - t0).count() / N;
  printf("  hook cost per 7-frame block (Mac): IN %.0f ns, OUT playing %.0f ns, OUT idle %.0f ns\n", in_ns, play_ns, idle_ns);
  CHECK(in_ns < 1000 && idle_ns < 200 && play_ns < 5000);   // generous: the Mac; the Pi 4 is a few times slower
}


// ---- fix round 1: the resample's wait (a new loop scheduled while the old one is heard)
static void test_resample_wait() {
  hooks.to_rytm = wire;
  // H2: a second grab, and a tap, during the wait do not cut the audible loop (a click, then silence)
  fresh();
  uint32_t len;
  started(loop::k1, &len);
  uint64_t s0 = (uint64_t)(g_origin + 41 * bar());
  played_at(s0 + 1000);   // (the audio thread runs: it has loop A)
  const int32_t* a_pcm = loop::grab_at(0)->pcm;
  to((uint64_t)(line(43) + bar() / 4));
  played_at(g_now);
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  played_at(g_now);
  CHECK(loop::grab(loop::k1) == loop::kOk);   // a third grab in the same wait
  run_ms(60);
  CHECK(loop::grabs() == 3);
  CHECK(loop::play(2));                        // and a tap on A's own pad
  uint64_t ns;
  uint32_t nl;
  CHECK(loop::playing_info(&ns, &nl) && ns == (uint64_t)(g_origin + 44 * bar()));
  uint64_t h = ns - 3000;
  CHECK(played_at(h) == a_pcm[2 * ((h - s0) % len)] && a_pcm[2 * ((h - s0) % len)] != 0);   // A is still heard until the bar line
  CHECK(loop::grab_at(2)->copied);   // (A is the oldest of three and still held: protected while heard)

  // H3: BAL 127: the Sidecar's own output stays down through the wait
  fresh();
  started(loop::k1, &len);
  s0 = (uint64_t)(g_origin + 41 * bar());
  loop::set_bal(127);
  run_ms(30);
  int32_t F[12][12];
  auto own = [&](uint64_t hh) {
    for (auto& r : F)
      for (int& c : r) c = 100000000;
    loop::out_mix(F, 12, hh);
    return F[11][5];
  };
  CHECK(own(s0 + 20000) == 0);
  to((uint64_t)(line(43) + bar() / 4));
  own(g_now);
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  CHECK(loop::playing_info(&ns, &nl));
  CHECK(own(g_now) == 0 && own(ns - 20000) == 0 && own(ns - 100) == 0);   // (it jumped back to full here)
  for (uint64_t hh = ns - 20; hh < ns + 200; ++hh) CHECK(own(hh) == 0);       // N1: and through the swap (a click 64 frames in)

  // H4: auto-mute and LIVE VIA hold through the wait; M1: a stop in the wait stops what is heard
  fresh();
  mod::rytm_out_reset();
  mod::rytm_host_cmd(true);
  loop::set_auto_mute(true);
  loop::set_live_via(3);
  g_muted = 0x005;
  g_wire.clear();
  started(loop::k1, &len);
  s0 = (uint64_t)(g_origin + 41 * bar());
  g_muted = 0xFFF;
  run_ms(20);
  size_t before = g_wire.size();
  played_at(g_now);
  to((uint64_t)(line(43) + bar() / 4));
  played_at(g_now);
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(100);
  CHECK(g_wire.size() == before);   // no unmute, no PERF back to 127: the Rytm stays muted under the resample's wait
  CHECK(loop::playing_info(&ns, &nl));
  played_at(g_now);
  const int32_t* a2 = loop::grab_at(1)->pcm;
  uint64_t t = g_now;
  loop::stop(true, true);   // FUNC + T8 in the wait
  CHECK(loop::state() == loop::kFading);
  uint64_t at = t + 96;
  uint64_t hb = at - 2;
  CHECK(played_at(hb) == a2[2 * ((hb - s0) % len)]);   // A plays to the cut
  CHECK(played_at(at + 480) == 0 && played_at(at + 3000) == 0 && played_at(ns + 100) == 0);   // then nothing: not B at its start
  to(at + 600);
  CHECK(loop::state() == loop::kRing && g_wire.size() > before);   // and the mutes are given back
  hooks.to_rytm = nullptr;
  printf("  resample wait: a second grab / a tap, BAL, auto-mute, LIVE VIA and stop hold up\n");
}


// ---- M4: a grab that cannot fit even with every droppable grab gone drops none
static void test_room_without_drops() {
  fresh(4u << 20);   // 4 MB: four 1-bar grabs (768 KB each) fill it
  to((uint64_t)line(10));
  for (int i = 0; i < 4; ++i) {
    CHECK(loop::grab(loop::k1) == loop::kOk);
    run_ms(40);
    to((uint64_t)(g_origin + (11 + i) * bar() + bar() / 2 + g_lat));
  }
  CHECK(loop::grabs() == 4);
  CHECK(loop::play(2));   // the second grab made plays: it sits in the middle of the store, protected
  uint32_t played_id = loop::grab_at(2)->id;
  CHECK(loop::grab(loop::k4) == loop::kOk);   // 3 MB: fits the store's size, not any gap around the playing grab
  run_ms(60);
  CHECK(loop::grabs() == 4 && loop::stats().dropped == 0 && loop::grab_at(2)->id == played_id);
  printf("  room: a grab that cannot fit drops nothing\n");
}


// ---- N4 / N5
static void test_wait_edges() {
  // N5: a tap while the first loop waits for its start puts nothing under it
  fresh();
  to((uint64_t)(line(40) + bar() / 3));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  CHECK(loop::grab(loop::kHalf) == loop::kOk);
  run_ms(60);
  CHECK(loop::play(1));   // (the first loop has not started)
  to((uint64_t)(g_origin + 45 * bar()));
  CHECK(loop::stats().grabs == 2);
  // N4: the copy of the loop under a waiting resample is abandoned: the new loop plays on, the old one is dropped
  fresh(48u << 20);
  g_per16 = 6000 * 120 / 47;   // 47 BPM: a 16-bar grab barely fits
  to((uint64_t)(line(20) + bar() / 16));
  CHECK(loop::grab(loop::k1) == loop::kOk);
  run_ms(60);
  CHECK(loop::grab(loop::k16) == loop::kOk);   // copying from the ring: the loop plays from the ring
  CHECK(loop::grabs() == 2);
  uint32_t big = loop::grab_at(0)->id, small = loop::grab_at(1)->id;
  (void)big;
  CHECK(loop::play(loop::grab_index(small)));   // a tap on the 1-bar grab while the 16-bar one is still copying
  for (int i = 0; i < 400 && loop::stats().abandoned == 0; ++i) {
    g_now += 48000, g_us += 1000;
    feed();
    loop::tick(g_us);
  }
  CHECK(loop::stats().abandoned == 1 && loop::grabs() == 1);
  uint64_t st;
  uint32_t ln;
  CHECK(loop::playing_info(&st, &ln) && ln == loop::grab_at(0)->frames && loop::state() != loop::kRing);   // the tapped loop plays on
  printf("  wait edges: a first loop's tap, an abandoned copy under a resample\n");
}

int main() {
  test_ring();
  test_ring_gap_cost();
  test_grabs();
  test_store();
  test_playback();
  test_resample_stop_mute();
  test_cal_console();
  test_resample_wait();
  test_room_without_drops();
  test_wait_edges();
  test_cost();
  printf("test_loop: OK\n");
  return 0;
}

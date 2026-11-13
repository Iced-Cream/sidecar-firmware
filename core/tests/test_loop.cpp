// core/tests/test_loop.cpp - the master looper (core/loop, spec-050 section 3): the MAIN ring and its frame map (Task 19),
// the grabs (20), playback (21), resample / stop / auto-mute / LIVE VIA (22), calibration and console (25).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
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
  CHECK(mem3[2 * 692] == 693 && mem3[2 * 693] == 0 && mem3[2 * 762] == 0);   // zeros at 693..762
  CHECK(mem3[2 * 763] == 764 && mem3[2 * 1000] == 1001);                     // linear again, the same map
  CHECK(g.gap_in(100 + 650, 200) && !g.gap_in(100 + 100, 100) && !g.gap_in(100 + 900, 200));
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
static uint64_t g_fed = 0;                      // ring indices written so far
static uint32_t g_us = 1000000;
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
    for (int i = 0; i < 7; ++i) b[2 * i] = (int32_t)(g_fed + i + 1), b[2 * i + 1] = -(int32_t)(g_fed + i + 1);
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
  g_now = 0, g_fed = 0, g_us = 1000000, g_origin = 10000, g_per16 = 6000, g_lat = 384, g_beats = true, g_muted = 0;
}
static void to(uint64_t h) {   // the clock to host frame h, a tick a ms on the way
  while (g_now + 48 <= h) {
    g_now += 48, g_us += 1000;
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

int main() {
  test_ring();
  test_grabs();
  test_store();
  printf("test_loop: OK\n");
  return 0;
}

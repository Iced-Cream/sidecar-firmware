// core/tests/test_loop.cpp - the master looper (core/loop, spec-050 section 3): the MAIN ring and its frame map (Task 19),
// the grabs (20), playback (21), resample / stop / auto-mute / LIVE VIA (22), calibration and console (25).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
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

int main() {
  test_ring();
  printf("test_loop: OK\n");
  return 0;
}

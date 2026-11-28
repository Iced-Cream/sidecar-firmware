// pi/engine/tests/test_drift_src.cpp - drift_src.h with two simulated boxes: each makes 32-frame audio blocks and moves them in 7-frame USB blocks, one
// a microframe, at its own crystal. Holds the fill, what comes out against the ideal tone, stops, restarts, a full ring, the cost.
#include "check.h"
#include "drift_src.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

static DriftSrc g_s;

// Both boxes in time, microframe by microframe: the writer's blocks arrive on the bus, the reader pulls 7 frames for each block of the master's.
struct Sim {
  double ppm_w, ppm_r;     // each box's clock against 48 kHz
  double f;                // the tone, Hz at the writer's clock
  double t_w = 0, t_r = 0; // the next 32-frame boundary, seconds
  double t_w_next = 0, t_r_next = 0;
  int have_w = 0, have_r = 0;
  uint64_t n_w = 0;
  double amp = 0.25;
  double now = 0;
  bool on = true, raw = false;
  void step(std::vector<float>* rec) {
    now += 125e-6;
    const double fs_w = 48000.0 * (1 + ppm_w * 1e-6), fs_r = 48000.0 * (1 + ppm_r * 1e-6);
    while (t_w <= now) t_w += 32 / fs_w, have_w += 32;
    while (t_r <= now) t_r += 32 / fs_r, have_r += 32;
    if (have_w >= 7) {
      have_w -= 7;
      if (on)
        for (int k = 0; k < 7; ++k, ++n_w) {
          float x = (float)(amp * sin(2 * M_PI * f * (double)n_w / 48000.0));
          g_s.push(x, -x);
        }
    }
    if (have_r >= 7) {
      have_r -= 7;
      float o[7][2];
      double at = g_s.position(), step = 1.0 + g_s.ppm() * 1e-6;   // the block's read points
      g_s.pull(o, 7);
      if (rec)
        for (int k = 0; k < 7; ++k) rec->push_back(raw ? o[k][0] : o[k][0] - (float)(amp * sin(2 * M_PI * f * (at + k * step) / 48000.0)));
    }
  }
};

int main() {
  g_s.init();
  CHECK(DriftSrc::kTarget > 2 * DriftSrc::kLow - DriftSrc::kFloor);
  struct { double w, r; } drift[] = {{0, 0}, {+40, -60}, {-60, +40}, {+150, -150}};
  for (auto d : drift) {
    double f[] = {100, 1000, 10000, 15000, 18000, 20000};
    for (double hz : f) {
      g_s.~DriftSrc();
      new (&g_s) DriftSrc();
      g_s.init();
      Sim s{d.w, d.r, hz};
      double lo = 1e9, hi = -1e9;
      for (long i = 0; i < 8000 * 12; ++i) s.step(nullptr);       // 12 s to settle
      std::vector<float> rec;
      double sum = 0;
      const long n = 8000 * 8;                                    // past the beat of the two boxes' blocks
      for (long i = 0; i < n; ++i) {
        s.step(&rec);
        double l = g_s.fill();
        lo = l < lo ? l : lo, hi = l > hi ? l : hi;
        sum += g_s.ppm();
      }
      CHECK(g_s.running());
      CHECK(g_s.under() == 0 && g_s.low() == 0 && g_s.high() == 0 && g_s.full() == 0);
      double e = 0;
      for (float x : rec) e += (double)x * x;
      double q = 10 * log10(s.amp * s.amp / 2 / (e / (double)rec.size()));   // under the tone, against the ideal value at each read position
      printf("  %+5.0f ppm %6.0f Hz: fill %.1f..%.1f, follows %+.1f ppm, %.1f dB\n", d.w - d.r, hz, lo, hi, sum / (double)n, q);
      CHECK(fabs(sum / (double)n - (d.w - d.r)) < 2.0);
      CHECK(lo > DriftSrc::kTarget - 8.0 && hi < DriftSrc::kTarget + 8.0);
      if (hz <= 15000) CHECK(q > 88.0);
    }
  }

  // The writer stops (unplugged): a fade, no underrun, silence; it comes back: primes, follows, nothing lost
  {
    g_s.~DriftSrc();
    new (&g_s) DriftSrc();
    g_s.init();
    Sim s{+50, -50, 1000};
    for (long i = 0; i < 8000 * 6; ++i) s.step(nullptr);
    s.on = false, s.raw = true;
    std::vector<float> rec;
    for (long i = 0; i < 8000; ++i) s.step(&rec);
    CHECK(g_s.low() == 1 && g_s.under() == 0 && !g_s.running());
    float jump = 0;
    for (size_t n = 1; n < rec.size(); ++n) jump = std::max(jump, fabsf(rec[n] - rec[n - 1]));
    CHECK(jump < 0.05f);      // a 1 kHz tone's own slope is 0.033 a sample at this level, the fade adds at most amp / kFade: no step
    CHECK(rec.back() == 0.0f);
    s.on = true;
    for (long i = 0; i < 8000 * 6; ++i) s.step(nullptr);
    CHECK(g_s.running() && g_s.low() == 1);
    CHECK(fabs(g_s.ppm() - 100.0) < 40.0);
  }

  // Nobody reading (no Rytm): the ring fills, the writer drops; the first pull starts at the newest frames, latency as ever
  {
    g_s.~DriftSrc();
    new (&g_s) DriftSrc();
    g_s.init();
    for (int i = 0; i < 2000; ++i) g_s.push(0.1f, 0.1f);
    CHECK(g_s.full() > 900);
    float o[7][2];
    g_s.pull(o, 7);
    CHECK(g_s.running());
    CHECK(fabs(g_s.fill() - DriftSrc::kTarget) < 8.0);
  }

  // One frame's work
  {
    g_s.~DriftSrc();
    new (&g_s) DriftSrc();
    g_s.init();
    Sim s{+50, -50, 1000};
    for (long i = 0; i < 8000; ++i) s.step(nullptr);
    float o[7][2];
    for (int i = 0; i < 200; ++i) g_s.push(0.1f, 0.1f);
    auto t0 = std::chrono::steady_clock::now();
    float sum = 0;
    for (int i = 0; i < 20000; ++i) {
      for (int k = 0; k < 7; ++k) g_s.push(0.1f, 0.1f);
      g_s.pull(o, 7);
      sum += o[0][0];
    }
    double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (20000 * 7);
    printf("  %.0f ns a stereo frame here (sum %.1f)\n", ns, sum);
  }
  printf("test_drift_src: ok\n");
  return 0;
}

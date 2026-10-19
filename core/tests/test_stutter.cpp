// core/tests/test_stutter.cpp - STUTTER (TYPE 15, core/fx/stutter_fx.h) through core::fx::SendFx: the dry sum
// bit-exact at unity, engaged repeats sample-exact and on the clock grid at several tempos (on the grid, an early
// report snapped, a late one at its phase), no step above the fade limits at engage / boundaries / gate / release,
// the decay and pitch-down curves, p-locked engage per step through 0x20 sequences, free run without a clock (and when
// stopped), CC 120 to dry, one STUTTER at a time, the CPU per 12-frame block.
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <unistd.h>
#include <vector>

#include "check.h"
#include "fx/send_fx.h"

using namespace core;
using namespace core::fx;

static void clear_control() {
  uint32_t clk = control.clock;
  memset((void*)&control, 0, sizeof control);
  control.clock = clk;   // clock_in's own state goes on (as the Rytm's)
}
// a value unique to each frame (a ramp), exact in float
static float ramp(long i) { return (float)(i % 200000) * (1.0f / 262144.0f) - 0.25f; }
static float g_f = 437.0f;
static float sine(long i) { return 0.5f * std::sin(6.283185307f * g_f * (float)i / 48000.0f); }

// One run: stem 0 = src (or every stem a different noise when all), a MIDI clock at fpt frames a tick (0: none)
// starting with Start at frame 0, blocks of `blk` frames. y = MAIN (left), dry = the plain sum.
struct Rig {
  std::unique_ptr<SendFx> fx = std::make_unique<SendFx>(48000.0f);
  double fpt = 0, next = 0;
  long frame = 0;
  int blk = 8;
  bool all = false, ticking = true;
  float (*src)(long) = ramp;
  std::vector<float> y, dry, r;
  uint32_t rng = 1;
  explicit Rig(double fpt_, float (*s)(long) = ramp) : fpt(fpt_), src(s) {
    clear_control();
    control.knob[0][0] = kStutter;
    control.knob[0][1] = 127;   // P1 POST: the insert on MAIN (0/1); P1 0 puts it on 2/3, test_send_fx.cpp
    if (fpt > 0) clock_in(0xFA);
  }
  void run(long n) {
    float st[SendFx::kMaxFrames][kStems], o[SendFx::kMaxFrames][2], m[SendFx::kMaxFrames][2];
    for (long done = 0; done < n;) {
      int k = (int)(n - done < blk ? n - done : blk);
      while (fpt > 0 && ticking && next <= (double)frame) clock_in(0xF8), next += fpt;
      for (int j = 0; j < k; ++j) {
        float sum = 0;
        for (int v = 0; v < kStems; ++v) {
          st[j][v] = !all ? (v == 0 ? src(frame + j) : 0.0f)
                          : (float)((int32_t)(rng = rng * 1664525u + 1013904223u) >> 8) * (1.0f / 16777216.0f);
          sum += st[j][v];
        }
        dry.push_back(sum);
      }
      fx->Process(st, k, o, control, m);
      for (int j = 0; j < k; ++j) {
        y.push_back(m[j][0]), r.push_back(m[j][1]);
        CHECK(o[j][0] == 0.0f && o[j][1] == 0.0f);   // nothing on the send return
      }
      frame += k, done += k;
    }
  }
  void p2(int v) { control.knob[0][2] = (uint8_t)v; }
};
static float max_step(const std::vector<float>& y, long a, long b) {
  float m = 0;
  for (long i = a + 1; i < b; ++i) m = std::fmax(m, std::fabs(y[i] - y[i - 1]));
  return m;
}
// knob values for the zones
static const int kL64 = 0, kL32 = 20, kL16 = 40, kL8 = 60, kL4 = 75, kL2 = 95, kBar = 120;

int main() {
  CHECK(sizeof(void*) >= 4);
  printf("test_stutter: ring %d frames = %zu bytes (Stutter %zu bytes, SendFx %zu bytes)\n", Stutter::kRing,
         sizeof(float) * Stutter::kRing, sizeof(Stutter), sizeof(SendFx));
  CHECK(Stutter::zone_half_ticks(kL64) == 3 && Stutter::zone_half_ticks(kL32) == 6 && Stutter::zone_half_ticks(kL16) == 12 &&
        Stutter::zone_half_ticks(kL8) == 24 && Stutter::zone_half_ticks(kL4) == 48 && Stutter::zone_half_ticks(kL2) == 96 &&
        Stutter::zone_half_ticks(kBar) == 192 && Stutter::zone_half_ticks(127) == 192);
  CHECK(type_from_name("stutter") == 15 && strcmp(type_name(15), "STUTTER") == 0 && builtin(15));

  // ---- the dry sum at unity, bit-exact (every stem noise; knobs 0, then P3..P5 set with P2 below 64)
  {
    Rig g(1000);
    g.all = true;
    g.run(48000);
    long bad = 0;
    for (long i = kFadeFrames; i < 48000; ++i) bad += g.y[i] != g.dry[i] || g.r[i] != g.dry[i];
    control.knob[0][3] = 90, control.knob[0][4] = 100, control.knob[0][5] = 50, g.p2(63);
    g.run(48000);
    for (long i = 48000; i < 96000; ++i) bad += g.y[i] != g.dry[i];
    printf("test_stutter: passthrough: %ld of 96000 frames differ from the dry sum\n", bad);
    CHECK(bad == 0);
    // MAIN not asked for: the insert added onto out (core_c / sim), still exact
    float st[12][kStems] = {}, o[12][2];
    st[0][0] = 0.3f, st[0][1] = 0.1f;
    g.fx->Process(st, 12, o, control);
    CHECK(o[0][0] == 0.3f + 0.1f && o[0][1] == o[0][0]);
  }

  // ---- engaged: the slice repeats sample-exact, on the grid, at three tempos and three lengths
  for (double fpt : {1200.0, 1000.0, 800.0})   // 100, 120, 150 BPM
    for (int len : {kL16, kL4, kBar}) {
      Rig g(fpt);
      int ht = Stutter::zone_half_ticks((uint8_t)len);
      long L = (long)(ht * 0.5 * fpt + 0.5), bar = (long)(96 * fpt);
      control.knob[0][3] = (uint8_t)len;
      long gp = 2 * bar;   // a grid point: bar 3
      g.run(gp);
      g.p2(127);
      g.run(3 * L);
      long bad = 0, checked = 0;
      for (long j = 0; j < 3 * L; ++j) {
        long f = gp + j, ph = j % L;
        if (j < Stutter::kEnv || (j >= L && ph < Stutter::kXf + 1)) continue;   // the fades
        ++checked, bad += g.y[f] != ramp(gp - L + ph);
      }
      CHECK(g.fx->stutter().SliceFrames() == L && g.fx->stutter().SliceStart() == (uint32_t)(gp - L));
      // early (a report 5 ms before the grid point: waits for it) and late (mid-slice: the same grid, its phase)
      Rig e(fpt);
      control.knob[0][3] = (uint8_t)len;
      e.run(gp - 240), e.p2(127), e.run(240 + 2 * L);
      for (long j = Stutter::kEnv; j < L; ++j) ++checked, bad += e.y[gp + j] != ramp(gp - L + j);
      for (long j = 0; j < 240; ++j) bad += e.y[gp - 240 + j] != e.dry[gp - 240 + j];   // dry until the grid point
      Rig l(fpt);
      control.knob[0][3] = (uint8_t)len;
      long late = L / 2;
      if (L > 2000) {
        l.run(gp + late), l.p2(127), l.run(L);
        for (long j = late + Stutter::kEnv; j < L; ++j) ++checked, bad += l.y[gp + j] != ramp(gp - L + j);
      }
      printf("test_stutter: %.0f BPM, slice %ld frames: %ld of %ld repeat frames off the captured audio\n",
             48000.0 * 60 / (24 * fpt), L, bad, checked);
      CHECK(bad == 0 && checked > 0);
    }

  // ---- no step above the fade limits: engage, slice boundaries (1/32 on a 437 Hz sine: every boundary a jump),
  // the gate's close, the release
  {
    float natural = 0.5f * 6.283185307f * g_f / 48000.0f;
    for (int gate : {0, 127})
      for (int p4 : {0, 40, 127}) {
        Rig g(1000, sine);
        control.knob[0][3] = kL32, control.knob[0][5] = (uint8_t)gate, control.knob[0][4] = (uint8_t)p4;
        g.run(96000 + 1234);   // off the grid
        g.p2(127), g.run(30000), g.p2(0), g.run(20000);
        float m = max_step(g.y, 96000, (long)g.y.size());
        printf("test_stutter: steps, gate %d P4 %d: max %.4f (the sine's own %.4f)\n", gate, p4, m, natural);
        CHECK(m < natural + 0.02f);
        long n = (long)g.y.size(), bad = 0;
        for (long i = n - 10000; i < n; ++i) bad += g.y[i] != g.dry[i];
        CHECK(bad == 0);   // released: dry again, exact
      }
  }

  // ---- the curves: decay (P4 1..63, dB per repeat) and pitch down (P4 64..127, semitones per repeat)
  {
    CHECK(Stutter::decay_db(0) == 0 && Stutter::semis(0) == 0 && Stutter::decay_db(63) == 12.0f && Stutter::semis(64) == 0.25f &&
          Stutter::semis(127) == 12.0f && Stutter::decay_db(64) == 0 && Stutter::gate(0) == 1.0f &&
          std::fabs(Stutter::gate(127) - 0.1f) < 1e-6f);
    // decay: a constant input, P4 32 (6.1 dB a repeat): repeat n's level -n x 6.1 dB
    Rig g(1000, [](long) { return 0.5f; });
    control.knob[0][3] = kL16, control.knob[0][4] = 32;
    long L = 6000, gp = 96000;
    g.run(gp), g.p2(127), g.run(4 * L);
    float db = Stutter::decay_db(32);
    bool ok = true;
    for (int rep = 0; rep < 4; ++rep) {
      float v = g.y[gp + rep * L + L / 2], want = 0.5f * std::pow(10.0f, -rep * db / 20.0f);
      printf("test_stutter: decay %.2f dB: repeat %d level %.5f (want %.5f)\n", db, rep, v, want);
      ok &= std::fabs(v - want) < 1e-5f;
    }
    CHECK(ok);
    // pitch: P4 127 = 12 semitones a repeat: repeat 1 at half speed reads the slice's first half (frame 2m = sample m)
    Rig p(1000);
    control.knob[0][3] = kL16, control.knob[0][4] = 127;
    p.run(gp), p.p2(127), p.run(3 * L);
    long bad = 0;
    for (long m = 100; m < L / 2; ++m) bad += p.y[gp + L + 2 * m] != ramp(gp - L + m);
    for (long m = 100; m < L / 4; ++m) bad += p.y[gp + 2 * L + 4 * m] != ramp(gp - L + m);   // repeat 2: a quarter
    // 0.25 semitones (P4 64): repeat 1 at 2^(-1/48)
    Rig q(1000);
    control.knob[0][3] = kL16, control.knob[0][4] = 64;
    q.run(gp), q.p2(127), q.run(2 * L);
    float r = std::exp2(-0.25f / 12.0f), pos = 3000 * r;
    long i0 = (long)pos;
    float want = ramp(gp - L + i0) + (ramp(gp - L + i0 + 1) - ramp(gp - L + i0)) * (pos - (float)i0);
    printf("test_stutter: pitch: repeat 1/2 at half/quarter speed: %ld frames off; 0.25 st: %.6f (want %.6f)\n", bad,
           q.y[gp + L + 3000], want);
    CHECK(bad == 0 && std::fabs(q.y[gp + L + 3000] - want) < 1e-6f);
    // gate P5 127: each repeat after the first sounds for 10 % of its length (+ the 2 ms close), then silence
    Rig t(1000);
    control.knob[0][3] = kL16, control.knob[0][5] = 127;
    t.run(gp), t.p2(127), t.run(3 * L);
    long open = 0, shut = 0;
    for (long j = Stutter::kXf + 1; j < 600; ++j) open += t.y[gp + L + j] == ramp(gp - L + j);
    for (long j = 600 + Stutter::kXf; j < L; ++j) shut += t.y[gp + L + j] != 0.0f;
    CHECK(open == 600 - Stutter::kXf - 1 && shut == 0);
  }

  // ---- p-lock engage per step through 0x20 (track 12: FX1 TYPE / P1 = word 29, P2 / P3 = 38, P4 / P5 = 39): a
  // 16-step pattern at 120 BPM, P2 locked to 127 on steps 5 and 13 (index 4, 12), the report at each step's start
  {
    auto report = [](int p2, int p3) {
      uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x20, 1, 12, 0, 0, kStutter, 127, (uint8_t)p2, (uint8_t)p3, 0, 0, 0, 0, 0, 0, 0xF7};
      CHECK(take_sendfx(m, (int)sizeof m));
    };
    Rig g(1000);
    long step = 6000, gp = 96000;
    report(0, kL16);
    g.run(gp);
    for (int s = 0; s < 16; ++s) {
      bool lock = s == 4 || s == 12;
      report(lock ? 127 : 0, kL16);
      g.run(step);
    }
    long bad = 0;
    for (int s = 0; s < 16; ++s) {
      long a = gp + s * step;
      bool lock = s == 4 || s == 12, after = s == 5 || s == 13;
      for (long j = 0; j < step; ++j) {
        if ((lock && j < Stutter::kEnv) || (after && j < Stutter::kEnv)) continue;
        float want = lock ? ramp(a - step + j) : g.dry[a + j];   // the locked step plays the step before it
        bad += g.y[a + j] != want;
      }
    }
    printf("test_stutter: p-locks on steps 5 and 13: %ld frames off\n", bad);
    CHECK(bad == 0);
  }

  // ---- no clock: free run at 120 BPM, the grid anchored at the engage; stopped: the ticks' tempo, free
  {
    Rig g(0);
    control.knob[0][3] = kL16;
    long e = 50000 + 77, L = 6000;
    g.run(e), g.p2(127), g.run(2 * L);
    CHECK(!g.fx->stutter().HasClock());
    long bad = 0;
    for (long j = Stutter::kEnv; j < L; ++j) bad += g.y[e + j] != ramp(e - L + j);
    for (long j = Stutter::kXf + 1; j < L; ++j) bad += g.y[e + L + j] != ramp(e - L + j);
    Rig s(1200);   // 100 BPM ticks, stopped
    clock_in(0xFC);
    control.knob[0][3] = kL16;
    s.run(e), s.p2(127), s.run(L);
    long L2 = 7200;
    CHECK(s.fx->stutter().HasClock() && s.fx->stutter().SliceFrames() == L2);
    for (long j = Stutter::kEnv; j < L; ++j) bad += s.y[e + j] != ramp(e - L2 + j);
    // the clock goes away while running: after 250 ms free, at the last tempo
    Rig v(1200);
    control.knob[0][3] = kL16;
    v.run(96000), v.ticking = false, v.run(13000 + 55), v.p2(127), v.run(L2);
    long ve = 96000 + 13055;
    CHECK(!v.fx->stutter().HasClock() && v.fx->stutter().SliceFrames() == L2);
    for (long j = Stutter::kEnv; j < L2; ++j) bad += v.y[ve + j] != ramp(ve - L2 + j);
    printf("test_stutter: free run (no clock / stopped / clock lost): %ld frames off\n", bad);
    CHECK(bad == 0);
  }

  // ---- CC 120: the repeat drops to dry in 5 ms and stays dry while P2 is held; P2 0 then 127 engages again
  {
    Rig g(1000, sine);
    control.knob[0][3] = kL8;
    g.run(96000), g.p2(127), g.run(20000);
    usleep(150000);   // all_sound_off's 100 ms burst window, after the earlier tests
    CHECK(all_sound_off());
    long k = (long)g.y.size();
    g.run(20000);
    long bad = 0;
    for (long i = k + Stutter::kEnv + 12; i < k + 20000; ++i) bad += g.y[i] != g.dry[i];
    float m = max_step(g.y, k - 100, k + 1000);
    g.p2(0), g.run(12), g.p2(127), g.run(20000);
    CHECK(g.fx->stutter().Engaged());
    printf("test_stutter: CC 120: %ld frames off dry after 5 ms, max step %.4f\n", bad, m);
    CHECK(bad == 0 && m < 0.5f * 6.283185307f * g_f / 48000.0f + 0.02f);
  }

  // ---- one STUTTER at a time: FX 2 STUTTER too = MISSING, the mix once (not doubled)
  {
    Rig g(1000);
    control.knob[1][0] = kStutter;
    CHECK(state(0) == kStReady && state(1) == kStMissing && stutter_slot(control) == 0);
    g.run(20000);
    long bad = 0;
    for (long i = kFadeFrames; i < 20000; ++i) bad += g.y[i] != g.dry[i];
    CHECK(bad == 0);
    control.knob[0][0] = kChorus;
    CHECK(state(1) == kStReady && stutter_slot(control) == 1);
  }

  // ---- CPU per 12-frame block: the Mac as a proxy for a Pi 4 core (x13.2, as test_send_fx)
  {
    const double kPiRatio = 3.7 / 0.28, kBlockUs = 12 / 48000.0 * 1e6;
    struct C { int p2, p4, p5; const char* name; } cases[] = {
        {0, 0, 0, "dry"}, {127, 0, 0, "engaged"}, {127, 127, 0, "engaged pitch"}, {127, 30, 64, "engaged decay gate"}};
    for (auto& cs : cases) {
      Rig g(1000, sine);
      g.blk = 12;
      control.knob[0][3] = kL32, control.knob[0][4] = (uint8_t)cs.p4, control.knob[0][5] = (uint8_t)cs.p5;
      g.run(48000);
      g.p2(cs.p2);
      float st[12][kStems], o[12][2], m[12][2];
      for (int j = 0; j < 12; ++j)
        for (int v = 0; v < kStems; ++v) st[j][v] = 0.01f * (float)((j * 7 + v * 3) % 11 - 5);
      const int blocks = 40000;
      float acc = 0;
      auto t0 = std::chrono::steady_clock::now();
      for (int b = 0; b < blocks; ++b) {
        g.fx->Process(st, 12, o, control, m);
        acc += m[5][0];
      }
      double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
      printf("test_stutter: CPU %s: %.3f us a 12-frame block on the Mac, ~%.1f us on a Pi 4 core (%.1f %% of %.0f us)%s\n",
             cs.name, us, us * kPiRatio, 100 * us * kPiRatio / kBlockUs, kBlockUs, acc == 12345 ? "!" : "");
    }
  }
  printf("test_stutter: PASS\n");
  return 0;
}

// core/tests/small/test_send_fx_small.cpp - the Teensy's send FX configuration (RYTM_SENDFX_LARGE=0, 16-bit lines;
// the Makefile builds this directory's tests and the core with -DRYTM_SENDFX_LARGE=0): the small set CHORUS COMB
// CRUSH RINGMOD HALO plays, RINGS / CLOUDSEED / STUTTER are MISSING; 0x21 / 0x24 list only the small set; the state
// fits ~16 KB (HALO in the small types' shared block); CHORUS / COMB keep the Pi's knob maps at 48 kHz (COMB's lowest TUNE with full SPREAD inside its
// 1024-sample lines); CRUSH and RINGMOD's responses; HALO's T60 per DECAY, FREEZE, SHIMMER, silence, minutes at full
// knobs, a clean start in a block another type used, All Sound Off; P1 = ROUTE picks the pair; no NaN or runaway.
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "check.h"
#include "fx/send_fx.h"

using namespace core;
using namespace core::fx;

static_assert(!RYTM_SENDFX_LARGE && RYTM_SENDFX_LINE16, "build with -DRYTM_SENDFX_LARGE=0");
static const float kSr = 48000.0f;

static double goertzel(const std::vector<float>& x, size_t from, double f) {
  double w = 2 * M_PI * f / kSr, c = 2 * std::cos(w), s1 = 0, s2 = 0;
  for (size_t i = from; i < x.size(); ++i) {
    double s0 = x[i] + c * s1 - s2;
    s2 = s1, s1 = s0;
  }
  return s1 * s1 + s2 * s2 - c * s1 * s2;
}
static double peak_freq(const std::vector<float>& x, size_t from, double lo, double hi) {
  double best = lo, bp = -1;
  for (double f = lo; f <= hi; f *= 1.0025) {
    double p = goertzel(x, from, f);
    if (p > bp) bp = p, best = f;
  }
  return best;
}
// a mono effect run directly: in -> L, R; k = P2..P5
template <class Fx>
static void run_fx(Fx& fx, const std::vector<float>& in, const uint8_t* k, std::vector<float>* l, std::vector<float>* r) {
  l->assign(in.size(), 0.0f), r->assign(in.size(), 0.0f);
  float o[12][2];
  for (size_t i = 0; i < in.size(); i += 12) {
    int n = (int)std::min<size_t>(12, in.size() - i);
    for (int j = 0; j < n; ++j) o[j][0] = o[j][1] = 0;
    fx.Process(&in[i], n, o, k, kSr, 1.0f);   // a = 1: the smoothers at their targets at once
    for (int j = 0; j < n; ++j) (*l)[i + j] = o[j][0], (*r)[i + j] = o[j][1];
  }
}
static std::vector<float> sine(size_t n, double f, float a) {
  std::vector<float> x(n);
  for (size_t i = 0; i < n; ++i) x[i] = a * (float)std::sin(2 * M_PI * f * i / kSr);
  return x;
}
static double rms(const std::vector<float>& x, size_t from) {
  double s = 0;
  for (size_t i = from; i < x.size(); ++i) s += (double)x[i] * x[i];
  return std::sqrt(s / (double)(x.size() - from));
}

int main() {
  // ---- the set: what plays, what is MISSING, the lists
  for (int id : {1, 2, 16, 17, 18}) CHECK(plays(id) && builtin(id) && labels(id));
  for (int id : {6, 8, 15, 3, 13}) CHECK(!plays(id) && !labels(id));
  CHECK(names_total() == 6);   // OFF CHORUS COMB CRUSH RINGMOD HALO (no user slot loaded)
  {
    uint8_t m[160];
    int n = sysex_names(m, true, names_total(), 0, names_total());
    CHECK(n == 9 + 6 * 14 && m[7] == 6);
    const uint8_t want[6] = {0, 1, 2, 16, 17, 18};
    const char* nm[6] = {"OFF", "CHORUS", "COMB", "CRUSH", "RINGMOD", "HALO"};
    for (int e = 0; e < 6; ++e) CHECK(m[8 + e * 14] == want[e] && memcmp(&m[9 + e * 14], nm[e], strlen(nm[e])) == 0);
    CHECK(sysex_labels(m, kCrush) == 24 && memcmp(&m[7], "RATE", 4) == 0 && memcmp(&m[11], "BITS", 4) == 0);
    CHECK(sysex_labels(m, kRingMod) == 24 && memcmp(&m[7], "FREQ", 4) == 0 && memcmp(&m[19], "LFO", 3) == 0);
    CHECK(sysex_labels(m, kComb) == 24 + 2 * 8 && sysex_labels(m, kRings) == 0);
    // HALO: DCY MOD TONE SHIM, one zone: DECAY 127 = FRZ (not stepped: the dial's number below it)
    CHECK(sysex_labels(m, kHalo) == 24 + 8 && memcmp(&m[7], "DCY", 3) == 0 && memcmp(&m[19], "SHIM", 4) == 0);
    CHECK(m[23] == 0 && m[24] == 127 && memcmp(&m[26], "FRZ", 3) == 0);
    CHECK(type_from_name("halo") == kHalo && strcmp(type_name(kHalo), "HALO") == 0);
  }
  memset((void*)&control, 0, sizeof control);
  char r[400];
  CHECK(command("sendfx=1:rings", r, sizeof r) && state(0) == kStMissing && strstr(r, "MISSING"));
  CHECK(command("sendfx=1:crush", r, sizeof r) && state(0) == kStReady && !strstr(r, "MISSING"));
  CHECK(command("sendfx=2:ringmod", r, sizeof r) && state(1) == kStReady);
  memset((void*)&control, 0, sizeof control);

  // ---- the memory: ~16 KB
  printf("test_send_fx_small: SendFx %zu bytes (Chorus %zu, Comb %zu, Crush %zu, RingMod %zu, Halo %zu: one block of %zu a slot)\n",
         sizeof(SendFx), sizeof(Chorus), sizeof(Comb), sizeof(Crush), sizeof(RingMod), sizeof(Halo), sizeof(Small));
  CHECK(sizeof(SendFx) <= 17 * 1024 && sizeof(Comb) < 4300 && sizeof(Chorus) < 4300 && sizeof(Halo) <= 8400);

  std::vector<float> l, rr;
  // ---- CHORUS: the Pi's map; the longest read (P4 delay 25 ms + 6 ms depth) inside the 2048 line
  {
    Chorus c;
    c.Clear();
    std::vector<float> x(48000, 0.0f);
    x[0] = 0.5f;
    const uint8_t k[4] = {0, 0, 127, 0};   // slowest rate, 25 % depth, 25 ms delay
    run_fx(c, x, k, &l, &rr);
    size_t at = 0;
    for (size_t i = 1; i < 4000; ++i) if (std::fabs(l[i]) > std::fabs(l[at])) at = i;
    printf("test_send_fx_small: chorus 25 ms: the echo at %.2f ms\n", at * 1000.0 / kSr);
    CHECK(at > 0.0230 * kSr && at < 0.0265 * kSr);   // 25 ms - 1.5 ms (the LFO at its phase 0)
  }
  // ---- COMB: P2 tune 55 Hz * 2^(v/24) as the Pi (the first echo of an impulse one period later); the lowest TUNE
  // with full SPREAD (R 3 % lower: 899 samples) inside the 1024-sample lines
  for (int v : {0, 48, 96}) {
    Comb c;
    c.Clear();
    std::vector<float> x(4096, 0.0f);
    x[0] = 0.5f;
    const uint8_t k[4] = {(uint8_t)v, 40, 0, 127};
    run_fx(c, x, k, &l, &rr);
    auto first = [](const std::vector<float>& y) {
      for (size_t i = 0; i < y.size(); ++i)
        if (std::fabs(y[i]) > 1e-3f) return (double)i;
      return -1.0;
    };
    double p = kSr / (55.0 * std::pow(2.0, v / 24.0)), el = first(l), er = first(rr);
    printf("test_send_fx_small: comb P2 %d: echoes at L %.0f, R %.0f samples (want %.1f, %.1f)\n", v, el, er, p,
           p * 1.03);
    CHECK(std::fabs(el - p) <= 1.5 && std::fabs(er - p * 1.03) <= 1.5);
  }
  {   // decay to silence: the 16-bit loop ends at 0 (truncation), no limit cycle
    Comb c;
    c.Clear();
    std::vector<float> x(48000 * 3, 0.0f);
    x[0] = 0.5f;
    const uint8_t k[4] = {24, 0, 0, 0};   // T60 0.2 s
    run_fx(c, x, k, &l, &rr);
    double tail = rms(l, 48000 * 2);
    printf("test_send_fx_small: comb tail after 2 s: rms %g\n", tail);
    CHECK(tail == 0.0);
  }
  // ---- CRUSH
  {
    Crush c;
    c.Clear();
    std::vector<float> x = sine(9600, 200.0, 0.3f);
    const uint8_t k0[4] = {0, 0, 0, 0};
    run_fx(c, x, k0, &l, &rr);
    std::vector<float> d(x.size());
    for (size_t i = 0; i < x.size(); ++i) d[i] = l[i] - x[i];
    double e = rms(d, 4800) / rms(x, 4800);
    printf("test_send_fx_small: crush at 0: the difference from the input %.3f (relative rms)\n", e);
    CHECK(e < 0.15 && rms(rr, 4800) > 0.15);
    c.Clear();
    x = sine(9600, 200.0, 0.01f);
    const uint8_t k1[4] = {0, 127, 0, 0};   // 1 bit: a small input rounds to 0
    run_fx(c, x, k1, &l, &rr);
    CHECK(rms(l, 4800) == 0.0);
    c.Clear();
    x = sine(19200, 5000.0, 0.3f);
    const uint8_t ko[4] = {0, 0, 0, 0}, kc[4] = {0, 0, 0, 127};   // TONE: 18 kHz .. 400 Hz
    run_fx(c, x, ko, &l, &rr);
    double open = rms(l, 9600);
    c.Clear();
    run_fx(c, x, kc, &l, &rr);
    double closed = rms(l, 9600);
    printf("test_send_fx_small: crush tone 5 kHz: open %.4f, closed %.4f (%.1f dB)\n", open, closed,
           20 * std::log10(closed / open));
    CHECK(closed < open * 0.18);
    c.Clear();
    x = sine(19200, 100.0, 0.3f);
    const uint8_t kr[4] = {127, 0, 0, 0};   // hold 64: images of 100 Hz around 750 Hz
    run_fx(c, x, kr, &l, &rr);
    CHECK(goertzel(l, 9600, 650.0) > 1e-3 * goertzel(l, 9600, 100.0));
  }
  // ---- RINGMOD: sidebands at f_in +- f_carrier, no f_in; R a fifth up with full SPRD
  {
    RingMod m;
    m.Clear();
    std::vector<float> x = sine(24000, 1000.0, 0.3f);
    const uint8_t v = 71;   // 10 * 2^(9 * 71 / 127) = 326 Hz
    const double fc = 10.0 * std::pow(2.0, 9.0 * v / 127.0);
    const uint8_t k[4] = {v, 127, 0, 0};
    run_fx(m, x, k, &l, &rr);
    double lo = goertzel(l, 4800, 1000.0 - fc), hi = goertzel(l, 4800, 1000.0 + fc), c0 = goertzel(l, 4800, 1000.0);
    double rlo = goertzel(rr, 4800, 1000.0 - fc * 1.5);
    printf("test_send_fx_small: ringmod %.0f Hz on 1 kHz: sidebands %.3g %.3g, the input %.3g, R's %.3g\n", fc, lo, hi,
           c0, rlo);
    CHECK(lo > 1e3 * c0 && hi > 1e3 * c0 && rlo > 0.1 * lo);
    m.Clear();
    const uint8_t ks[4] = {v, 0, 127, 127};   // square-ish, the LFO fast: bounded, finite
    run_fx(m, x, ks, &l, &rr);
    for (size_t i = 0; i < l.size(); ++i) CHECK(std::isfinite(l[i]) && std::fabs(l[i]) <= 0.3001f);
  }
  // ---- HALO alone
  {
    std::unique_ptr<Halo> hp(new Halo());
    Halo& h = *hp;
    auto noise = [](size_t n, float a, size_t len) {
      std::vector<float> x(n, 0.0f);
      uint32_t r = 7;
      for (size_t i = 0; i < len && i < n; ++i) x[i] = a * (((r = r * 1664525u + 1013904223u) >> 9) / 4194304.0f - 1.0f);
      return x;
    };
    auto clean = [](const std::vector<float>& y) {   // finite, no subnormal
      for (float v : y)
        if (!std::isfinite(v) || std::fpclassify(v) == FP_SUBNORMAL) return false;
      return true;
    };
    // T60 per DECAY: the Schroeder integral of an impulse's response, -5 .. -25 dB x 3; TONE bright (the damping's
    // shortening of the highs aside), MOD lush. Nominal 0.3 s x 100^(v / 126).
    for (int v : {0, 32, 64, 100, 126}) {
      h.Clear();
      const double t60 = 0.3 * std::pow(100.0, v / 126.0);
      std::vector<float> x((size_t)(kSr * std::min(40.0, 1.5 * t60 + 1.0)), 0.0f);
      x[0] = 1.0f;
      const uint8_t k[4] = {(uint8_t)v, 64, 127, 0};
      run_fx(h, x, k, &l, &rr);
      std::vector<double> e(x.size());
      double acc = 0;
      for (size_t i = x.size(); i-- > 0;) acc += (double)l[i] * l[i] + (double)rr[i] * rr[i], e[i] = acc;
      size_t i5 = 0, i25 = 0;
      while (i5 < e.size() && e[i5] > e[0] * std::pow(10.0, -0.5)) ++i5;
      while (i25 < e.size() && e[i25] > e[0] * std::pow(10.0, -2.5)) ++i25;
      const double m = 3.0 * (double)(i25 - i5) / kSr;
      printf("test_send_fx_small: halo DECAY %3d: T60 %.2f s (nominal %.2f)\n", v, m, t60);
      CHECK(m > 0.7 * t60 && m < 1.4 * t60 && clean(l) && clean(rr));
    }
    // wide: L and R nearly uncorrelated in the tail
    {
      h.Clear();
      std::vector<float> x = noise(48000 * 2, 0.3f, 4800);
      const uint8_t k[4] = {64, 64, 64, 0};
      run_fx(h, x, k, &l, &rr);
      double ll = 0, rr2 = 0, lr = 0;
      for (size_t i = 9600; i < x.size(); ++i) ll += (double)l[i] * l[i], rr2 += (double)rr[i] * rr[i], lr += (double)l[i] * rr[i];
      const double c = lr / std::sqrt(ll * rr2);
      printf("test_send_fx_small: halo L/R correlation in the tail %.3f\n", c);
      CHECK(std::fabs(c) < 0.3);
    }
    // silence: a short tail ends at exactly 0 (the half floats' smallest normal flushes), nothing left over
    {
      h.Clear();
      std::vector<float> x = noise(48000 * 6, 0.5f, 4800);
      const uint8_t k[4] = {0, 127, 64, 0};
      run_fx(h, x, k, &l, &rr);
      double tail = rms(l, 48000 * 5) + rms(rr, 48000 * 5);
      printf("test_send_fx_small: halo DECAY 0 after 5 s: rms %g\n", tail);
      CHECK(tail == 0.0);
    }
    // FREEZE holds: noise for 0.9 s at DECAY 100, then 127 (input cut): the level 60 s on within 6 dB of 5 s on
    // (measured -2 .. -5 dB: the cubic reads' top loss);
    // with SHIMMER the rising cloud stays bounded at the ceiling
    for (int sh : {0, 127}) {
      h.Clear();
      const size_t n = (size_t)kSr * 62;
      std::vector<float> x = noise(n, 0.3f, n);   // the input keeps going: frozen, none of it may come in
      l.assign(n, 0.0f), rr.assign(n, 0.0f);
      float o[42][2];
      for (size_t i = 0; i < n; i += 42) {
        const uint8_t k[4] = {(uint8_t)(i < 0.9 * kSr ? 100 : 127), 64, 64, (uint8_t)sh};
        int m = (int)std::min<size_t>(42, n - i);
        for (int j = 0; j < m; ++j) o[j][0] = o[j][1] = 0;
        h.Process(&x[i], m, o, k, kSr, 0.002f);
        for (int j = 0; j < m; ++j) l[i + j] = o[j][0], rr[i + j] = o[j][1];
      }
      std::vector<float> a(l.begin() + 48000 * 5, l.begin() + 48000 * 6), b(l.begin() + 48000 * 60, l.begin() + 48000 * 61);
      const double r5 = rms(a, 0), r60 = rms(b, 0);
      float pk = 0;
      for (float v : l) pk = std::max(pk, std::fabs(v));
      printf("test_send_fx_small: halo FREEZE (SHIM %d): rms 5 s %.4f, 60 s %.4f (%+.1f dB), peak %.2f\n", sh, r5, r60,
             20 * std::log10(r60 / r5), pk);
      CHECK(r5 > 0.02 && std::fabs(20 * std::log10(r60 / r5)) < 6.0 && pk < 2.0f && clean(l));
    }
    // SHIMMER: an octave up appears (a 400 Hz tone in: energy around 800 Hz, ~nothing without SHIMMER; a pure tone
    // comes out as 2f and the shifter's window sidebands 2f +- 37.5 Hz, how much of each by the two taps' phase),
    // and the tail still decays (DECAY 64: -60 dB well within 20 s)
    for (int sh : {0, 127}) {
      h.Clear();
      std::vector<float> x = sine(48000 * 20, 400.0, 0.3f);
      for (size_t i = 48000; i < x.size(); ++i) x[i] = 0.0f;
      const uint8_t k[4] = {64, 0, 127, (uint8_t)sh};
      run_fx(h, x, k, &l, &rr);
      std::vector<float> w(l.begin(), l.begin() + 48000);
      double oct = 0;   // the octave's band (800 +- 80 Hz: the shifter's window sidebands) re the tone
      for (double fq = 720.0; fq <= 880.0; fq += 10.0) oct += goertzel(w, 24000, fq);
      oct /= goertzel(w, 24000, 400.0);
      const double end = rms(l, 48000 * 19), start = rms(w, 24000);
      printf("test_send_fx_small: halo SHIM %3d: 720..880 / 400 Hz %.2g, after 19 s %.1f dB\n", sh, oct, 20 * std::log10(end / start + 1e-30));
      CHECK(sh ? oct > 1e-3 : oct < 1e-5);
      CHECK(end < start * 1e-3 && clean(l));
    }
    // minutes at full knobs (MOD, TONE, SHIM 127; DECAY 126 then FREEZE) on full-scale noise: bounded, finite
    for (int dv : {126, 127}) {
      h.Clear();
      std::vector<float> x = noise(48000 * 10, 1.0f, 48000 * 10), y0(42), y1(42);
      float o[42][2], pk = 0;
      bool ok = true;
      for (int s = 0; s < 3 * 60 * 48000; s += 42) {
        const uint8_t k[4] = {(uint8_t)(s < 48000 ? 126 : dv), 127, 127, 127};
        for (int j = 0; j < 42; ++j) o[j][0] = o[j][1] = 0;
        h.Process(&x[s % (48000 * 10 - 42)], 42, o, k, kSr, 0.002f);
        for (int j = 0; j < 42; ++j) {
          ok &= std::isfinite(o[j][0]) && std::isfinite(o[j][1]);
          pk = std::max(pk, std::max(std::fabs(o[j][0]), std::fabs(o[j][1])));
        }
      }
      printf("test_send_fx_small: halo 3 min at full knobs (DECAY %d): peak %.2f\n", dv, pk);
      CHECK(ok && pk < 8.0f);
    }
    // the CPU alone (Mac)
    {
      h.Clear();
      std::vector<float> x = noise(48000 * 4, 0.3f, 48000 * 4);
      const uint8_t k[4] = {100, 64, 64, 64};
      auto t0 = std::chrono::steady_clock::now();
      run_fx(h, x, k, &l, &rr);
      double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (x.size() / 12.0);
      printf("test_send_fx_small: CPU halo alone %.0f ns a 12-frame block (Mac)\n", ns);
    }
  }
  // ---- HALO through SendFx: it starts clean in the block CHORUS used (no leftover read as half floats), a TYPE
  // change away and back, All Sound Off clears its tail
  {
    std::unique_ptr<SendFx> fx(new SendFx(kSr));
    memset((void*)&control, 0, sizeof control);
    for (int t = 0; t < kTracks; ++t) control.send[t][0] = 127;
    float st[42][kStems], o[42][2];
    uint32_t rng = 3;
    auto run = [&](int frames, float amp) {
      double e = 0;
      for (int b = 0; b < frames; b += 42) {
        for (int j = 0; j < 42; ++j)
          for (int s = 0; s < kStems; ++s) st[j][s] = amp * (((rng = rng * 1664525u + 1013904223u) >> 9) / 4194304.0f - 1.0f);
        fx->Process(st, 42, o, control);
        for (int j = 0; j < 42; ++j) {
          CHECK(std::isfinite(o[j][0]) && std::isfinite(o[j][1]));
          e += o[j][0] * o[j][0] + o[j][1] * o[j][1];
        }
      }
      return e;
    };
    control.knob[0][0] = kChorus;
    for (int j = 2; j < 6; ++j) control.knob[0][j] = 127;
    run(48000, 0.5f);                  // CHORUS fills the shared block
    control.knob[0][0] = kHalo;        // to HALO with the input silent: nothing may come out
    control.knob[0][2] = 126, control.knob[0][3] = 64, control.knob[0][4] = 64, control.knob[0][5] = 127;
    run(4800, 0.0f);                   // CHORUS's fade-out tail (its line)
    double e = run(48000, 0.0f);
    printf("test_send_fx_small: halo after chorus, silent input: energy %g\n", e);
    CHECK(e == 0.0);
    run(48000, 0.3f);                  // a long tail
    double before = run(4800, 0.0f);
    control.kill = (uint8_t)(control.kill + 1);   // All Sound Off
    run(4800, 0.0f);                   // out in 5 ms, cleared, back
    double after = run(48000, 0.0f);
    printf("test_send_fx_small: halo All Sound Off: tail energy %g before, %g after\n", before, after);
    CHECK(before > 0.0 && after == 0.0);
    control.knob[0][0] = kComb;        // HALO's block to COMB and back to HALO: both clean
    run(4800, 0.0f);
    CHECK(run(48000, 0.0f) == 0.0);
    control.knob[0][0] = kHalo;
    run(4800, 0.0f);
    CHECK(run(48000, 0.0f) == 0.0 && fx->Faults() == 0);
    memset((void*)&control, 0, sizeof control);
  }
  // ---- through SendFx: P1 = ROUTE, every type at full knobs, clean output; the CPU per 12-frame block
  {
    std::unique_ptr<SendFx> fx(new SendFx(kSr));
    memset((void*)&control, 0, sizeof control);
    for (int t = 0; t < kTracks; ++t) control.send[t][0] = control.send[t][1] = 127;
    float st[12][kStems], o[12][2], mn[12][2];
    uint32_t rng = 1;
    auto block = [&](int frames) {
      double pre = 0, post = 0;
      for (int b = 0; b < frames; b += 12) {
        for (int j = 0; j < 12; ++j)
          for (int s = 0; s < kStems; ++s) st[j][s] = ((rng = rng * 1664525u + 1013904223u) >> 9) / 4194304.0f - 1.0f;
        fx->Process(st, 12, o, control, mn);
        for (int j = 0; j < 12; ++j)
          for (int ch = 0; ch < 2; ++ch) {
            CHECK(std::isfinite(o[j][ch]) && std::fabs(o[j][ch]) <= kLimitFx && std::fabs(mn[j][ch]) <= kLimitMain);
            pre += o[j][ch] * o[j][ch], post += mn[j][ch] * mn[j][ch];
          }
      }
      return std::array<double, 2>{pre, post};
    };
    const uint8_t types[5] = {kChorus, kComb, kCrush, kRingMod, kHalo};
    for (uint8_t t : types)
      for (int route : {0, 127}) {
        for (int s = 0; s < 2; ++s) {
          control.knob[s][0] = s ? (t == kChorus ? kComb : kChorus) : t;
          control.knob[s][1] = (uint8_t)route;
          for (int j = 2; j < 6; ++j) control.knob[s][j] = 127;
        }
        block(4800);   // the fades and the route's crossfade
        auto e = block(4800);
        CHECK(route ? e[0] == 0.0 && e[1] > 0 : e[1] == 0.0 && e[0] > 0);
      }
    CHECK(fx->Faults() == 0);
    // CPU on the Mac (a proxy only: the Teensy's own number is measured on the device, ':sendfx')
    control.knob[0][0] = kChorus, control.knob[1][0] = kComb;
    for (int s = 0; s < 2; ++s) control.knob[s][1] = 0;
    block(4800);
    auto t0 = std::chrono::steady_clock::now();
    block(48000);
    double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / 4000.0;
    printf("test_send_fx_small: CPU chorus + comb %.0f ns a 12-frame block (Mac)\n", ns);
    control.knob[0][0] = kHalo, control.knob[1][0] = kHalo;
    for (int s = 0; s < 2; ++s) control.knob[s][2] = 100, control.knob[s][3] = 64, control.knob[s][4] = 64, control.knob[s][5] = 64;
    block(4800);
    t0 = std::chrono::steady_clock::now();
    block(48000);
    ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / 4000.0;
    printf("test_send_fx_small: CPU halo x2 %.0f ns a 12-frame block (Mac)\n", ns);
    CHECK(fx->Faults() == 0);
  }
  printf("test_send_fx_small: PASS\n");
  return 0;
}

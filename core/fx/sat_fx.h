// core/fx/sat_fx.h - INSERT TYPE SAT (core/fx/insert.h, docs/INSERT-FX.md): a saturator with three characters, its curve
// run at 4x. Allocation-free, no table, no libm call in the audio path (expf, exp2f only when a knob moved; sqrtf and
// fabsf compile to instructions): the curves' sines are a polynomial.
//
//   x -> TAPE's pre-emphasis -> 2x -> 2x -> DRV, the curve, the level -> 1/2 -> 1/2 -> TAPE's de-emphasis and head bump
//     -> TONE, a tilt at 1 kHz -> MIX with x delayed as the wet is
//
// Curves after Airwindows (MIT, Chris Johnson; core/fx/THIRD_PARTY_LICENSES.txt), each scaled to slope 1 at 0:
//   TAPE  ToTape6's "mojo" sin(v m pi/2) / m, m = |v|^(1/4), past a shelf (+6 dB over 2.5 kHz, the highs saturate first)
//         and back, then a head bump at 100 Hz, up to +3 dB with DRV
//   CONS  Console7Channel: sin(u |u|) / |u| x 0.8 + sin(u) x 0.2, u held at +-1.097
//   SOFT  Density: sin(u), u held at +-pi/2
// DRV 0..+24 dB; the level is made up for a -12 dBFS input (as DRIVE, in RMS). DRV's first four steps fade the wet in, so DRV 0
// is the dry signal as MIX 0 is, bit for bit: at rest the dry path is x itself, 0 samples late. Wet playing, it takes
// kLatency (a 5 ms fade, a moving comb: half a period at 1.6 kHz), so MIX in between does not comb, and keeps it kHold past
// the wet's end: a p-lock or an LFO through 0 changes no delay. A CHAR change crossfades the two curves over 5 ms.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace core {
namespace satfx {

constexpr int kChars = 3;
enum Char : uint8_t { kTape = 0, kCons, kSoft };
inline int char_of(int v) { return v < 43 ? kTape : v < 86 ? kCons : kSoft; }   // the CHAR knob's zones
inline const char* char_name(int c) { return c == kTape ? "TAPE" : c == kCons ? "CONS" : "SOFT"; }

constexpr int kLatency = 15;   // frames of the wet path (the filters below: 13 + 2)
constexpr int kBlock = 16;     // frames a run at most (ins::kChunk)
constexpr int kFade = 240;     // a CHAR change, the dry path's change of delay: 5 ms
constexpr int kHold = 12000;   // the dry path's delay stays this long after the wet ended: 250 ms

constexpr float kHalfPi = 1.57079633f;
constexpr float kPreGain = 1.0f;   // TAPE's shelf: +6 dB

// sin(sqrt(q)) / sqrt(q) for q <= (pi/2)^2 (error under 2e-4)
inline float sinc2(float q) { return 1.0f + q * (-0.16666667f + q * (0.0083333333f - q * 0.00019841270f)); }
inline float hold(float x, float l) { return x > l ? l : x < -l ? -l : x; }

// c's curve at u, slope 1 at 0, no division: 0 at 0 exactly
inline float curve_tape(float u) {
  const float v = hold(u * (1.0f / kHalfPi), 1.0f);
  const float m = sqrtf(sqrtf(fabsf(v))), w = v * m * kHalfPi;
  return v * kHalfPi * sinc2(w * w);
}
inline float curve_cons(float u) {
  u = hold(u, 1.097f);
  const float q = u * u, w = u * fabsf(u);
  return u * (0.8f * sinc2(w * w) + 0.2f * sinc2(q));
}
inline float curve_soft(float u) {
  u = hold(u, kHalfPi);
  return u * sinc2(u * u);
}
inline float curve(int c, float u) { return c == kTape ? curve_tape(u) : c == kCons ? curve_cons(u) : curve_soft(u); }

// What the knobs make (insert.h Coefs, when they moved)
struct Coefs {
  int sel;                       // the character the knob names
  float g, comp[kChars];         // DRV; the level per curve
  float mix;                     // MIX, DRV's first steps in it
  float gl, gh;                  // TONE
  float bump;                    // the head bump
  float ca, cb1, cb2, ct;        // one-poles: the shelf, the bump's two, the tilt
};
inline void set(Coefs& k, const float* p, float sr) {
  k.sel = char_of((int)(p[0] * 127.0f + 0.5f));
  k.g = exp2f(4.0f * p[1]);
  for (int c = 0; c < kChars; ++c) {   // a -12 dBFS sine's peak kept, less what a sine gains in RMS on its way to a square
    const float o = curve(c, 0.25f * k.g);
    k.comp[c] = (0.25f / o) * (0.7f + 0.3f * o / (0.25f * k.g));
  }
  const float in = p[1] * (127.0f / 4.0f);
  k.mix = p[3] * (in < 1.0f ? in : 1.0f);
  const float v = p[2] * 127.0f - 64.0f;
  k.gh = exp2f(v * (v > 0.0f ? 1.0f / 63.0f : 1.0f / 64.0f)), k.gl = 1.0f / k.gh;
  k.bump = 0.7f * p[1];
  const float w = 6.2831853f / sr;
  k.ca = 1.0f - expf(-w * 2500.0f), k.ct = 1.0f - expf(-w * 1000.0f);
  k.cb1 = 1.0f - expf(-w * 180.0f), k.cb2 = 1.0f - expf(-w * 60.0f);
}

// One channel. All zero is "not started" (Dsp::Clear): the first run takes the knobs as they are
struct Ch {
  float up1[13], up2[3], dn2e[5], dn2o[3], dn1e[13], dn1o[7];   // the filters' histories
  float dry[kLatency];
  float pre, de, b1, b2, tl;     // the shelf both ways, the bump, the tilt
  float gp, cp[kChars], mp, ep;  // DRV, the levels, the mix and the dry path's delay share as the last run ended
  int32_t hold;                  // frames left of the dry path's delay with no wet playing
  int16_t fade;                  // frames left of a CHAR change
  uint8_t on, from, to;
};

// Halfband FIRs, equiripple, each branch summing to 1/2 (DC gain 1), up to a float's rounding; the centre tap a plain delay.
// 96 kHz: 27 taps, flat to 18 kHz (-0.8 dB at 20), -60 dB from 30 kHz. 192 kHz: 7 taps up, 11 down.
constexpr float kC1[7] = {0.003633923f, -0.010428389f, 0.024089347f, -0.048672920f, 0.093306767f, -0.189914297f, 0.627985569f};
constexpr float kU2[2] = {-0.087857227f, 0.587857227f};
constexpr float kD2[3] = {0.020285310f, -0.117829502f, 0.597544192f};

inline float flush(float x) { return (x > -1e-20f && x < 1e-20f) ? 0.0f : x; }

// x[0..m-1] -> y[0..2m-1]. a: history + x; the outer of the two output rows is the delay (13 at 96 kHz, 3 at 192 kHz)
inline void up1(float* h, const float* x, float* y, int m) {
  float a[13 + kBlock];
  memcpy(a, h, sizeof(float) * 13), memcpy(a + 13, x, sizeof(float) * (unsigned)m);
  for (int i = 0; i < m; ++i) {
    const float* p = a + i;   // p[13] the newest, p[0] the oldest
    y[2 * i] = kC1[0] * (p[13] + p[0]) + kC1[1] * (p[12] + p[1]) + kC1[2] * (p[11] + p[2]) + kC1[3] * (p[10] + p[3]) +
               kC1[4] * (p[9] + p[4]) + kC1[5] * (p[8] + p[5]) + kC1[6] * (p[7] + p[6]);
    y[2 * i + 1] = p[7];
  }
  memcpy(h, a + m, sizeof(float) * 13);
}
inline void up2(float* h, const float* x, float* y, int m) {
  float a[3 + 2 * kBlock];
  memcpy(a, h, sizeof(float) * 3), memcpy(a + 3, x, sizeof(float) * (unsigned)m);
  for (int i = 0; i < m; ++i) {
    const float* p = a + i;
    y[2 * i] = kU2[0] * (p[3] + p[0]) + kU2[1] * (p[2] + p[1]);
    y[2 * i + 1] = p[2];
  }
  memcpy(h, a + m, sizeof(float) * 3);
}
// x[0..2m-1] -> y[0..m-1]: the even inputs through the branch, the odd ones the centre tap
inline void dn2(float* he, float* ho, const float* x, float* y, int m) {
  float e[5 + 2 * kBlock], o[3 + 2 * kBlock];
  memcpy(e, he, sizeof(float) * 5), memcpy(o, ho, sizeof(float) * 3);
  for (int i = 0; i < m; ++i) e[5 + i] = x[2 * i], o[3 + i] = x[2 * i + 1];
  for (int i = 0; i < m; ++i) {
    const float* p = e + i;
    y[i] = 0.5f * (kD2[0] * (p[5] + p[0]) + kD2[1] * (p[4] + p[1]) + kD2[2] * (p[3] + p[2]) + o[i]);
  }
  memcpy(he, e + m, sizeof(float) * 5), memcpy(ho, o + m, sizeof(float) * 3);
}
inline void dn1(float* he, float* ho, const float* x, float* y, int m) {
  float e[13 + kBlock], o[7 + kBlock];
  memcpy(e, he, sizeof(float) * 13), memcpy(o, ho, sizeof(float) * 7);
  for (int i = 0; i < m; ++i) e[13 + i] = x[2 * i], o[7 + i] = x[2 * i + 1];
  for (int i = 0; i < m; ++i) {
    const float* p = e + i;
    y[i] = 0.5f * (kC1[0] * (p[13] + p[0]) + kC1[1] * (p[12] + p[1]) + kC1[2] * (p[11] + p[2]) + kC1[3] * (p[10] + p[3]) +
                   kC1[4] * (p[9] + p[4]) + kC1[5] * (p[8] + p[5]) + kC1[6] * (p[7] + p[6]) + o[i]);
  }
  memcpy(he, e + m, sizeof(float) * 13), memcpy(ho, o + m, sizeof(float) * 7);
}

// DRV ramped g -> g + n dg, the level k -> k + n dk, over n samples: y = the curve c at v
inline void shape(int c, const float* v, float* y, int n, float g, float dg, float k, float dk) {
  switch (c) {
    case kTape:
      for (int i = 0; i < n; ++i) g += dg, k += dk, y[i] = k * curve_tape(g * v[i]);
      break;
    case kCons:
      for (int i = 0; i < n; ++i) g += dg, k += dk, y[i] = k * curve_cons(g * v[i]);
      break;
    default:
      for (int i = 0; i < n; ++i) g += dg, k += dk, y[i] = k * curve_soft(g * v[i]);
      break;
  }
}

// m <= kBlock frames of x -> y (x and y apart)
inline void run(Ch& s, const Coefs& k, const float* x, float* y, int m) {
  if (!s.on) {
    s.on = 1, s.from = s.to = (uint8_t)k.sel, s.gp = k.g, s.mp = k.mix;
    for (int c = 0; c < kChars; ++c) s.cp[c] = k.comp[c];
  }
  if (!s.fade && k.sel != s.to) s.from = s.to, s.to = (uint8_t)k.sel, s.fade = kFade;
  const int f0 = s.fade, f1 = f0 > m ? f0 - m : 0;
  s.fade = (int16_t)f1;
  const float w1 = 1.0f - (float)f1 / kFade;   // the new curve's share at the end of these frames
  const float a = (s.to == kTape ? w1 : 0.0f) + (s.from == kTape ? 1.0f - w1 : 0.0f);   // TAPE's share of the filters
  const float bq = a * kPreGain * (1.0f - k.ca), ide = 1.0f / (1.0f + bq), bh = a * kPreGain;

  float u[kBlock], w[2 * kBlock], v[4 * kBlock], o[4 * kBlock], q[4 * kBlock], t[2 * kBlock];
  float lp = s.pre;
  for (int i = 0; i < m; ++i) {
    lp += k.ca * (x[i] - lp);
    u[i] = x[i] + bh * (x[i] - lp);
  }
  s.pre = flush(lp);
  up1(s.up1, u, w, m);
  up2(s.up2, w, v, 2 * m);
  const int n = 4 * m;
  const float dg = (k.g - s.gp) / (float)n;
  if (!f0) {
    const float c0 = s.cp[s.to];
    shape(s.to, v, o, n, s.gp, dg, c0, (k.comp[s.to] - c0) / (float)n);
  } else {
    const float c0 = s.cp[s.from], c1 = s.cp[s.to];
    shape(s.from, v, o, n, s.gp, dg, c0, (k.comp[s.from] - c0) / (float)n);
    shape(s.to, v, q, n, s.gp, dg, c1, (k.comp[s.to] - c1) / (float)n);
    const float z = 1.0f - (float)f0 / kFade, dz = (w1 - z) / (float)n;
    float r = z;
    for (int i = 0; i < n; ++i) r += dz, o[i] += r * (q[i] - o[i]);
  }
  s.gp = k.g;
  for (int c = 0; c < kChars; ++c) s.cp[c] = k.comp[c];
  dn2(s.dn2e, s.dn2o, o, t, 2 * m);
  dn1(s.dn1e, s.dn1o, t, u, m);

  float de = s.de, b1 = s.b1, b2 = s.b2, tl = s.tl;
  const float ba = a * k.bump;
  for (int i = 0; i < m; ++i) {
    float z = (u[i] + bq * de) * ide;   // the shelf undone exactly (its one-pole's direct term in bq)
    de += k.ca * (z - de);
    b1 += k.cb1 * (z - b1), b2 += k.cb2 * (z - b2);
    z += ba * (b1 - b2);
    tl += k.ct * (z - tl);
    u[i] = k.gl * tl + k.gh * (z - tl);
  }
  s.de = flush(de), s.b1 = flush(b1), s.b2 = flush(b2), s.tl = flush(tl);

  float d[kLatency + kBlock];
  memcpy(d, s.dry, sizeof s.dry), memcpy(d + kLatency, x, sizeof(float) * (unsigned)m);
  memcpy(s.dry, d + m, sizeof s.dry);
  const float m0 = s.mp, m1 = k.mix;
  s.mp = m1;
  if (m0 > 0.0f || m1 > 0.0f) s.hold = kHold;
  else s.hold = s.hold > m ? s.hold - m : 0;
  if (!s.hold && s.ep <= 0.0f) {   // bypass: x itself
    memcpy(y, x, sizeof(float) * (unsigned)m);
    return;
  }
  const float dm = (m1 - m0) / (float)m, de1 = s.hold ? 1.0f / kFade : -1.0f / kFade;
  float mi = m0, e = s.ep;
  for (int i = 0; i < m; ++i) {
    mi += dm, e += de1;
    e = e < 0.0f ? 0.0f : e > 1.0f ? 1.0f : e;
    const float dr = x[i] + e * (d[i] - x[i]);
    y[i] = dr + mi * (u[i] - dr);
  }
  s.ep = e;
}

}  // namespace satfx
}  // namespace core

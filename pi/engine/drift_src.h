// pi/engine/drift_src.h - a stereo stream from a box with its own sample clock, read at another box's: an adaptive resampler in front of a ring.
// The writer (the box's IN completion) and the reader (the master's OUT fill) run at the same nominal rate; the crystals differ by tens of ppm, so
// a plain ring slips a frame every fraction of a second. Instead the reader follows the ring's fill: a fractional read position, a windowed-sinc
// interpolator (kTaps at kPhases, the coefficients linearly interpolated) and a PI loop that holds the fill at kTarget. Portable, no Circle:
// tests/test_drift_src.cpp. One writer, one reader, no locks and no allocation; push and pull never wait.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

class DriftSrc {
 public:
  static constexpr uint32_t kRing = 1024, kMask = kRing - 1;
  static constexpr uint32_t kTaps = 32, kPhases = 256;
  static constexpr uint32_t kTarget = 72;                     // frames from the newest one written to the read point: this ring's whole latency, the filter's delay included
  static constexpr uint32_t kFade = 16;                       // frames of a fade, in and out
  static constexpr uint32_t kFloor = kTaps / 2 + 1;           // the filter reads this far ahead of the read point
  static constexpr uint32_t kLow = kFloor + kFade + 2;        // below it: fade out while there is still something to read, then prime again
  static constexpr uint32_t kHigh = kRing - kTaps - 8;        // above it: the same, nearly full
  static constexpr double kMaxStep = 500e-6;                  // the loop's reach (crystals are within 100 ppm or so of each other)
  static constexpr double kTau = 1.0;                         // the loop's time constant, seconds

  // Once, before the first pull (task level: sinf and a Bessel series over the table)
  void init() {
    const double beta = 9.0, cut = 0.96;   // Kaiser, about 90 dB; cutoff 23 kHz at 48 kHz
    const double i0b = bessel_i0(beta), half = kTaps / 2.0;
    for (uint32_t p = 0; p <= kPhases; ++p) {
      double sum = 0.0, h[kTaps];
      for (uint32_t k = 0; k < kTaps; ++k) {
        double t = (double)k - (double)(kTaps / 2 - 1) - (double)p / kPhases;   // tap k's distance from the read point
        double x = 3.14159265358979 * cut * t, s = fabs(x) < 1e-9 ? 1.0 : sin(x) / x;
        double u = t / half, w = fabs(u) >= 1.0 ? 0.0 : bessel_i0(beta * sqrt(1.0 - u * u)) / i0b;
        h[k] = cut * s * w, sum += h[k];
      }
      for (uint32_t k = 0; k < kTaps; ++k) tab_[p][k] = (float)(h[k] / sum);   // every phase unity at DC: no gain wobble as the phase turns
    }
    reset();
  }
  // Reader's side: forget the state, the next pull primes
  void reset() {
    run_ = false, leaving_ = false, gain_ = 0.0f, frac_ = 0.0, step_ = 0.0, integ_ = 0.0, fill_ = (double)kTarget;
  }

  // The writer: one frame. A full ring drops it (counted): the reader, which owns the read point, sees the fill and starts over.
  void push(float l, float r) {
    uint32_t w = __atomic_load_n(&wr_, __ATOMIC_RELAXED), g = __atomic_load_n(&rd_, __ATOMIC_ACQUIRE);
    if (w - g >= kRing - kTaps) {
      ++full_;
      return;
    }
    l_[w & kMask] = l, r_[w & kMask] = r;
    __atomic_store_n(&wr_, w + 1, __ATOMIC_RELEASE);
  }

  // The reader: n frames (L, R), each block's fill steering the next. Silence while priming; nothing here waits.
  void pull(float (*out)[2], unsigned n) {
    uint32_t w = __atomic_load_n(&wr_, __ATOMIC_ACQUIRE), rd = __atomic_load_n(&rd_, __ATOMIC_RELAXED);
    for (unsigned f = 0; f < n; ++f) {
      uint32_t lvl = w - rd;
      if (!run_) {
        if (lvl < kTarget) {
          out[f][0] = out[f][1] = 0.0f;
          continue;
        }
        rd = w - kTarget, lvl = kTarget;   // a ring left full by a reader that was away: from the newest frames
        run_ = true, leaving_ = false, frac_ = 0.0, fill_ = (double)kTarget;
      }
      if (lvl <= kFloor) {                 // the box stopped faster than a fade: nothing left to read
        ++under_;
        run_ = false, gain_ = 0.0f;
        out[f][0] = out[f][1] = 0.0f;
        continue;
      }
      if (lvl < kLow || lvl > kHigh) {
        if (!leaving_) (lvl < kLow ? ++low_ : ++high_);
        leaving_ = true;
      }
      gain_ += leaving_ ? -1.0f / kFade : 1.0f / kFade;
      gain_ = gain_ < 0.0f ? 0.0f : gain_ > 1.0f ? 1.0f : gain_;
      float ph = (float)frac_ * kPhases;
      uint32_t i = (uint32_t)ph;
      i = i > kPhases - 1 ? kPhases - 1 : i;
      float a = ph - (float)i;
      float h[kTaps], x = 0.0f, y = 0.0f;
      const float* p0 = tab_[i];
      const float* p1 = tab_[i + 1];
      for (uint32_t k = 0; k < kTaps; ++k) h[k] = p0[k] + a * (p1[k] - p0[k]);
      uint32_t at = rd - (kTaps / 2 - 1);
      for (uint32_t k = 0; k < kTaps; ++k) {
        x += h[k] * l_[(at + k) & kMask];
        y += h[k] * r_[(at + k) & kMask];
      }
      out[f][0] = x * gain_, out[f][1] = y * gain_;
      frac_ += 1.0 + step_;
      uint32_t adv = (uint32_t)frac_;
      frac_ -= adv, rd += adv;
      if (leaving_ && gain_ == 0.0f) run_ = false;
    }
    __atomic_store_n(&rd_, rd, __ATOMIC_RELEASE);
    if (!run_) return;
    // The loop: the fill, smoothed past the boxes' 32-frame bursts, against the target; P for the time constant, I for the crystals' difference
    double lvl = (double)(uint32_t)(__atomic_load_n(&wr_, __ATOMIC_ACQUIRE) - rd) - frac_;
    double blk = (double)n / 48000.0;
    fill_ += (lvl - fill_) * (blk / 0.05);
    double e = (fill_ - (double)kTarget) / 48000.0;   // seconds of error
    integ_ += e * blk / (4.0 * kTau * kTau);
    integ_ = integ_ > kMaxStep ? kMaxStep : integ_ < -kMaxStep ? -kMaxStep : integ_;
    double s = e / kTau + integ_;
    step_ = s > kMaxStep ? kMaxStep : s < -kMaxStep ? -kMaxStep : s;
  }

  bool running() const { return run_; }
  double position() const { return (double)__atomic_load_n(&rd_, __ATOMIC_RELAXED) + frac_; }   // the read point, in frames written so far
  double fill() const { return fill_; }                       // frames, smoothed
  double ppm() const { return step_ * 1e6; }                  // the reader's rate against the writer's, ppm: the crystals' difference, once settled
  uint32_t under() const { return under_; }                   // nothing left to read: an abrupt stop
  uint32_t low() const { return low_; }                       // faded out, ring nearly empty
  uint32_t high() const { return high_; }                     // faded out, ring nearly full
  uint32_t full() const { return full_; }                     // frames the writer dropped

 private:
  static double bessel_i0(double x) {
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 40; ++k) {
      t *= (x / (2.0 * k)) * (x / (2.0 * k));
      s += t;
    }
    return s;
  }
  float tab_[kPhases + 1][kTaps];
  float l_[kRing], r_[kRing];
  alignas(64) uint32_t wr_ = 0;
  alignas(64) uint32_t rd_ = 0;
  // the reader's
  bool run_ = false, leaving_ = false;
  float gain_ = 0.0f;
  double frac_ = 0.0, step_ = 0.0, integ_ = 0.0, fill_ = (double)kTarget;
  uint32_t under_ = 0, low_ = 0, high_ = 0;
  volatile uint32_t full_ = 0;
};

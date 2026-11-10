// core/dsp/blocks.h - the DSP library's primitives (the send FX phase, decided 2026-11-05: a library of general
// building blocks in the engine; a Rytm mod sends a graph of them, core/dsp/dsp.h). Each block is a core::proc::Proc:
// it reads its slot's own stereo input (b.sin, what the route's points summed) and writes its own stereo output
// (b.sout), never OUT directly. Nothing here knows what a mod means by an effect: ECHO, CHORUS or COMB are graphs
// and settings of these, kept in the Rytm mod.
//
// Feedback that must close sample by sample (combs, flangers, echoes' repeats) lives inside a block, because a loop
// through route points could only close once per audio block; the route refuses such loops.
//
// Params: 14 bits each (0..16383, the host link's two 7-bit bytes), set through core::proc::set_param from the lock
// service (BASE, LOCK) or a graph's setup. Every param is smoothed where a jump would click. Portable C++17: no
// allocation (a block is built in its slot's memory by placement new), no libc but <math.h>.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "../proc/proc.h"

namespace core {
namespace dsp {

constexpr float kRate = 48000.0f;
constexpr float kPi = 3.14159265358979f;

// a 14-bit param as 0..1
inline float unit(int v) { return (float)(v < 0 ? 0 : v > 16383 ? 16383 : v) * (1.0f / 16383.0f); }
// a 14-bit param mapped exponentially onto lo..hi (lo, hi > 0)
inline float expmap(int v, float lo, float hi) { return lo * powf(hi / lo, unit(v)); }
// a 14-bit param's zone: 0..n-1 (n equal parts of the range)
inline int zone(int v, int n) {
  int z = (int)(unit(v) * (float)n);
  return z >= n ? n - 1 : z;
}
// keeps a feedback path out of denormals (a tail decaying for minutes would otherwise slow the core)
inline float undenormal(float x) { return fabsf(x) < 1e-20f ? 0.0f : x; }
// a soft clip that is the identity near 0 and saturates at +-1
inline float soft(float x) {
  if (x > 3.0f) return 1.0f;
  if (x < -3.0f) return -1.0f;
  float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);   // tanh's Pade approximant, exact at +-3
}

// a one-pole smoother toward a target, per sample (time constant ~ms)
struct Smooth {
  float v = 0.0f, k = 0.0f;
  void time(float ms) { k = 1.0f - expf(-1.0f / (ms * 0.001f * kRate)); }
  float step(float target) { v += k * (target - v); return v; }
};

// one-pole low-pass and high-pass (in the delay's loop)
struct OnePole {
  float z = 0.0f;
  float lp(float x, float a) { z += a * (x - z); z = undenormal(z); return z; }
  float hp(float x, float a) { return x - lp(x, a); }
  static float coef(float hz) { float a = 1.0f - expf(-2.0f * kPi * hz / kRate); return a > 1.0f ? 1.0f : a; }
};

// the base of every block: the param store and the stereo I/O
class Block : public proc::Proc {
 public:
  static constexpr int kMaxParams = 8;
  void set_param(int param, int value14) override {
    if (param >= 0 && param < kMaxParams) p_[param] = (uint16_t)(value14 < 0 ? 0 : value14 > 16383 ? 16383 : value14);
  }
  int param(int i) const { return p_[i]; }

 protected:
  uint16_t p_[kMaxParams] = {};
  // the slot's input sample (0 when the slot has no input channel)
  static float in(const proc::Block& b, int k, int f) { return b.sin[k] ? b.sin[k][f] : 0.0f; }
  static void out(const proc::Block& b, int k, int f, float v) { if (b.sout[k]) b.sout[k][f] = v; }
};

// ---- DELAY: a stereo delay line with its own feedback loop ------------------------------------------------------------
// P0 TIME 0.05 ms .. 2 s (exponential), P1 FEEDBACK 0 .. 0.995, P2 DAMP low-pass in the loop 20 kHz (16383) .. 500 Hz
// (0), P3 TONE high-pass in the loop 10 Hz (0) .. 2 kHz, P4 DRIVE saturation in the loop (0 clean), P5 MOD DEPTH 0 ..
// 10 ms, P6 MOD RATE 0.02 .. 10 Hz, P7 SPREAD 0 = each side feeds itself .. 1 = each side feeds the other (ping-pong).
// The output is the line's output (wet only). Covers echo, tape echo, ping-pong, chorus, flanger, comb.
class Delay : public Block {
 public:
  static constexpr int kLen = 96000 + 1024;   // 2 s at 48 kHz, plus the modulation's reach
  Delay() {
    memset(buf_, 0, sizeof buf_);
    p_[0] = 11000; p_[1] = 6000; p_[2] = 16383; p_[3] = 0; p_[4] = 0; p_[5] = 0; p_[6] = 6000; p_[7] = 0;
    time_.time(60.0f);
    fb_.time(10.0f);
    time_.v = target_samples();
  }
  const char* name() const override { return "DELAY"; }
  void link_reset() override { memset(buf_, 0, sizeof buf_); lp_[0] = lp_[1] = hp_[0] = hp_[1] = OnePole(); }
  void process(const proc::Block& b) override {
    float t_target = target_samples(), fb_target = unit(p_[1]) * 0.995f;
    float damp = OnePole::coef(expmap(p_[2], 500.0f, 20000.0f)), tone = OnePole::coef(expmap(p_[3], 10.0f, 2000.0f));
    float drive = unit(p_[4]), dgain = 1.0f + 7.0f * drive;
    float depth = unit(p_[5]) * 0.010f * kRate, rate = expmap(p_[6], 0.02f, 10.0f) / kRate;
    float spread = unit(p_[7]);
    for (int f = 0; f < b.frames; ++f) {
      float t = time_.step(t_target), g = fb_.step(fb_target);
      phase_ += rate;
      if (phase_ >= 1.0f) phase_ -= 1.0f;
      float m = sinf(2.0f * kPi * phase_);
      float y[2];
      for (int k = 0; k < 2; ++k) {
        float d = t + depth * (1.0f + (k ? -m : m)) * 0.5f;   // the two sides modulated in opposite directions
        y[k] = read(k, d);
      }
      for (int k = 0; k < 2; ++k) {
        float fbk = (1.0f - spread) * y[k] + spread * y[1 - k];
        fbk = hp_[k].hp(lp_[k].lp(fbk, damp), tone);
        if (drive > 0.0f) fbk = soft(fbk * dgain) / soft(dgain);
        float w = in(b, k, f) + g * fbk;
        buf_[k][w_] = undenormal(w > 4.0f ? 4.0f : w < -4.0f ? -4.0f : w);
        out(b, k, f, y[k]);
      }
      if (++w_ >= kLen) w_ = 0;
    }
  }

 private:
  float buf_[2][kLen];
  int w_ = 0;
  float phase_ = 0.0f;
  Smooth time_, fb_;
  OnePole lp_[2], hp_[2];
  float target_samples() const {
    float s = expmap(p_[0], 0.05f, 2000.0f) * 0.001f * kRate;
    return s < 1.0f ? 1.0f : s > 96000.0f ? 96000.0f : s;
  }
  // the line's sample d samples ago (linear interpolation; d >= 1)
  float read(int k, float d) const {
    if (d < 1.0f) d = 1.0f;
    if (d > (float)(kLen - 2)) d = (float)(kLen - 2);
    float pos = (float)w_ - d;
    if (pos < 0.0f) pos += (float)kLen;
    int i = (int)pos;
    float fr = pos - (float)i;
    int j = i + 1 >= kLen ? 0 : i + 1;
    return buf_[k][i] + fr * (buf_[k][j] - buf_[k][i]);
  }
};

// ---- FILTER: a stereo state-variable filter (topology-preserving transform) ------------------------------------------
// P0 MODE (zones) LP, HP, BP, NOTCH, P1 CUTOFF 20 Hz .. 20 kHz (exponential), P2 RESONANCE Q 0.5 .. 20, P3 DRIVE
// before the filter (0 clean).
class Filter : public Block {
 public:
  Filter() { p_[0] = 0; p_[1] = 16383; p_[2] = 2000; p_[3] = 0; cut_.time(5.0f); cut_.v = target(); }
  const char* name() const override { return "FILTER"; }
  void link_reset() override { memset(s_, 0, sizeof s_); }
  void process(const proc::Block& b) override {
    int mode = zone(p_[0], 4);
    float q = expmap(p_[2], 0.5f, 20.0f), kk = 1.0f / q;
    float drive = unit(p_[3]), dgain = 1.0f + 7.0f * drive;
    float ct = target();
    for (int f = 0; f < b.frames; ++f) {
      float fc = cut_.step(ct);
      float g = tanf(kPi * fc / kRate);
      float a1 = 1.0f / (1.0f + g * (g + kk)), a2 = g * a1, a3 = g * a2;
      for (int c = 0; c < 2; ++c) {
        float x = in(b, c, f);
        if (drive > 0.0f) x = soft(x * dgain) / soft(dgain);
        float v3 = x - s_[c][1];
        float v1 = a1 * s_[c][0] + a2 * v3, v2 = s_[c][1] + a2 * s_[c][0] + a3 * v3;
        s_[c][0] = undenormal(2.0f * v1 - s_[c][0]);
        s_[c][1] = undenormal(2.0f * v2 - s_[c][1]);
        float lp = v2, bp = v1, hp = x - kk * v1 - v2;
        float y = mode == 0 ? lp : mode == 1 ? hp : mode == 2 ? bp : lp + hp;
        out(b, c, f, y);
      }
    }
  }

 private:
  float s_[2][2] = {};
  Smooth cut_;
  float target() const { float fc = expmap(p_[1], 20.0f, 20000.0f); return fc > 0.45f * kRate ? 0.45f * kRate : fc; }
};

// ---- SHAPER: saturation, folding and bit / rate reduction -------------------------------------------------------------
// P0 MODE (zones) CLIP (soft clip), FOLD (sine wavefolder), P1 DRIVE gain 1 .. 32 (exponential), P2 BITS 16 (16383)
// .. 1 (0), P3 RATE sample-rate reduction 48 kHz (16383) .. 100 Hz (0), P4 LEVEL output 0 .. 2 (8192 = unity). With
// DRIVE at 0, BITS and RATE at full, it is the identity.
class Shaper : public Block {
 public:
  Shaper() { p_[0] = 0; p_[1] = 0; p_[2] = 16383; p_[3] = 16383; p_[4] = 8192; }
  const char* name() const override { return "SHAPER"; }
  void link_reset() override { hold_[0] = hold_[1] = 0.0f; acc_ = 0.0f; }
  void process(const proc::Block& b) override {
    int mode = zone(p_[0], 2);
    float drive = expmap(p_[1], 1.0f, 32.0f);
    float bits = 1.0f + 15.0f * unit(p_[2]);
    float steps = powf(2.0f, bits - 1.0f);
    float rate = p_[3] >= 16383 ? 1.0f : expmap(p_[3], 100.0f, kRate) / kRate;
    float level = (float)p_[4] / 8192.0f;   // 8192: unity
    for (int f = 0; f < b.frames; ++f) {
      acc_ += rate;
      bool take = acc_ >= 1.0f;
      if (acc_ >= 1.0f) acc_ -= 1.0f;
      for (int c = 0; c < 2; ++c) {
        float x = in(b, c, f) * drive;
        float y;
        if (mode == 0) y = drive > 1.0f ? soft(x) : x;
        else y = sinf(x * (kPi * 0.5f));
        if (bits < 15.99f) y = floorf(y * steps + 0.5f) / steps;
        if (take) hold_[c] = y;
        out(b, c, f, hold_[c] * level);
      }
    }
  }

 private:
  float hold_[2] = {};
  float acc_ = 0.0f;
};

// ---- MOD: ring modulation, amplitude modulation and tremolo by its own oscillator ------------------------------
// P0 MODE (zones) RING (x times the oscillator), AMP (x times 1 - depth x (0.5 - 0.5 osc): AM at audio rates, tremolo
// at low ones), P1 RATE 0.1 Hz .. 5 kHz (exponential), P2 DEPTH 0 .. 1, P3 SHAPE sine (0) .. triangle (8192) .. square
// (16383), P4 PHASE the right side's offset 0 .. 180 degrees.
class Modulator : public Block {
 public:
  Modulator() { p_[0] = 0; p_[1] = 9000; p_[2] = 16383; p_[3] = 0; p_[4] = 0; }
  const char* name() const override { return "MOD"; }
  void link_reset() override { phase_ = 0.0f; }
  void process(const proc::Block& b) override {
    int mode = zone(p_[0], 2);
    float rate = expmap(p_[1], 0.1f, 5000.0f) / kRate, depth = unit(p_[2]), shape = unit(p_[3]), off = unit(p_[4]) * 0.5f;
    for (int f = 0; f < b.frames; ++f) {
      phase_ += rate;
      if (phase_ >= 1.0f) phase_ -= 1.0f;
      for (int c = 0; c < 2; ++c) {
        float ph = phase_ + (c ? off : 0.0f);
        if (ph >= 1.0f) ph -= 1.0f;
        float o = osc(ph, shape);
        float x = in(b, c, f);
        float y = mode == 0 ? x * (1.0f - depth + depth * o) : x * (1.0f - depth * (0.5f - 0.5f * o));
        out(b, c, f, y);
        last_[c] = o;
      }
    }
  }
  // ctl(0) / ctl(1): the left / right oscillator (-1..1) at the end of the block: an LFO for mod points (2026-11-10)
  float ctl(int k) const override { return k == 0 || k == 1 ? last_[k] : 0.0f; }

 private:
  float phase_ = 0.0f, last_[2] = {};
  static float osc(float ph, float shape) {
    float s = sinf(2.0f * kPi * ph);
    float t = ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f;
    float q = ph < 0.5f ? 1.0f : -1.0f;
    return shape < 0.5f ? s + (t - s) * (shape * 2.0f) : t + (q - t) * ((shape - 0.5f) * 2.0f);
  }
};

// ---- WIDTH: stereo width, balance and level -------------------------------------------------------------------------
// P0 WIDTH 0 mono .. 8192 as it is .. 16383 twice the side, P1 BALANCE 0 left .. 8192 centre .. 16383 right, P2 GAIN
// 0 silent .. 8192 unity .. 16383 +6 dB (the route's scale).
class Width : public Block {
 public:
  Width() { p_[0] = 8192; p_[1] = 8192; p_[2] = 8192; g_.time(5.0f); g_.v = 1.0f; }
  const char* name() const override { return "WIDTH"; }
  void process(const proc::Block& b) override {
    float w = (float)p_[0] / 8192.0f, bal = (float)p_[1] / 8192.0f - 1.0f, gt = (float)p_[2] / 8192.0f;
    float gl = bal > 0.0f ? 1.0f - bal : 1.0f, gr = bal < 0.0f ? 1.0f + bal : 1.0f;
    for (int f = 0; f < b.frames; ++f) {
      float g = g_.step(gt);
      float l = in(b, 0, f), r = in(b, 1, f);
      float mid = 0.5f * (l + r), side = 0.5f * (l - r) * w;
      out(b, 0, f, (mid + side) * gl * g);
      out(b, 1, f, (mid - side) * gr * g);
    }
  }

 private:
  Smooth g_;
};

}  // namespace dsp
}  // namespace core

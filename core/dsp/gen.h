// core/dsp/gen.h - the DSP library's sources and envelope (2026-11-10, the remaining ext machines on the library;
// Connor: "use library DSP where possible"): OSC, NOISE and ENV, generic blocks a voice graph chains (OSC -> NOISE ->
// FILTER -> ENV is a subtractive voice; ENV's control output on a mod point moves FILTER's cutoff). Nothing here knows a
// machine: JUNO, DIGIDRUM's tones and the like are graphs and settings of these, kept in the Rytm mods.
//
// Params use the voice blocks' scale (as SAMPLE's, core/dsp/sample.h): 14 bits = the Rytm knob's value x 64 + the
// fraction's top 6 bits, so "value" below is 0..127.98. A note reaches a block as its midi() (a voice machine's trig
// through core/dsp's TRIG, src proc::kSrcRytm, or a USB-A device's note); a note off as 0x80 or velocity 0.
// Sources add their input onto their output at THRU, so a chain mixes: OSC -> NOISE adds the noise to the oscillator.
#pragma once
#include "blocks.h"

namespace core {
namespace dsp {

inline float val(int v) { return (float)v / 64.0f; }   // a voice block's param as its knob value, 0..127.98
// value 0..127 -> seconds lo..hi, exponential
inline float vtime(int v, float lo, float hi) { return lo * powf(hi / lo, val(v) / 127.0f); }

// ---- OSC: an oscillator at the note's pitch ---------------------------------------------------------------------------
// P0 LEV  level, value / 100 (unity at 100)
// P1 WAV  the wave, morphed: 0 sine .. 42 triangle .. 85 saw .. 127 pulse (band-limited saw and pulse)
// P2 TUN  semitones from 64, added to the note's distance from 60 (C4 = 261.63 Hz)
// P3 FIN  cents from 64 x 64 (+-100 over the range)
// P4 PW   the pulse's width: 0 5% .. 64 50% .. 127 95%
// P5 FM   the input L as phase modulation: 0 none .. 127 two cycles at full scale
// P6 SUB  a square an octave down, its level: 0 none .. 127 unity
// P7 THRU the input added onto the output: 0 none .. 127 unity (the FM input too)
// Mono out on L and R. The phase runs free (a note does not reset it).
class Osc : public Block {
 public:
  Osc() { p_[0] = 100 * 64; p_[1] = 85 * 64; p_[2] = 64 * 64; p_[3] = 64 * 64; p_[4] = 64 * 64; p_[5] = 0; p_[6] = 0; p_[7] = 0; }
  const char* name() const override { return "OSC"; }
  void link_reset() override { ph_ = sub_ = 0.0f; }
  void midi(int, uint8_t status, uint8_t d1, uint8_t d2) override {
    if ((status & 0xF0) == 0x90 && d2) note_ = d1;
  }
  void process(const proc::Block& b) override {
    const float semis = (float)note_ - 60.0f + (val(p_[2]) - 64.0f) + (val(p_[3]) - 64.0f) / 64.0f;
    float dt = 261.6256f * exp2f(semis / 12.0f) / kRate;
    if (dt > 0.45f) dt = 0.45f;
    const float lev = val(p_[0]) / 100.0f, w = val(p_[1]) / 127.0f * 3.0f;
    const float pw = 0.05f + 0.9f * val(p_[4]) / 127.0f, fm = val(p_[5]) / 127.0f * 2.0f;
    const float sub = val(p_[6]) / 127.0f, thru = val(p_[7]) / 127.0f;
    for (int f = 0; f < b.frames; ++f) {
      float x = in(b, 0, f);
      float ph = ph_ + fm * x;
      ph -= floorf(ph);
      float y = wave(ph, dt, w, pw);
      if (sub > 0.0f) y += sub * (sub_ < 0.5f ? 1.0f : -1.0f);
      y = y * lev + thru * x;
      out(b, 0, f, y);
      out(b, 1, f, y);
      ph_ += dt;
      if (ph_ >= 1.0f) { ph_ -= 1.0f; sub_ += 0.5f; if (sub_ >= 1.0f) sub_ -= 1.0f; }
    }
  }

 private:
  float ph_ = 0.0f, sub_ = 0.0f;
  uint8_t note_ = 60;
  static float blep(float t, float dt) {   // polyBLEP: the step's residue near a discontinuity
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
  }
  static float wave(float ph, float dt, float w, float pw) {
    float s = sinf(2.0f * kPi * ph);
    float t = ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f;
    float saw = 2.0f * ph - 1.0f - blep(ph, dt);
    if (w <= 1.0f) return s + (t - s) * w;
    if (w <= 2.0f) return t + (saw - t) * (w - 1.0f);
    float q2 = ph + 1.0f - pw;
    q2 -= floorf(q2);
    float pulse = (ph < pw ? 1.0f : -1.0f) + blep(ph, dt) - blep(q2, dt);
    return saw + (pulse - saw) * (w - 2.0f);
  }
};

// ---- NOISE: noise added onto the input ---------------------------------------------------------------------------------
// P0 LEV  level, value / 100 (unity at 100)
// P1 COL  colour: 0 dark (low-passed at 100 Hz) .. 64 white .. 127 bright (high-passed at 8 kHz)
// P2 THRU the input added onto the output: 0 none .. 127 unity
// Mono noise on L and R; the input's L and R pass at THRU.
class Noise : public Block {
 public:
  Noise() { p_[0] = 100 * 64; p_[1] = 64 * 64; p_[2] = 127 * 64; }
  const char* name() const override { return "NOISE"; }
  void process(const proc::Block& b) override {
    const float lev = val(p_[0]) / 100.0f, col = val(p_[1]), thru = val(p_[2]) / 127.0f;
    const float a = col < 64.0f ? OnePole::coef(100.0f * powf(200.0f, col / 64.0f))
                                : OnePole::coef(20.0f * powf(400.0f, (col - 64.0f) / 63.98f));
    for (int f = 0; f < b.frames; ++f) {
      seed_ = seed_ * 1664525u + 1013904223u;
      float n = (float)(int32_t)seed_ * (1.0f / 2147483648.0f);
      if (col < 63.5f) n = pole_.lp(n, a) * (1.0f + (64.0f - col) / 8.0f);   // the low-pass's loss made up, roughly
      else if (col > 64.5f) n = pole_.hp(n, a);
      n *= lev;
      out(b, 0, f, n + thru * in(b, 0, f));
      out(b, 1, f, n + thru * in(b, 1, f));
    }
  }

 private:
  uint32_t seed_ = 22222u;
  OnePole pole_;
};

// ---- ENV: an envelope on each note, as a VCA and a control output -----------------------------------------------------
// P0 ATK  attack 0.5 ms .. 5 s
// P1 DEC  decay 1 ms .. 10 s (to SUS)
// P2 SUS  sustain level 0 .. 127 (unity); held only in MODE ADSR
// P3 REL  release 1 ms .. 10 s, from a note off (ADSR), or from the end of the decay (AD: to silence)
// P4 MODE 0 AD (attack, then decay to SUS and release to 0 at once: a hit; note offs not needed); 1 and up ADSR (held
//         at SUS until the note off)
// P5 VCA  how much the envelope sets the input's level: 0 not at all (the input passes) .. 127 all of it
// P6 VEL  how much the note's velocity sets the envelope's height: 0 none .. 127 all (velocity 127 is unity either way)
// ctl(0): the envelope (0..1, with VEL), as of the end of the block: the mod points' source.
class Env : public Block {
 public:
  Env() { p_[0] = 0; p_[1] = 64 * 64; p_[2] = 0; p_[3] = 32 * 64; p_[4] = 0; p_[5] = 127 * 64; p_[6] = 0; }
  const char* name() const override { return "ENV"; }
  void link_reset() override { stage_ = kOff; e_ = 0.0f; }
  void midi(int, uint8_t status, uint8_t d1, uint8_t d2) override {
    (void)d1;
    uint8_t s = status & 0xF0;
    if (s == 0x90 && d2) { stage_ = kAtk; vel_ = d2; }
    else if ((s == 0x80 || s == 0x90) && stage_ != kOff && val(p_[4]) >= 1.0f) stage_ = kRel;
  }
  void process(const proc::Block& b) override {
    const float ka = coef(vtime(p_[0], 0.0005f, 5.0f)), kd = coef(vtime(p_[1], 0.001f, 10.0f));
    const float kr = coef(vtime(p_[3], 0.001f, 10.0f)), sus = val(p_[2]) / 127.0f;
    const bool adsr = val(p_[4]) >= 1.0f;
    const float vca = val(p_[5]) / 127.0f, vamt = val(p_[6]) / 127.0f;
    const float h = 1.0f - vamt * (1.0f - (float)vel_ / 127.0f);
    for (int f = 0; f < b.frames; ++f) {
      switch (stage_) {
        case kAtk:
          e_ += ka * (1.2f - e_);   // toward past the top, as an analogue attack: it gets there
          if (e_ >= 1.0f) { e_ = 1.0f; stage_ = kDec; }
          break;
        case kDec:
          e_ += kd * (sus - e_);
          if (e_ - sus < 1e-4f) { e_ = sus; stage_ = adsr ? kSus : kRel; }
          break;
        case kSus: e_ = sus; break;
        case kRel:
          e_ += kr * (0.0f - e_);
          if (e_ < 1e-5f) { e_ = 0.0f; stage_ = kOff; }
          break;
        case kOff: break;
      }
      float g = 1.0f - vca + vca * e_ * h;
      out(b, 0, f, in(b, 0, f) * g);
      out(b, 1, f, in(b, 1, f) * g);
    }
    ctl_ = e_ * h;
  }
  float ctl(int k) const override { return k == 0 ? ctl_ : 0.0f; }

 private:
  enum Stage : uint8_t { kOff, kAtk, kDec, kSus, kRel };
  Stage stage_ = kOff;
  float e_ = 0.0f, ctl_ = 0.0f;
  uint8_t vel_ = 127;
  static float coef(float sec) { return 1.0f - expf(-1.0f / (sec * kRate)); }
};

}  // namespace dsp
}  // namespace core

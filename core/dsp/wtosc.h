// core/dsp/wtosc.h - WTOSC, the DSP library's wavetable oscillator (2026-11-10; its oscillator is upstream WAVETABLE's,
// core/wavetable/wavetable.cpp render_add on trigcondition/sidecar-firmware dev, one note of it: Connor 15:44, library
// extensions that are not specific to a mod). A band-limited wavetable read at the note's pitch, the frame position
// morphing between neighbours, a warp (SYNC, PWM, FM), unison up to 7 oscillators, a sine sub an octave down. The
// tables are upstream's (core/wavetable/tables.*, vendored): the built-ins BASIC PWM VOCAL ORGAN FM SYNC, 64 frames
// each, mip levels an octave apart, generated once at dsp::begin. Tables from the sample pool: not yet. In a POLY
// (poly.h) with SVF and ENV it is upstream's WAVETABLE voice; on its own, one note at a time.
//
// Params (the voice blocks' scale, value 0..127.98):
//   P0 LEV  level, value / 100 (unity at 100)
//   P1 TBL  the table: 0 BASIC, 1 PWM, 2 VOCAL, 3 ORGAN, 4 FM, 5 SYNC (value; above: SYNC)
//   P2 POS  the frame: 0 the first .. 127 the last, morphed; smoothed (upstream's one pole, 0.12 a block, ramped)
//   P3 WARP zones: OFF SYNC PWM FM        P4 AMT  the warp's amount
//   P5 UNI  unison: 0 one oscillator; up, pairs fade in (3, 5, 7) and the detune opens (to 60 cents)
//   P6 SUB  a sine an octave down, its level (x 0.8)
//   P7 TUN  semitones from 64        P8 FIN  cents from 64 x 64 (upstream: none; 64 is the note as played)
//   P9 PM   the input L as phase modulation: 0 none .. 127 two cycles at full scale (upstream: none)
//   P10 THRU the input added onto the output: 0 none .. 127 unity
//   P11 POSM a position offset, not smoothed: 8192 (the 14-bit word) none, 0 -the whole table, 16383 +; a note's own
//            envelope reaches it through a POLY internal mod point (upstream's mod envelope onto POS)
//   P12 PCV  the input R as a position offset, sample by sample: 8192 none, 0 -1 x, 16383 +1 x (the whole table at a
//            full-scale input): an ENV in CV mode before it in a chain is upstream's mod envelope onto POS, exactly
// A note resets the phases (the first oscillator at 0, the unison's at random, the sub at 0) and the DC blocker.
// Mono on L and R.
#pragma once
#include "../wavetable/tables.h"
#include "blocks.h"

namespace core {
namespace dsp {

const float* wt_table(int b);   // built-in table b's 64 frames (kFrameFloats each); null before wt_build
void wt_build();                // control core: the built-ins generated, once

class WtOsc : public Block {
 public:
  static constexpr int kUni = 7;
  static constexpr int kSine = 1024;
  WtOsc() {
    p_[0] = 100 * 64; p_[1] = 0; p_[2] = 0; p_[3] = 0; p_[4] = 0; p_[5] = 0; p_[6] = 0; p_[7] = 64 * 64;
    p_[8] = 64 * 64; p_[9] = 0; p_[10] = 0; p_[11] = 8192; p_[12] = 8192;
    wt_build();
    for (int i = 0; i <= kSine; ++i) sine_[i] = sinf(2.0f * kPi * (float)i / (float)kSine);
    start();
  }
  const char* name() const override { return "WTOSC"; }
  void link_reset() override { start(); }
  void midi(int, uint8_t status, uint8_t d1, uint8_t d2) override {
    if ((status & 0xF0) == 0x90 && d2) {
      note_ = d1;
      start();
      if (!began_) pos_s_ = v(p_[2]) / 127.0f, began_ = true;
    }
  }
  void process(const proc::Block& b) override {
    const int nfr = b.frames;
    int tb = p_[1] / 64;
    const float* data = wt_table(tb >= wt::kBuiltins ? wt::kBuiltins - 1 : tb);
    const uint32_t nf = data ? wt::kBuiltinFrames : 0;
    const float kpos = clamp01(v(p_[2]) / 127.0f);
    const int warp = vzone(p_[3], 4);
    const float amt = clamp01(v(p_[4]) / 127.0f), uni = clamp01(v(p_[5]) / 127.0f), sub = clamp01(v(p_[6]) / 127.0f);
    const float lev = v(p_[0]) / 100.0f, pm = v(p_[9]) / 127.0f * 2.0f, thru = v(p_[10]) / 127.0f;
    const float posm = ((float)p_[11] - 8192.0f) / 8192.0f, pcv = ((float)p_[12] - 8192.0f) / 8192.0f;
    // POS smoothed, ramped over the block (upstream: per voice, 0.12 a block)
    if (!began_) pos_s_ = kpos, began_ = true;
    float p0 = pos_s_, p1 = p0 + (kpos - p0) * 0.12f;
    if (fabsf(p1 - kpos) < 1e-5f) p1 = kpos;
    pos_s_ = p1;
    // the unison: the pairs' weights, the detune
    float w[kUni], cents[kUni];
    int nu = 1;
    float wsum2 = 1.0f;
    w[0] = 1.0f, cents[0] = 0.0f;
    const float det = kDetuneCents * uni * sqrtf(uni);
    for (int j = 1; j < kUni; ++j) {
      float pw = 3.0f * uni - (float)((j - 1) / 2);
      w[j] = pw < 0.0f ? 0.0f : pw > 1.0f ? 1.0f : pw;
      cents[j] = kSpread[j] * det;
      if (w[j] > 0.0f) nu = j + 1, wsum2 += w[j] * w[j];
    }
    const float unorm = 1.0f / sqrtf(wsum2);
    // the warp: its top rate (for the level) and parameters
    float wfac = 1.0f, sync_r = 1.0f, pw_w = 0.5f, fm_i = 0.0f;
    if (warp == 1) sync_r = 1.0f + 7.0f * amt, wfac = sync_r;
    else if (warp == 2) pw_w = 0.5f - 0.46f * amt, wfac = 0.5f / pw_w;
    else if (warp == 3) fm_i = 5.0f * amt * amt, wfac = 1.0f + fm_i;
    // the rates and the level (the highest oscillator's, the warp folded in)
    const float key = (float)note_ + (v(p_[7]) - 64.0f) + (v(p_[8]) - 64.0f) / 64.0f;
    const float inc0 = 440.0f / kRate * exp2f((key - 69.0f) / 12.0f);
    uint32_t iu[kUni];
    float top = 0.0f;
    for (int j = 0; j < nu; ++j) {
      float inc = inc0 * exp2f(cents[j] / 1200.0f);
      top = inc > top ? inc : top;
      iu[j] = (uint32_t)(inc * 4294967296.0f);
    }
    const int lvl = wt::pick_level(top * wfac);
    const uint32_t sub_inc = (uint32_t)(inc0 * 0.5f * 4294967296.0f);
    const int lsz = lvl < wt::kLevels ? wt::kLevelSize[lvl] : 0;
    const int loff = lvl < wt::kLevels ? wt::level_off(lvl) : 0;
    for (int s = 0; s < nfr; ++s) {
      const float x = in(b, 0, s);
      float y = 0.0f;
      if (data && lsz) {
        float pos = p0 + (p1 - p0) * (float)(s + 1) / (float)nfr + posm;
        if (pcv != 0.0f) pos += pcv * in(b, 1, s);
        pos = pos < 0.0f ? 0.0f : pos > 1.0f ? 1.0f : pos;
        float fp = pos * (float)(nf - 1);
        uint32_t fa = (uint32_t)fp;
        if (fa > nf - 1) fa = nf - 1;
        uint32_t fb = fa + 1 < nf ? fa + 1 : fa;
        float ft = fp - (float)fa;
        const float* A = data + (size_t)fa * wt::kFrameFloats + loff;
        const float* B = data + (size_t)fb * wt::kFrameFloats + loff;
        const uint32_t pmo = pm != 0.0f ? (uint32_t)(int32_t)(pm * x * 4294967296.0f) : 0u;
        float o = 0.0f;
        for (int j = 0; j < nu; ++j) {
          if (w[j] <= 0.0f) { ph_[j] += iu[j]; continue; }
          uint32_t ph = ph_[j] + pmo;
          ph_[j] += iu[j];
          float pf = (float)(ph >> 8) * (1.0f / 16777216.0f);
          switch (warp) {
            case 1: pf = pf * sync_r, pf -= (float)(int)pf; break;
            case 2: pf = pf < pw_w ? pf * 0.5f / pw_w : 0.5f + (pf - pw_w) * 0.5f / (1.0f - pw_w); break;
            case 3: {
              uint32_t mp = fm_ph_[j];
              fm_ph_[j] = mp + iu[j];
              pf += fm_i * 0.159155f * sin01((float)(mp >> 8) * (1.0f / 16777216.0f));
              pf -= floorf(pf);
              break;
            }
            default: break;
          }
          float xx = pf * (float)lsz;
          int i = (int)xx;
          float tt = xx - (float)i;
          i &= lsz - 1;
          float a = herm(A + i, tt);
          o += w[j] * (a + (herm(B + i, tt) - a) * ft);
        }
        y = o * unorm;
        float d = y - dc_x_ + 0.9995f * dc_y_;   // the DC blocker (4 Hz): a warp's asymmetry has a mean
        dc_x_ = y, dc_y_ = d, y = d;
      }
      if (sub > 0.0f) {
        y += sub * 0.8f * sin01((float)(sub_ph_ >> 8) * (1.0f / 16777216.0f));
        sub_ph_ += sub_inc;
      }
      y = y * lev + thru * x;
      out(b, 0, s, y);
      out(b, 1, s, y);
    }
  }

 private:
  static constexpr float kDetuneCents = 60.0f;
  static constexpr float kSpread[kUni] = {0.0f, -0.33f, 0.33f, -0.66f, 0.66f, -1.0f, 1.0f};
  uint32_t ph_[kUni], fm_ph_[kUni], sub_ph_ = 0, seed_ = 0x9e3779b9u;
  float dc_x_ = 0.0f, dc_y_ = 0.0f, pos_s_ = 0.0f;
  bool began_ = false;
  int note_ = 60;
  float sine_[kSine + 1];

  void start() {   // a note's start (upstream's): the first phase 0, the unison's at random, the sub 0, the DC blocker
    ph_[0] = 0;
    uint32_t s = seed_ = seed_ * 1664525u + 1013904223u;
    for (int j = 1; j < kUni; ++j) ph_[j] = s = s * 1664525u + 1013904223u;
    for (int j = 0; j < kUni; ++j) fm_ph_[j] = ph_[j];
    sub_ph_ = 0, dc_x_ = dc_y_ = 0.0f;
  }
  static float v(int x) { return (float)x / 64.0f; }
  static float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
  float sin01(float p) const {   // sin(2 pi p), p in [0, 1)
    float x = p * kSine;
    int i = (int)x;
    i &= kSine - 1;
    float t = x - (float)(int)x;
    return sine_[i] + (sine_[i + 1] - sine_[i]) * t;
  }
  static float herm(const float* x, float t) {   // x[-1..2] (upstream's read)
    float xm1 = x[-1], x0 = x[0], x1 = x[1], x2 = x[2];
    float c = (x1 - xm1) * 0.5f, w = x0 - x1, a = c + w + w + (x2 - x0) * 0.5f, bb = c + w + a;
    return ((a * t - bb) * t + c) * t + x0;
  }
};

}  // namespace dsp
}  // namespace core

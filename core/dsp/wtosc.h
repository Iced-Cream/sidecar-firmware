// core/dsp/wtosc.h - WTOSC, the DSP library's wavetable oscillator (2026-11-10, ext's WAVETABLE on the library): a
// band-limited wavetable read at the note's pitch, the frame position morphing between neighbours. The tables are
// upstream's (core/wavetable/tables.*, trigcondition/sidecar-firmware dev, vendored): the built-ins BASIC PWM VOCAL
// ORGAN FM SYNC, 64 frames each, mip levels an octave apart (no aliasing at any pitch), generated once on the control
// core at dsp::begin (boot; wt_build, 63 ms on a PC [test]). Tables from the sample pool: not yet.
//
// Params (the voice blocks' scale, value 0..127.98):
//   P0 LEV  level, value / 100 (unity at 100)
//   P1 TBL  the table: 0 BASIC, 1 PWM, 2 VOCAL, 3 ORGAN, 4 FM, 5 SYNC (value; above: SYNC)
//   P2 POS  the frame: 0 the first .. 127 the last, morphed (a mod point's ENV / LFO sweeps it)
//   P3 TUN  semitones from 64, added to the note's distance from 60 (C4 = 261.63 Hz)
//   P4 FIN  cents from 64 x 64
//   P5 FM   the input L as phase modulation: 0 none .. 127 two cycles at full scale
//   P6 SUB  a sine an octave down, its level
//   P7 THRU the input added onto the output: 0 none .. 127 unity
#pragma once
#include "../wavetable/tables.h"
#include "blocks.h"

namespace core {
namespace dsp {

const float* wt_table(int b);   // built-in table b's 64 frames (kFrameFloats each); null before wt_build
void wt_build();                // control core: the built-ins generated, once

class WtOsc : public Block {
 public:
  WtOsc() {
    p_[0] = 100 * 64; p_[1] = 0; p_[2] = 0; p_[3] = 64 * 64; p_[4] = 64 * 64; p_[5] = 0; p_[6] = 0; p_[7] = 0;
    wt_build();
    pos_.time(5.0f);
  }
  const char* name() const override { return "WTOSC"; }
  void link_reset() override { ph_ = sub_ = 0.0f; }
  void midi(int, uint8_t status, uint8_t d1, uint8_t d2) override {
    if ((status & 0xF0) == 0x90 && d2) note_ = d1;
  }
  void process(const proc::Block& b) override {
    int tb = p_[1] / 64;
    const float* tab = wt_table(tb >= wt::kBuiltins ? wt::kBuiltins - 1 : tb);
    const float semis = (float)note_ - 60.0f + (v(p_[3]) - 64.0f) + (v(p_[4]) - 64.0f) / 64.0f;
    const float inc = 261.6256f * exp2f(semis / 12.0f) / kRate;
    const int lv = wt::pick_level(inc);
    const float lev = v(p_[0]) / 100.0f, fm = v(p_[5]) / 127.0f * 2.0f, sub = v(p_[6]) / 127.0f, thru = v(p_[7]) / 127.0f;
    const float pt = v(p_[2]) / 127.0f * (float)(wt::kBuiltinFrames - 1);
    const int n = lv < wt::kLevels ? wt::kLevelSize[lv] : 0;
    for (int f = 0; f < b.frames; ++f) {
      float x = in(b, 0, f), y = 0.0f;
      float pf = pos_.step(pt);
      if (tab && n) {
        int f0 = (int)pf;
        if (f0 >= wt::kBuiltinFrames - 1) f0 = wt::kBuiltinFrames - 2;
        float t = pf - (float)f0;
        float ph = ph_ + fm * x;
        ph -= floorf(ph);
        float at = ph * (float)n;
        int i = (int)at;
        if (i >= n) i = n - 1;
        float fr = at - (float)i;
        const float* a = wt::level(tab + (size_t)f0 * wt::kFrameFloats, lv) + i;
        const float* c = wt::level(tab + (size_t)(f0 + 1) * wt::kFrameFloats, lv) + i;
        float ya = herm(a, fr), yc = herm(c, fr);
        y = ya + (yc - ya) * t;
      }
      if (sub > 0.0f) y += sub * sinf(2.0f * kPi * sub_);
      y = y * lev + thru * x;
      out(b, 0, f, y);
      out(b, 1, f, y);
      ph_ += inc;
      sub_ += 0.5f * inc;
      if (ph_ >= 1.0f) ph_ -= 1.0f;
      if (sub_ >= 1.0f) sub_ -= 1.0f;
    }
  }

 private:
  float ph_ = 0.0f, sub_ = 0.0f;
  int note_ = 60;
  Smooth pos_;
  static float v(int x) { return (float)x / 64.0f; }
  static float herm(const float* x, float t) {   // x[-1..2] (upstream's read)
    float xm1 = x[-1], x0 = x[0], x1 = x[1], x2 = x[2];
    float c = (x1 - xm1) * 0.5f, w = x0 - x1, a = c + w + w + (x2 - x0) * 0.5f, bb = c + w + a;
    return ((a * t - bb) * t + c) * t + x0;
  }
};

}  // namespace dsp
}  // namespace core

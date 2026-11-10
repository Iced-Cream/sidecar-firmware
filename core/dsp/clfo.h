// core/dsp/clfo.h - CLFO, the DSP library's clock-synced LFO with harmonic outputs (2026-11-10; the wave upstream's
// GRANULAR animates its cloud with, ANIM in core/granular/granular.cpp, as a generic block: Connor 15:44). A cycle of
// whole beats on the Rytm's MIDI clock (core/clock position: the song position while it plays, else on at the last
// tempo), and four control outputs for mod points: three sines at a harmonic of the cycle and a phase each, and a
// constant 1 while it runs. Audio passes through.
//
// Params (the voice scale, value 0..127.98):
//   P0 CYC  0 off (every output 0); 1..127 the cycle in zones of 16: 8BAR 4BAR 2BAR 1BAR 1/2 1/4 1/8 1/16 (32 .. 1/4
//           beats, upstream ANIM's anim_beats)
//   P1 H1   P2 H2   P3 H3   ctl 0..2's harmonic of the cycle (value; defaults 1, 2, 3)
//   P4 PH1  P5 PH2  P6 PH3  ctl 0..2's phase, the 14-bit word / 16384 of a cycle (defaults 0, 1.3 rad, 0.4 rad: ANIM's)
// ctl k (k 0..2) = sin(H_k x the cycle's phase + PH_k); ctl 3 = 1 while CYC is on. Upstream ANIM is depth d on:
//   head  0.8 d ctl0 + 0.2 d ctl2' (H 3, PH 0.4 - the same as ctl 2)     size x 2^(0.6 d ctl1)
//   density x 2^(0.4 d ctl2)     spray + 0.15 d ctl3     (and the input's head further back by d / 2)
// - mod points from here onto GRAIN's HOFS, SIZE, DENS, SPRY in a POLY, and LOFS set to d.
#pragma once
#include "../clock/clock.h"
#include "../platform.h"
#include "blocks.h"

namespace core {
namespace dsp {

class Clfo : public Block {
 public:
  Clfo() {
    p_[0] = 0; p_[1] = 1 * 64; p_[2] = 2 * 64; p_[3] = 3 * 64; p_[4] = 0; p_[5] = 3390; p_[6] = 1043;
  }
  const char* name() const override { return "CLFO"; }
  void process(const proc::Block& b) override {
    // the clock position (upstream: the song position while locked, else on at the last tempo)
    float tp;
    if (clock::position(now_us(), &tp)) ticks_ = tp;
    else {
      float tus = clock::tick_us();
      ticks_ = (float)fmod(ticks_ + b.frames * (1e6 / 48000.0) / (tus > 1 ? tus : 20833.3f), (double)clock::kWrap);
    }
    const float v = (float)p_[0] / 64.0f;
    on_ = v >= 1.0f;
    if (on_) {
      int z = (int)v / 16;
      float beats = 32.0f / (float)(1 << (z > 7 ? 7 : z));
      double per = beats * 24.0;
      float ph = (float)(fmod((double)ticks_, per) / per) * 6.2831853f;
      for (int k = 0; k < 3; ++k) c_[k] = sinf((float)(p_[1 + k] / 64) * ph + (float)p_[4 + k] / 16384.0f * 6.2831853f);
    } else {
      c_[0] = c_[1] = c_[2] = 0.0f;
    }
    for (int f = 0; f < b.frames; ++f) { out(b, 0, f, in(b, 0, f)); out(b, 1, f, in(b, 1, f)); }
  }
  float ctl(int k) const override { return k < 3 ? c_[k < 0 ? 0 : k] : k == 3 && on_ ? 1.0f : 0.0f; }
  float ticks() const { return ticks_; }

 private:
  float ticks_ = 0.0f, c_[3] = {};
  bool on_ = false;
};

}  // namespace dsp
}  // namespace core

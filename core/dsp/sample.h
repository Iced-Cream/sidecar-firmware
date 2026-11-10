// core/dsp/sample.h - SAMPLE, the DSP library's sample player (2026-11-10, with core/samples): plays one entry of the
// project's sample pool on each note its slot gets (a Rytm voice machine's trig through core/dsp's TRIG, or a USB-A
// device's note), in RAM, at the note's pitch. Generic: any mod may place it; DSP SAMPLE is the first.
//
// Params (14 bits; the Rytm's knob words >> 2, so value x 64 + the fraction's top 6 bits):
//   P0 LEV   output level, value / 100 (unity at 100)
//   P1 SMP   the pool entry: value 0 is OFF, value n is entry n - 1 (core/samples' index)
//   P2 TUN   semitones from 64 (fractions included), added to the note's distance from 60 (C4 plays at the file's rate)
//   P3 STA   where it starts, 0..127 over the sample's length
//   P4 END   where it ends, 0..127 over the length (127.98: the last frame); END before STA plays backwards
//   P5 LOOP  two zones: 0 one shot, 1 loop STA..END
//   P6 FIN   cents from 64 x 64 (+-100 over the range)
// A note starts the sample at STA, with a 1 ms fade-in; a new note while it plays starts over (the old one is cut,
// faded over 1 ms). A note off does nothing (the Rytm's amp and the mod decide the length). Output: the sample's L and R
// (a mono file on both), linear interpolation, read where core/samples published it (lock-free; an entry that is not
// READY is silence).
#pragma once
#include "../samples/samples.h"
#include "blocks.h"

namespace core {
namespace dsp {

class Sample : public Block {
 public:
  Sample() {
    p_[0] = 100 * 64; p_[1] = 0; p_[2] = 64 * 64; p_[3] = 0; p_[4] = 16383; p_[5] = 0; p_[6] = 64 * 64;
  }
  const char* name() const override { return "SAMPLE"; }
  void link_reset() override { play_ = false; }
  void midi(int src, uint8_t status, uint8_t d1, uint8_t d2) override {
    if ((status & 0xF0) != 0x90 || !d2) return;
    note_ = d1;
    pending_ = true;
  }
  void process(const proc::Block& b) override {
    samples::View v;
    const int smp = p_[1] / 64;
    const bool have = smp > 0 && samples::get(smp - 1, &v) && v.frames > 1;
    if (pending_) {
      pending_ = false;
      if (have) {
        if (play_) fade_out_ = kFade;
        play_ = true;
        entry_ = smp;
        start(v);
        fade_in_ = 0;
      }
    }
    if (!have || smp != entry_) play_ = false;   // OFF, another entry or gone: silence until the next note
    const float gain = (float)p_[0] / (64.0f * 100.0f);
    const float semis = (float)note_ - 60.0f + ((float)p_[2] / 64.0f - 64.0f) + ((float)p_[6] / 64.0f - 64.0f) / 64.0f;
    const float step = have ? exp2f(semis / 12.0f) * v.rate / kRate : 0.0f;
    for (int f = 0; f < b.frames; ++f) {
      float l = 0.0f, r = 0.0f;
      if (play_) {
        read(v, pos_, &l, &r);
        float g = fade_in_ < kFade ? (float)++fade_in_ / kFade : 1.0f;
        l *= g;
        r *= g;
        pos_ += dir_ * step;
        if ((dir_ > 0 && pos_ >= hi_) || (dir_ < 0 && pos_ <= lo_)) {
          if (loop_ && hi_ - lo_ >= 2.0f) pos_ = dir_ > 0 ? lo_ + (pos_ - hi_) : hi_ - (lo_ - pos_);
          else play_ = false;
        }
      }
      if (fade_out_ > 0) {   // the cut note's last frames, faded
        float ol = 0.0f, orr = 0.0f;
        if (have && old_pos_ >= 0.0f && old_pos_ < (float)(v.frames - 1)) read(v, old_pos_, &ol, &orr);
        float g = (float)fade_out_-- / kFade;
        l += ol * g;
        r += orr * g;
        old_pos_ += old_dir_ * step;
      }
      out(b, 0, f, l * gain);
      out(b, 1, f, r * gain);
    }
  }

 private:
  static constexpr int kFade = 48;   // 1 ms
  bool pending_ = false, play_ = false, loop_ = false;
  int note_ = 60, entry_ = 0, fade_in_ = 0, fade_out_ = 0;
  float pos_ = 0.0f, lo_ = 0.0f, hi_ = 0.0f, dir_ = 1.0f, old_pos_ = -1.0f, old_dir_ = 1.0f;

  void start(const samples::View& v) {
    old_pos_ = pos_;
    old_dir_ = dir_;
    const float last = (float)(v.frames - 1);
    float s = (float)p_[3] / 16383.0f * last, e = (float)p_[4] / 16383.0f * last;
    dir_ = e >= s ? 1.0f : -1.0f;
    lo_ = s < e ? s : e;
    hi_ = s < e ? e : s;
    loop_ = zone(p_[5], 2) == 1;
    pos_ = s;
  }
  static void read(const samples::View& v, float pos, float* l, float* r) {
    if (pos < 0.0f) pos = 0.0f;
    uint32_t i = (uint32_t)pos;
    if (i >= v.frames - 1) i = v.frames - 2;
    float fr = pos - (float)i;
    if (fr > 1.0f) fr = 1.0f;
    const int16_t* a = v.pcm + (size_t)i * v.ch;
    const int16_t* c = a + v.ch;
    float x0 = a[0] + fr * (c[0] - a[0]);
    float x1 = v.ch > 1 ? a[1] + fr * (c[1] - a[1]) : x0;
    *l = x0 * (1.0f / 32768.0f);
    *r = x1 * (1.0f / 32768.0f);
  }
};

}  // namespace dsp
}  // namespace core

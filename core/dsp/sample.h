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
//   P5 LOOP  0 one shot; 1 and up (value >= 1): loop STA..END
//   P6 FIN   cents from 64 x 64 (+-100 over the range)
//   P7 VEL   how much the note's velocity sets the level: 0 none, 127 all (velocity 127 is unity either way)
//   P8 SLN   slices (2026-11-10, CHOP on the library): 0 off; n (1..64) cuts STA..END into n equal slices, and a note
//            plays slice SLC + (note - 60), modulo n, at TUN / FIN only (the note picks the slice, not the pitch)
//   P9 SLC   the slice a note 60 (a plain trig) plays, 0..63 (p-lockable: a slice a step)
// An instrument entry (core/samples: an .sfz / .elmulti; MULTISAMPLE, CHOP's slice maps): a note plays the zone it
// falls in (key, velocity, round robin), at the zone's pitch, level and velocity tracking (times LEV / VEL / TUN /
// FIN), over the zone's offset..end (STA / END within it), looping the zone's loop. Slices are not used on one.
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
    p_[0] = 100 * 64; p_[1] = 0; p_[2] = 64 * 64; p_[3] = 0; p_[4] = 16383; p_[5] = 0; p_[6] = 64 * 64; p_[7] = 0;
    p_[8] = 0; p_[9] = 0;
  }
  const char* name() const override { return "SAMPLE"; }
  void link_reset() override { play_ = false; }
  void midi(int src, uint8_t status, uint8_t d1, uint8_t d2) override {
    if ((status & 0xF0) != 0x90 || !d2) return;
    note_ = d1;
    vel_ = d2;
    pending_ = true;
  }
  void process(const proc::Block& b) override {
    samples::View v, ov;
    const int smp = p_[1] / 64;
    if (pending_) {   // a note: the entry it plays (an instrument's zone, or SMP's sample)
      pending_ = false;
      samples::Zone z;
      const bool zoned = smp > 0 && samples::zone(smp - 1, note_, vel_, &z);
      const int src = zoned ? z.entry : smp - 1;
      if (smp > 0 && samples::get(src, &v) && v.frames > 1) {
        if (play_) { fade_out_ = kFade; old_src_ = src_; }
        play_ = true;
        entry_ = smp;
        src_ = src;
        zoned_ = zoned;
        if (zoned) z_ = z;
        start(v);
        fade_in_ = 0;
      }
    }
    const bool have = smp > 0 && smp == entry_ && samples::get(src_, &v) && v.frames > 1;
    if (!have) play_ = false;   // OFF, another entry or gone: silence until the next note
    const bool ohave = fade_out_ > 0 && samples::get(old_src_, &ov) && ov.frames > 1;
    const float vamt = (float)p_[7] / 16383.0f, vel = (float)vel_ / 127.0f;
    float gain = (float)p_[0] / (64.0f * 100.0f) * (1.0f - vamt * (1.0f - vel));
    float semis = ((float)p_[2] / 64.0f - 64.0f) + ((float)p_[6] / 64.0f - 64.0f) / 64.0f;
    if (zoned_) {
      gain *= z_.gain * (1.0f - z_.veltrack * (1.0f - vel));
      semis += ((float)note_ - (float)z_.keycenter) * z_.keytrack + z_.cents / 100.0f;
    } else if (!slices()) {
      semis += (float)note_ - 60.0f;
    }
    const float step = have ? exp2f(semis / 12.0f) * v.rate / kRate : 0.0f;
    const float ostep = ohave ? exp2f(semis / 12.0f) * ov.rate / kRate : 0.0f;
    for (int f = 0; f < b.frames; ++f) {
      float l = 0.0f, r = 0.0f;
      if (play_) {
        read(v, pos_, &l, &r);
        float g = fade_in_ < kFade ? (float)++fade_in_ / kFade : 1.0f;
        l *= g;
        r *= g;
        pos_ += dir_ * step;
        if (dir_ > 0 && loop_ && pos_ >= lhi_ && lhi_ - llo_ >= 2.0f) pos_ = llo_ + (pos_ - lhi_);
        else if (dir_ < 0 && loop_ && pos_ <= llo_ && lhi_ - llo_ >= 2.0f) pos_ = lhi_ - (llo_ - pos_);
        else if ((dir_ > 0 && pos_ >= hi_) || (dir_ < 0 && pos_ <= lo_)) play_ = false;
      }
      if (fade_out_ > 0) {   // the cut note's last frames, faded
        float ol = 0.0f, orr = 0.0f;
        if (ohave && old_pos_ >= 0.0f && old_pos_ < (float)(ov.frames - 1)) read(ov, old_pos_, &ol, &orr);
        float g = (float)fade_out_-- / kFade;
        l += ol * g;
        r += orr * g;
        old_pos_ += old_dir_ * ostep;
      }
      out(b, 0, f, l * gain);
      out(b, 1, f, r * gain);
    }
  }

 private:
  static constexpr int kFade = 48;   // 1 ms
  bool pending_ = false, play_ = false, loop_ = false, zoned_ = false;
  int note_ = 60, vel_ = 127, entry_ = 0, src_ = -1, old_src_ = -1, fade_in_ = 0, fade_out_ = 0;
  float pos_ = 0.0f, lo_ = 0.0f, hi_ = 0.0f, llo_ = 0.0f, lhi_ = 0.0f, dir_ = 1.0f, old_pos_ = -1.0f, old_dir_ = 1.0f;
  samples::Zone z_{};

  int slices() const { int n = p_[8] / 64; return n > 64 ? 64 : n; }
  // the note's range: a zone's offset..end (STA / END within it), or STA..END of the sample (a slice of it); its loop
  void start(const samples::View& v) {
    old_pos_ = pos_;
    old_dir_ = dir_;
    const float last = (float)(v.frames - 1);
    float a0 = 0.0f, a1 = last;
    if (zoned_) {
      a0 = z_.offset < v.frames ? (float)z_.offset : 0.0f;
      a1 = z_.end != samples::kNone && z_.end < v.frames ? (float)z_.end : last;
    }
    float s = a0 + (float)p_[3] / 16383.0f * (a1 - a0), e = a0 + (float)p_[4] / 16383.0f * (a1 - a0);
    if (int n = zoned_ ? 0 : slices()) {   // slice k of n of s..e (in the play direction)
      int k = (p_[9] / 64 + note_ - 60) % n;
      if (k < 0) k += n;
      float a = s + (e - s) * (float)k / (float)n, z = s + (e - s) * (float)(k + 1) / (float)n;
      s = a;
      e = z;
    }
    dir_ = e >= s ? 1.0f : -1.0f;
    lo_ = s < e ? s : e;
    hi_ = s < e ? e : s;
    llo_ = lo_;
    lhi_ = hi_;
    loop_ = p_[5] >= 64;
    if (zoned_ && z_.loop) {   // the zone's loop (its loop_start..loop_end, or the zone's range)
      loop_ = true;
      if (z_.loop_start != samples::kNone && (float)z_.loop_start < hi_) llo_ = (float)z_.loop_start;
      if (z_.loop_end != samples::kNone && (float)z_.loop_end > llo_ && (float)z_.loop_end <= last) lhi_ = (float)z_.loop_end;
    }
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

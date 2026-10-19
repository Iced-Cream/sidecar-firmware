// One sample-playback voice for the Teensy's SD-card samples, shared by the
// simulator (sim/samplevoice.py through libdaisy) and the Teensy sketch.
// It plays 16-bit PCM it is pointed at (a whole file in memory for now; the
// Teensy will stream from the card behind the same interface - docs/
// sampler-plan.md), mono (stereo files are summed), at any file rate, with the
// Rytm SMPL page's controls: TUN/FIN pitch, STA/END (0..120, as the Rytm's),
// LOP (loop STA..END), LEV.
#pragma once
#include <math.h>
#include <stdint.h>

namespace rytm {

class SampleVoice {
 public:
  void Set(const int16_t* pcm, uint32_t frames, int channels, float rate) {
    pcm_ = pcm;
    frames_ = frames;
    channels_ = channels < 1 ? 1 : channels;
    rate_ = rate;
    playing_ = false;
  }

  void Trigger() {
    playing_ = pcm_ && frames_ > 1;
    first_ = true;
  }

  // semitones: TUN + FIN (0 = the file's pitch); start/end 0..1 of the file;
  // loop between them; writes n floats -1..1 (zeros when silent).
  void Render(float semitones, float start, float end, bool loop, float level,
              float out_rate, float* out, int n) {
    if (!playing_) {
      for (int i = 0; i < n; ++i) out[i] = 0.0f;
      return;
    }
    float a = clamp01(start) * (frames_ - 1), b = clamp01(end) * (frames_ - 1);
    bool rev = b < a;   // END before STA plays backwards, as on the Rytm
    float lo = rev ? b : a, hi = rev ? a : b;
    if (first_) {
      pos_ = rev ? hi : lo;
      first_ = false;
    }
    float step = rate_ / out_rate * powf(2.0f, semitones / 12.0f) * (rev ? -1.0f : 1.0f);
    float g = level / (32768.0f * channels_);
    for (int i = 0; i < n; ++i) {
      if (pos_ < lo || pos_ > hi) {
        if (!loop || hi - lo < 1.0f) {
          playing_ = false;
          for (; i < n; ++i) out[i] = 0.0f;
          return;
        }
        pos_ += rev ? (hi - lo) : -(hi - lo);
      }
      uint32_t k = (uint32_t)pos_;
      float f = pos_ - (float)k;
      uint32_t k2 = k + 1 < frames_ ? k + 1 : k;
      float s0 = 0.0f, s1 = 0.0f;
      for (int c = 0; c < channels_; ++c) {
        s0 += pcm_[k * channels_ + c];
        s1 += pcm_[k2 * channels_ + c];
      }
      out[i] = (s0 + (s1 - s0) * f) * g;
      pos_ += step;
    }
  }

  bool playing() const { return playing_; }

 private:
  static float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
  const int16_t* pcm_ = nullptr;
  uint32_t frames_ = 0;
  int channels_ = 1;
  float rate_ = 48000.0f, pos_ = 0.0f;
  bool playing_ = false, first_ = false;
};

}  // namespace rytm

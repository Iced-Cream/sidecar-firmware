// core/main_out.h - the voices on the Overbridge MAIN L/R inputs, for a stock Rytm (its OS plays only the MAIN
// channels; the mod build plays each voice's own channel): the 8 voices summed at kGain, a fade-in over kFadeFrames
// after each Reset (a new stream, or the switch to main) and a hard limit at kLimit (-12 dBFS) on every sample - the
// Pi engine's out=main (pi/engine/output.*) and the Teensy host build's ':out=main' (teensy/voiceloop) share it.
// Header-only and allocation-free; the board places the loop that calls Next() (the Teensy: program flash).
#pragma once
#include <stdint.h>

namespace core {

class MainOut {
 public:
  static const int kVoices = 8;
  static constexpr float kLimit = 0.25f;           // -12 dBFS
  static const uint32_t kFadeFrames = 48000 / 5;   // 200 ms
  static constexpr float kGain = 0.5f;             // the voices' sum, before the limit

  void Reset() { faded_ = 0; }                     // fade in again from silence
  // One frame of the 8 voices (int16, as core::render_voices makes them) -> MAIN L = R, full scale 1.0, faded
  // and limited.
  float Next(const int16_t* v) { return Sum(v, Gain()); }
  // The two halves, for a board that fades other channels with the same gain (the Pi's out=tracks): the fade's
  // gain for the next frame, then the faded sum, limited.
  float Gain() { return faded_ < kFadeFrames ? (float)faded_++ / kFadeFrames : 1.0f; }
  float Sum(const int16_t* v, float g) {
    int32_t s = 0;
    for (int i = 0; i < kVoices; ++i) s += v[i];
    float x = (float)s * (kGain / 32768.0f) * g;
    x = x > kLimit ? kLimit : x < -kLimit ? -kLimit : x;
    float a = x < 0 ? -x : x;
    if (a > peak_) peak_ = a;
    return x;
  }
  // The same as int16 full scale (|x| <= kLimit, so it always fits).
  int16_t Next16(const int16_t* v) { return (int16_t)(Next(v) * 32768.0f); }
  float TakePeak() {                               // the largest |sample| (0..1) since the last call
    float p = peak_;
    peak_ = 0.0f;
    return p;
  }
  bool Fading() const { return faded_ < kFadeFrames; }

 private:
  uint32_t faded_ = 0;
  float peak_ = 0.0f;
};

}  // namespace core

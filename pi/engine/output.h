// pi/engine/output.h - the engine's output toward the host. Blocks of the 8 voices (int16, as core::render_voices
// makes them) go in on the control core; frames of the host's 12 Overbridge channels (int32, full scale 2^31) come
// out where the transport needs them (the USB OUT completion, a TCP request). On the way: the channel map (MAIN:
// every voice summed onto MAIN L/R, for a stock Rytm; TRACKS: each voice on its track's channel, for the mod
// build), a 200 ms fade-in after each Reset (a new stream) and a hard -12 dBFS limit on every sample (spike3's
// safety stage; the MAIN sum, fade and limit are core/main_out.h's, shared with the Teensy host build). An empty
// FIFO gives silence, never stale data, and is counted.
#pragma once
#include <stdint.h>

#include "main_out.h"
#include "spsc.h"

class Output {
 public:
  enum Mode { kMain = 0, kTracks = 1 };
  static const int kVoices = 8, kChannels = 12;
  static const uint32_t kFifo = 2048;                   // frames
  static constexpr float kLimit = core::MainOut::kLimit;            // -12 dBFS
  static const uint32_t kFadeFrames = core::MainOut::kFadeFrames;   // 200 ms
  static constexpr float kMainGain = core::MainOut::kGain;          // MAIN: the voices' sum, before the limit
  static const uint8_t kVoiceToChannel[kVoices];        // ob_block.h's kVoiceToRytm

  void SetMode(Mode m) { mode_ = m; }
  Mode GetMode() const { return mode_; }
  void Reset();                                         // transport (consumer) side only: a new stream drops the
                                                        // frames left from the last one and fades in again
  uint32_t Level() const { return fifo_.size(); }
  uint32_t Push(const int16_t (*frames)[kVoices], uint32_t n);   // control core: frames taken (FIFO room)
  // transport: n frames; voices (if given): each frame's 8 voices as rendered (int16, before the map, the fade and
  // the limit; zeros on an underflow) - the Sidecar's IN channels 13-20 (sidecar_gadget.h)
  void Pull(int32_t (*out)[kChannels], uint32_t n, int16_t (*voices)[kVoices] = nullptr);
  uint32_t Underflows() const { return under_; }
  float TakePeak();                                     // the largest |sample| (0..1) sent since the last call

 private:
  struct Frame { int16_t v[kVoices]; };
  static float Limit(float x) { return x > kLimit ? kLimit : x < -kLimit ? -kLimit : x; }
  static int32_t To32(float x) { return (int32_t)(x * 2147483648.0f); }
  Spsc<Frame, kFifo> fifo_;
  Mode mode_ = kMain;
  core::MainOut main_;                                  // the fade (both modes), MAIN's sum and limit
  uint32_t under_ = 0;
  float peak_ = 0.0f;
};

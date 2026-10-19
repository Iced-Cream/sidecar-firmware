// pi/engine/output.cpp - see output.h.
#include "output.h"

#include <string.h>

const uint8_t Output::kVoiceToChannel[Output::kVoices] = {4, 7, 5, 8, 10, 9, 11, 6};

void Output::Reset() {
  // Pops what is queued now, as the FIFO's consumer: the producer may keep pushing meanwhile, bounded by the count.
  Frame f;
  for (uint32_t n = fifo_.size(); n && fifo_.pop(&f); --n) {
  }
  main_.Reset();
}

uint32_t Output::Push(const int16_t (*frames)[kVoices], uint32_t n) {
  uint32_t k = 0;
  for (; k < n; ++k) {
    Frame f;
    memcpy(f.v, frames[k], sizeof f.v);
    if (!fifo_.push(f)) break;
  }
  return k;
}

void Output::Pull(int32_t (*out)[kChannels], uint32_t n, int16_t (*voices)[kVoices]) {
  for (uint32_t i = 0; i < n; ++i) {
    for (int c = 0; c < kChannels; ++c) out[i][c] = 0;
    Frame f;
    if (!fifo_.pop(&f)) {
      ++under_;
      if (voices) memset(voices[i], 0, sizeof voices[i]);
      continue;
    }
    if (voices) memcpy(voices[i], f.v, sizeof f.v);
    float g = main_.Gain();
    if (mode_ == kMain) {
      out[i][0] = out[i][1] = To32(main_.Sum(f.v, g));
    } else {
      for (int v = 0; v < kVoices; ++v) {
        float x = Limit(f.v[v] / 32768.0f * g);
        float a = x < 0 ? -x : x;
        if (a > peak_) peak_ = a;
        out[i][kVoiceToChannel[v]] = To32(x);
      }
    }
  }
}

float Output::TakePeak() {
  float p = main_.TakePeak();
  if (peak_ > p) p = peak_;
  peak_ = 0.0f;
  return p;
}

#pragma once
// the audio return (audio_return.h) as the test drives it: g_level on every channel while g_slot is set
#include <stdint.h>
namespace aret {
const int kChannels = 12;
const int kMaxFrames = 64;
inline bool g_slot = false;
inline int32_t g_level = 0;
inline int g_in = 0;
inline bool Wanted() { return g_slot; }
inline void Reset() {}
inline void In(const int32_t (*)[kChannels], int n) { g_in += n; }
inline void Fill(int32_t (*out)[kChannels], int n) {
  if (!g_slot) return;
  for (int f = 0; f < n; ++f)
    for (int c = 0; c < kChannels; ++c) out[f][c] += g_level;
}
inline uint32_t InDropped() { return 0; }
inline uint32_t InMissing() { return 0; }
}  // namespace aret

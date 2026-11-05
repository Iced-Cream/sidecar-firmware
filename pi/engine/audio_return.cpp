// pi/engine/audio_return.cpp - see audio_return.h.
#include "audio_return.h"

#include "proc/proc.h"
#include "route/route.h"
#include "spsc.h"

namespace aret {
namespace {
// Each IN frame carries the stream it came in (Reset counts them): the OUT fill drops what an earlier stream left
struct Frame { int32_t c[kChannels]; uint32_t gen; };
Spsc<Frame, 1024> s_In;
volatile uint32_t s_nGen;
uint32_t s_nDropped, s_nMissing;
// a slot is filled or a route point is open (or still fading out): the IN frames are needed
inline bool active() { return core::proc::any() || core::route::any(); }
inline int32_t sat(float x) {
  float v = x * 2147483648.0f;
  if (v >= 2147483647.0f) return 2147483647;
  if (v <= -2147483648.0f) return -2147483647 - 1;
  return (int32_t)v;
}
}  // namespace

bool Wanted() { return active(); }
void Reset() { s_nGen = s_nGen + 1; }

void In(const int32_t (*in)[kChannels], int n) {
  if (!active()) return;
  for (int f = 0; f < n; ++f) {
    Frame x;
    x.gen = s_nGen;
    for (int c = 0; c < kChannels; ++c) x.c[c] = in[f][c];
    if (!s_In.push(x)) ++s_nDropped;
  }
}

void Fill(int32_t (*out)[kChannels], int n) {
  Frame x;
  if (!active()) {
    while (s_In.pop(&x)) {}   // (a slot emptied, the last point closed: nothing kept for it)
    return;
  }
  if (n > kMaxFrames) n = kMaxFrames;
  static float I[kChannels][kMaxFrames], O[kChannels][kMaxFrames];
  for (int f = 0; f < n; ++f) {
    bool got;
    while ((got = s_In.pop(&x)) && x.gen != s_nGen) {}   // (an earlier stream's: dropped)
    if (!got) {
      ++s_nMissing;
      for (int c = 0; c < kChannels; ++c) x.c[c] = 0;
    }
    for (int c = 0; c < kChannels; ++c) I[c][f] = (float)x.c[c] * (1.0f / 2147483648.0f), O[c][f] = 0.0f;
  }
  core::proc::Block b;
  for (int c = 0; c < kChannels; ++c) b.in[c] = I[c], b.out[c] = O[c];
  b.frames = n;
  core::route::process(b.in, b.out, n);   // the route's points first (core/route), then the processor slots
  core::proc::process(b);
  for (int f = 0; f < n; ++f)
    for (int c = 0; c < kChannels; ++c) {
      if (O[c][f] == 0.0f) continue;
      int64_t v = (int64_t)out[f][c] + sat(O[c][f]);
      out[f][c] = v > 2147483647 ? 2147483647 : v < -2147483648LL ? (int32_t)(-2147483647 - 1) : (int32_t)v;
    }
}

uint32_t InDropped() { return s_nDropped; }
uint32_t InMissing() { return s_nMissing; }
}  // namespace aret

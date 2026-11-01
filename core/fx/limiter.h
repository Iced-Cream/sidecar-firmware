// core/fx/limiter.h - the master's stereo peak limiter (docs/MASTER-FX.md): one gain on both sides, so the image holds, and
// kLook frames of lookahead, so the gain is down before a peak leaves the delay and nothing passes the ceiling. No soft
// knee: under the ceiling a sample only comes out later.
//
// The gain goes down in a straight line: a new peak the line does not reach steepens it or lengthens it (a line only
// ever gets lower, so what an earlier peak asked for still holds), and holds it until that peak has left. Then it goes back
// to 1, a one-pole. O(1) a frame, no search over the delay; a division only when a peak asks for more. Plain floats.
#pragma once

namespace core {
namespace fx {

class Limiter {
 public:
  static constexpr int kLook = 48;                     // frames, 1 ms: all it adds to the signal's path
  static constexpr float kCeiling = 0.98f;             // of full scale
  static constexpr float kRelease = 1.0f / 4800.0f;    // a frame's way back to 1: 100 ms

  void Reset() {
    for (auto& d : d_) d[0] = d[1] = 0.0f;
    w_ = left_ = hold_ = 0;
    g_ = low_ = 1.0f, s_ = 0.0f;
  }

  // a frame in, the frame kLook ago out, in place
  void Frame(float& l, float& r) {
    float a = l < 0.0f ? -l : l, b = r < 0.0f ? -r : r;
    if (a != a) a = 0.0f;   // (a NaN on one side must not hide the other side's peak)
    if (b != b) b = 0.0f;
    const float p = a > b ? a : b;
    float* d = d_[w_];
    const float ol = d[0], or_ = d[1];
    d[0] = l, d[1] = r;
    w_ = w_ + 1 == kLook ? 0 : w_ + 1;
    const float end = g_ + s_ * (float)left_;   // where the line ends: the gain stays there or lower for this peak
    if (p * end > kAim) {
      const float need = kAim / p, s = (need - g_) / (float)kLook;
      if (s < s_) {
        s_ = s, left_ = kLook;
      } else {   // already steep enough: it only has to go on longer
        const int m = s_ < 0.0f ? (int)((need - g_) / s_) + 1 : 0;   // (no line and a need on the gain itself, rounding: nothing to do)
        left_ = m > kLook ? kLook : m;
      }
      hold_ = kLook + 1;
    } else if (p * (end + (1.0f - end) * kRelease * (float)(kLook + 1 - left_)) > kAim) {
      hold_ = kLook + 1;                        // the line is low enough, the release would not be
    }
    if (left_ > 0) {
      g_ += s_;
      if (--left_ == 0) s_ = 0.0f;
    } else if (hold_ == 0 && g_ < 1.0f) {
      g_ += (1.0f - g_) * kRelease;
      if (g_ > 0.9998f) g_ = 1.0f;   // (a float stalls here: -74 dB of a sample, the last step)
    }
    if (hold_ > 0) --hold_;
    if (g_ < low_) low_ = g_;
    l = ol * g_, r = or_ * g_;
  }

  float Gain() const { return g_; }
  float TakeLow() {   // the lowest gain since the last call
    const float x = low_;
    low_ = 1.0f;
    return x;
  }

 private:
  static constexpr float kAim = kCeiling * 0.9997f;   // (the line's rounding and the release's last step, 0.9998 to 1, stay under the ceiling)
  float d_[kLook][2] = {};
  float g_ = 1.0f, s_ = 0.0f, low_ = 1.0f;
  int w_ = 0, left_ = 0, hold_ = 0;
};

}  // namespace fx
}  // namespace core

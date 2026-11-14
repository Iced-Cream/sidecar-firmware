// pi/engine/sysex_asm.h - whole SysEx messages (F0 .. F7) from a byte stream, one byte at a time: the second box's link
// (link1.h). Realtime bytes (F8..FF) inside a message are skipped, any other status byte drops the message so far, and a
// message longer than kMax is dropped whole (counted). Portable (tests/test_sysex_asm.cpp).
#pragma once
#include <stdint.h>

template <unsigned kMax>
class SysExAsm {
 public:
  // One byte; returns the whole message's length (in data()) when b ended one, else 0.
  unsigned feed(uint8_t b) {
    if (b >= 0xF8) return 0;                       // realtime: never part of a message, never ends one
    if (b == 0xF0) {
      if (in_) ++bad_;                             // the one before never ended
      n_ = 0, in_ = true, over_ = false;
    } else if (b >= 0x80 && b != 0xF7) {
      if (in_) ++bad_;
      in_ = false;
      return 0;
    } else if (!in_) {
      return 0;                                    // data outside a SysEx (a note's bytes): not ours
    }
    if (over_) {
      if (b == 0xF7) in_ = false;
      return 0;
    }
    if (n_ >= kMax) {
      over_ = true, ++bad_;
      if (b == 0xF7) in_ = false;
      return 0;
    }
    d_[n_++] = b;
    if (b != 0xF7) return 0;
    in_ = false;
    return n_;
  }
  const uint8_t* data() const { return d_; }
  unsigned bad() const { return bad_; }

 private:
  uint8_t d_[kMax] = {};
  unsigned n_ = 0, bad_ = 0;
  bool in_ = false, over_ = false;
};

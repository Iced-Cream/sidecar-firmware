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

// The MIDI realtime bytes and song position from the same stream (3(c) step 10: the second box's clock, link1.h): F8 FA FB
// FC anywhere, even inside a SysEx (SysExAsm skips them), and F2 with its two data bytes (no running status; any other
// status byte before both came drops it). feed() is true when b ended one: its status in status(), an F2's position (in
// 16ths) in spp().
class RealtimeAsm {
 public:
  bool feed(uint8_t b) {
    if (b >= 0xF8) {
      if (b == 0xF8 || b == 0xFA || b == 0xFB || b == 0xFC) return st_ = b, spp_ = 0, true;
      return false;                                 // (active sensing, reset, undefined: not a clock's)
    }
    if (b & 0x80) {
      have_ = b == 0xF2 ? 0 : -1;                   // another status: an F2 begins, or none waits
      return false;
    }
    if (have_ < 0) return false;
    d_[have_++] = b;
    if (have_ < 2) return false;
    have_ = -1, st_ = 0xF2, spp_ = (uint16_t)(d_[0] | d_[1] << 7);
    return true;
  }
  uint8_t status() const { return st_; }
  uint16_t spp() const { return spp_; }

 private:
  uint8_t d_[2] = {};
  int have_ = -1;
  uint8_t st_ = 0;
  uint16_t spp_ = 0;
};

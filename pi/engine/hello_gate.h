// pi/engine/hello_gate.h - what the engine hears from a box that is not its host link yet: whole device hellos (0x4F, core/device/device.h) and nothing else.
// An Analog Four MKII's USB MIDI until its hello has chosen it: a stock A4 says none, and its notes and clock stay out of the engine. Fed from the USB
// interrupt, one packet's MIDI bytes at a time; no allocation. Portable: tests/test_usb_box.cpp.
#pragma once
#include <stdint.h>

class HelloGate {
 public:
  static constexpr unsigned kMax = 24;   // a hello: 10 bytes and a short name
  // One byte; a whole hello is in data(), n bytes, when it returns n > 0.
  unsigned feed(uint8_t b) {
    if (b == 0xF0) {
      n_ = 0, in_ = true;
    } else if (b >= 0x80 && b != 0xF7) {
      if (b < 0xF8) in_ = false;   // a realtime byte may sit inside a message, anything else ends it
      return 0;
    }
    if (!in_) return 0;
    if (n_ >= kMax) return in_ = false, 0;
    d_[n_++] = b;
    if (b != 0xF7) return 0;
    in_ = false;
    return n_ >= 10 && d_[1] == 0x7D && d_[2] == 0x52 && d_[3] == 0x54 && d_[4] == 0x4F ? n_ : 0;
  }
  const uint8_t* data() const { return d_; }

 private:
  uint8_t d_[kMax] = {};
  unsigned n_ = 0;
  bool in_ = false;
};

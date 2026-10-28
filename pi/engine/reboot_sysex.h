// pi/engine/reboot_sysex.h - the computer's reboot request on the Sidecar's USB MIDI port (sidecar_gadget.cpp, cmdline
// usbreboot=1): the exact SysEx F0 7D 52 54 7E 52 45 42 4F 4F 54 F7 (the host link's prefix, command 7E, "REBOOT").
// A host-link SysEx toward the Rytm is dropped anyway (midi_filter.h), so the request never reaches it. With the
// rpiboot GPIO held low (docs: pi4-rpiboot), the Pi boots again from the computer's rpiboot folder: a new build
// without a power cycle or the network. Fed every packet from the computer; Matched() once a whole request came.
// Portable (Mac-tested: tests/test_reboot_sysex.cpp).
#pragma once
#include <stdint.h>

#include "usbmidi_stream.h"

class RebootSysEx {
 public:
  static const int kLen = 12;
  static const uint8_t* Magic() {
    static const uint8_t m[kLen] = {0xF0, 0x7D, 0x52, 0x54, 0x7E, 'R', 'E', 'B', 'O', 'O', 'T', 0xF7};
    return m;
  }

  // One USB MIDI event packet in.
  void Feed(const uint8_t* p) {
    uint8_t cin = p[0] & 0x0F;
    if (cin != 0x4 && cin != 0x5 && cin != 0x6 && cin != 0x7) {
      if (cin != 0xF) n_ = -1;           // a channel or common message ends a SysEx; real time does not
      return;
    }
    int len = usbmidi_packet_len(p);
    for (int i = 0; i < len; i++) Byte(p[1 + i]);
  }

  bool Matched() const { return matched_; }
  void Clear() { matched_ = false; }

 private:
  void Byte(uint8_t b) {
    if (b == 0xF0) n_ = 0;
    if (n_ < 0) return;
    if (n_ >= kLen || Magic()[n_] != b) {
      n_ = -1;
      return;
    }
    if (++n_ == kLen) {
      matched_ = true;
      n_ = -1;
    }
  }

  int n_ = -1;                           // bytes of the request matched so far; -1: not in one
  bool matched_ = false;
};

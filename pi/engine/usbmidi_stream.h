// pi/engine/usbmidi_stream.h - USB MIDI 1.0 event packets (cable 0) to and from a MIDI byte stream, for the TCP link's
// osupdate=1 path (tcp_link.cpp): the computer's packets for the Rytm go to rytm-emu as bytes ('M'), and rytm-emu's
// bytes (its USB MIDI output, any split) come back to the computer as packets. UsbMidiFromStream keeps running
// status, passes real-time bytes at once (also inside a SysEx) and drops stray data bytes.
// Portable (Mac-tested: tests/test_usbmidi_stream.cpp).
#pragma once
#include <stdint.h>

// The MIDI bytes one event packet carries (0..3), from its CIN.
inline int usbmidi_packet_len(const uint8_t* p) {
  static const uint8_t kLen[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
  return kLen[p[0] & 0x0F];
}

class UsbMidiFromStream {
 public:
  // One byte in; emit(const uint8_t p[4]) is called for each finished packet.
  template <class Emit>
  void Feed(uint8_t b, Emit emit) {
    if (b >= 0xF8) {                     // real time: one byte, wherever it comes
      uint8_t p[4] = {0x0F, b, 0, 0};
      emit(p);
      return;
    }
    if (b == 0xF0) {
      sysex_ = true;
      n_ = 0;
      buf_[n_++] = b;
      return;
    }
    if (b == 0xF7) {
      if (sysex_) {                      // CIN 5, 6 or 7: the last 1, 2 or 3 bytes
        buf_[n_++] = b;
        uint8_t p[4] = {(uint8_t)(0x04 + n_), buf_[0], n_ > 1 ? buf_[1] : (uint8_t)0, n_ > 2 ? buf_[2] : (uint8_t)0};
        emit(p);
      }
      sysex_ = false;
      n_ = 0;
      status_ = 0;
      return;
    }
    if (b >= 0x80) {                     // a status ends a SysEx left open (its bytes so far are dropped)
      sysex_ = false;
      n_ = 0;
      if (b >= 0xF0) {                   // system common: no running status
        status_ = 0;
        if (b == 0xF6) {
          uint8_t p[4] = {0x05, b, 0, 0};
          emit(p);
        } else {
          common_ = b;
          buf_[n_++] = b;
          want_ = b == 0xF2 ? 3 : (b == 0xF1 || b == 0xF3) ? 2 : 0;
          if (want_ == 0) n_ = 0;        // F4, F5: undefined, dropped
        }
        return;
      }
      common_ = 0;
      status_ = b;
      buf_[n_++] = b;
      want_ = (b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0 ? 2 : 3;
      return;
    }
    if (sysex_) {                        // CIN 4 for each 3 bytes that do not end it
      buf_[n_++] = b;
      if (n_ == 3) {
        uint8_t p[4] = {0x04, buf_[0], buf_[1], buf_[2]};
        emit(p);
        n_ = 0;
      }
      return;
    }
    if (n_ == 0) {                       // running status (channel messages only)
      if (status_ == 0) return;          // a stray data byte
      buf_[n_++] = status_;
      want_ = (status_ & 0xF0) == 0xC0 || (status_ & 0xF0) == 0xD0 ? 2 : 3;
    }
    buf_[n_++] = b;
    if (n_ == want_) {
      uint8_t cin = common_ ? (uint8_t)(want_ == 2 ? 0x02 : 0x03) : (uint8_t)(buf_[0] >> 4);
      uint8_t p[4] = {cin, buf_[0], buf_[1], want_ > 2 ? buf_[2] : (uint8_t)0};
      emit(p);
      n_ = 0;
      common_ = 0;
    }
  }

 private:
  bool sysex_ = false;
  uint8_t status_ = 0, common_ = 0;
  uint8_t buf_[3];
  int n_ = 0, want_ = 0;
};

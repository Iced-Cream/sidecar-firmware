// pi/engine/midi_filter.h - a SysEx filter on a stream of USB MIDI 1.0 event packets (4 bytes: cable/CIN, 3 MIDI
// bytes), for the Sidecar's MIDI pass-through between the Rytm and the computer (sidecar_gadget.h). Packets go
// through in order and unchanged. A SysEx is held back (at most 3 packets, 9 bytes) until its head decides; then it
// passes or is dropped whole, up to its end. Non-SysEx packets go through at once, including real-time bytes (CIN
// 0xF) inside a SysEx.
//
// What is dropped (the predicates below, by direction):
//   both ways      the engine's host-link SysEx F0 7D 52 54 ... (the Rytm and the engine talk to each other only)
//                  MIDI-CI, universal SysEx F0 7E|7F <dev> 0D ... (macOS 26 sends discovery on every port open;
//                  an echoed CI message wedged CoreMIDI's port, measured in P1)
//   to the Rytm    Elektron OS update F0 00 20 3C <dev> 00 7E|7F ... (an OS .syx file's messages: 7E data,
//                  7F start/end; rytm-emu fw/*.syx, measured), unless SetPassOsUpdate(true) (cmdline osupdate=1)
//                  the empty SysEx F0 F7 (macOS sends one on a port open, measured; it means nothing)
// Portable: Mac-tested (tests/test_midi_filter.cpp).
#pragma once
#include <stdint.h>
#include <string.h>

class MidiFilter {
 public:
  static const int kHead = 9;            // bytes of a SysEx's head that decide (3 packets)

  explicit MidiFilter(bool to_rytm) : to_rytm_(to_rytm) {}
  void SetPassOsUpdate(bool pass) { pass_os_update_ = pass; }
  void Reset() { held_ = 0; head_n_ = 0; state_ = kIdle; }        // a new source: nothing of the old one's SysEx stays   // to the Rytm: OS updates go through (sidecar_gadget: osupdate=)

  // One packet in. emit(const uint8_t p[4]) is called for every packet that goes through (0..4 per call).
  template <class Emit>
  void Feed(const uint8_t* p, Emit emit) {
    uint8_t cin = p[0] & 0x0F;
    bool sysex_part = cin == 0x4 || cin == 0x6 || cin == 0x7 || (cin == 0x5 && (p[1] == 0xF7 || state_ != kIdle));
    if (!sysex_part) {                   // channel, system common, real time: never part of a SysEx
      // passes at once, also inside a SysEx (USB MIDI packets are whole; a SysEx being dropped stays dropped)
      emit(p);
      return;
    }
    bool starts = p[1] == 0xF0;
    bool ends = cin != 0x4;
    if (starts && state_ != kIdle) Abort(emit);   // a new SysEx without the old one's end
    if (state_ == kIdle) {
      if (!starts) {                     // a continuation without a start: pass it, as received
        emit(p);
        return;
      }
      state_ = kHolding;
      held_ = 0;
      head_n_ = 0;
    }
    if (state_ == kPassing) {
      emit(p);
      if (ends) state_ = kIdle;
      return;
    }
    if (state_ == kDropping) {
      dropped_++;
      if (ends) state_ = kIdle;
      return;
    }
    // holding: keep the packet and its bytes, decide when the head is long enough or the SysEx ends
    memcpy(hold_[held_++], p, 4);
    int n = ends ? (cin == 0x5 ? 1 : cin == 0x6 ? 2 : 3) : 3;
    for (int i = 0; i < n && head_n_ < kHead; i++) head_[head_n_++] = p[1 + i];
    if (!ends && head_n_ < kHead) return;
    if (Drop(head_, head_n_)) {
      dropped_ += held_;
      dropped_msgs_++;
      state_ = ends ? kIdle : kDropping;
    } else {
      for (int i = 0; i < held_; i++) emit(hold_[i]);
      state_ = ends ? kIdle : kPassing;
    }
    held_ = 0;
  }

  uint32_t DroppedPackets() const { return dropped_; }
  uint32_t DroppedMessages() const { return dropped_msgs_; }
  bool InSysEx() const { return state_ != kIdle; }

  // The predicates (public: the tests use them).
  static bool HostLink(const uint8_t* h, int n) { return n >= 4 && h[1] == 0x7D && h[2] == 0x52 && h[3] == 0x54; }
  static bool MidiCi(const uint8_t* h, int n) { return n >= 4 && (h[1] == 0x7E || h[1] == 0x7F) && h[3] == 0x0D; }
  static bool Empty(const uint8_t* h, int n) { return n == 2 && h[1] == 0xF7; }
  static bool OsUpdate(const uint8_t* h, int n) {
    return n >= 7 && h[1] == 0x00 && h[2] == 0x20 && h[3] == 0x3C && h[5] == 0x00 && (h[6] == 0x7E || h[6] == 0x7F);
  }

 private:
  enum State { kIdle, kHolding, kPassing, kDropping };

  bool Drop(const uint8_t* h, int n) const {
    return HostLink(h, n) || MidiCi(h, n) || (to_rytm_ && ((!pass_os_update_ && OsUpdate(h, n)) || Empty(h, n)));
  }
  // A held SysEx cut short by a status byte or a new F0: its packets go through (undecided: a short one).
  template <class Emit>
  void Abort(Emit emit) {
    if (state_ == kHolding) {
      if (Drop(head_, head_n_)) {
        dropped_ += held_;
        dropped_msgs_++;
      } else {
        for (int i = 0; i < held_; i++) emit(hold_[i]);
      }
    }
    held_ = 0;
    state_ = kIdle;
  }

  bool to_rytm_;
  bool pass_os_update_ = false;
  State state_ = kIdle;
  uint8_t hold_[3][4];
  int held_ = 0;
  uint8_t head_[kHead];
  int head_n_ = 0;
  uint32_t dropped_ = 0, dropped_msgs_ = 0;
};

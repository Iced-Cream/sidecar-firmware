// pi/engine/usbmidi_os_path.h - the computer's MIDI on its way to the Rytm (sidecar_gadget.cpp) and the usbmidi=os:
// decisions around it, kept here so the Mac tests run the same code (tests/test_usbmidi_os_path.cpp).
//
// ToRytmPath: the Rytm-bound filter (midi_filter.h), the queue the gadget IRQ fills and core 0's loop empties (the USB
// merge, or the TCP link's SendComputerMIDI), and whether a SysEx is half sent. Cut () drops all of it at once - what
// waits, the rest of the SysEx being filtered and the half-sent one's state - so nothing of a message meant for one
// destination reaches the next: the emulator's OS image never the real Rytm.
//
// OsGate: the setting (usbmidi_os.h Mode), the current host, and an aborted upgrade.
//   pass ()         the filter lets OS upgrades (device byte 0C) through: on, or emu while the TCP link is the host
//   SetLink / SetMode  TRUE: cut the path now. With the setting on or emu, any host switch (the queue was meant for the
//                   old one); with any setting, the pass going from on to off.
//   Hold            the OUT endpoint's flow control: held while on and the queue has no room for a whole transfer
#pragma once
#include <stdint.h>
#include <string.h>

#include "midi_filter.h"
#include "spsc.h"
#include "usbmidi_os.h"

template <uint32_t N>
class ToRytmPath {
 public:
  ToRytmPath() : filter_(true) {}

  // the gadget IRQ: one packet of the computer's (after the keys tap); FALSE when the queue was full (dropped)
  bool FromHost(const uint8_t* p) {
    bool ok = true;
    filter_.Feed(p, [this, &ok](const uint8_t* q) {
      uint32_t w;
      memcpy(&w, q, 4);
      if (!q_.push(w)) ok = false;
    });
    return ok;
  }
  // core 0's loop: the next packet for the Rytm (or the emulator)
  bool Pop(uint32_t* w) {
    if (!q_.pop(w)) return false;
    uint8_t cin = *w & 0x0F, b1 = (*w >> 8) & 0xFF;
    if (cin == 0x4 && b1 == 0xF0) in_sysex_ = true;
    else if (cin == 0x5 || cin == 0x6 || cin == 0x7) { if (in_sysex_ || b1 == 0xF7) in_sysex_ = false; }
    return true;
  }
  bool InSysEx() const { return in_sysex_; }
  uint32_t Size() const { return q_.size(); }
  // room for one more OUT transfer of `packets` packets and what the filter may still hold back and then emit
  bool Room(unsigned packets) const { return q_.size() + packets + MidiFilter::kHoldPackets <= N; }
  // everything on the way dropped (both sides masked: the gadget IRQ and the loop); the packets dropped
  unsigned Cut() {
    uint32_t w;
    unsigned n = 0;
    while (q_.pop(&w)) n++;
    filter_.Cut();
    in_sysex_ = false;
    cut_ += n;
    return n;
  }
  unsigned CutPackets() const { return cut_; }
  MidiFilter& Filter() { return filter_; }
  const MidiFilter& Filter() const { return filter_; }

 private:
  MidiFilter filter_;
  Spsc<uint32_t, N> q_;
  bool in_sysex_ = false;
  unsigned cut_ = 0;
};

class OsGate {
 public:
  bool On() const { return mode_ == usbmidi_os::kOn || mode_ == usbmidi_os::kEmu; }
  bool Pass() const { return mode_ == usbmidi_os::kOn || (mode_ == usbmidi_os::kEmu && tcp_); }
  bool Tcp() const { return tcp_; }
  usbmidi_os::Mode Mode() const { return mode_; }

  bool SetLink(bool tcp) {
    if (tcp == tcp_) return false;
    bool was = Pass();
    tcp_ = tcp;
    return On() || (was && !Pass());
  }
  bool SetMode(usbmidi_os::Mode m) {
    bool was = Pass();
    mode_ = m == usbmidi_os::kInvalid ? usbmidi_os::kOff : m;
    return was && !Pass();
  }

  // the OUT endpoint, at the end of a transfer: wait instead of re-arming (room: ToRytmPath::Room)
  bool Hold(bool room) const { return On() && !room; }
  // core 0's loop: re-arm a held endpoint
  bool Resume(bool held, bool room) const { return held && On() && room; }

 private:
  usbmidi_os::Mode mode_ = usbmidi_os::kOff;
  bool tcp_ = false;
};

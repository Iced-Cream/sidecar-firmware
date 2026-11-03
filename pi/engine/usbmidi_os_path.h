// pi/engine/usbmidi_os_path.h - the computer's MIDI on its way to the Rytm (sidecar_gadget.cpp) and the usbmidi=os:
// decisions around it, kept here so the Mac tests run the same code (tests/test_usbmidi_os_path.cpp).
//
// ToRytmPath: works on the Rytm-bound filter (midi_filter.h), the queue the gadget IRQ fills and core 0's loop empties
// (the USB merge, or the TCP link's SendComputerMIDI), and whether a SysEx is half sent - the gadget's own members
// (OwnedToRytmPath holds them itself, for the tests). Cut () drops all of it at once - what
// waits, the rest of the SysEx being filtered and the half-sent one's state - so nothing of a message meant for one
// destination reaches the next: the emulator's OS image never the real Rytm.
//
// OsGate: the setting (usbmidi_os.h Mode), the current host, and an aborted upgrade.
//   pass ()         the filter lets OS upgrades (device byte 0C) through: on, or emu while the TCP link is the host;
//                   never while an aborted upgrade is still being sent
//   SetLink / SetMode  TRUE: cut the path now. With the setting on or emu, any host switch (the queue was meant for the
//                   old one); with any setting, the pass going from on to off.
//   Hold / Resume   the OUT endpoint's flow control: held while on and the queue has no room for a whole transfer;
//                   resumed when there is room, or at once when the setting is off (dev never holds): turning it off
//                   while held never leaves the computer's MIDI OUT NAKed
//   EngineWaits     the TCP link holds the engine's records back behind a half-sent computer SysEx only while the
//                   setting is on: off is dev's behaviour (the TCP link never looked at the computer's MIDI)
//   Lost / Quiet    a send of the computer's packets failed, or the Rytm (or the emulator) went away with OS data still
//                   waiting, during an upgrade (an OS message seen within kQuietUs): the upgrade is aborted - the path
//                   cut, OS messages dropped - rather than the Rytm getting an image with a hole in it. The pass comes
//                   back once the computer has sent no OS message for kQuietUs (it gave up or finished), or at a new
//                   setting; after the quiet the filter still drops OS messages until a new upgrade's start
//                   (MidiFilter::AwaitStart, os_upgrade::start), so a sender that paused mid-image never gets its tail
//                   through: an image's end (7F 02) stays dropped too, only its start (7F 01) ends the wait.
//   Switched        a host switch during an upgrade (setting on or emu) aborts it the same way: the rest of an image
//                   meant for one host never reaches the other, also with on, where the pass would stay on.
//                   Seen () must run every loop (Quiet does), so "within kQuietUs" is measured from when
//                   the count last moved.
#pragma once
#include <stdint.h>
#include <string.h>

#include "midi_filter.h"
#include "spsc.h"
#include "usbmidi_os.h"

template <uint32_t N>
class ToRytmPath {
 public:
  ToRytmPath(MidiFilter& filter, Spsc<uint32_t, N>& q, bool& in_sysex) : filter_(filter), q_(q), in_sysex_(in_sysex) {}

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
  // OS data on the way: queued, half sent, or a SysEx in the filter
  bool Busy() const { return q_.size() != 0 || in_sysex_ || filter_.InSysEx(); }
  unsigned CutPackets() const { return cut_; }
  MidiFilter& Filter() { return filter_; }
  const MidiFilter& Filter() const { return filter_; }

 private:
  MidiFilter& filter_;
  Spsc<uint32_t, N>& q_;
  bool& in_sysex_;
  unsigned cut_ = 0;
};

template <uint32_t N>
struct ToRytmParts {
  MidiFilter filter{true};
  Spsc<uint32_t, N> q;
  bool in_sysex = false;
};
template <uint32_t N>
class OwnedToRytmPath : private ToRytmParts<N>, public ToRytmPath<N> {
 public:
  OwnedToRytmPath() : ToRytmPath<N>(this->filter, this->q, this->in_sysex) {}
};

class OsGate {
 public:
  static constexpr uint32_t kQuietUs = 3000000;

  bool On() const { return mode_ == usbmidi_os::kOn || mode_ == usbmidi_os::kEmu; }
  bool Pass() const { return !aborted_ && (mode_ == usbmidi_os::kOn || (mode_ == usbmidi_os::kEmu && tcp_)); }
  bool Tcp() const { return tcp_; }
  usbmidi_os::Mode Mode() const { return mode_; }
  bool Aborted() const { return aborted_; }
  unsigned Aborts() const { return aborts_; }

  bool SetLink(bool tcp) {
    if (tcp == tcp_) return false;
    bool was = Pass();
    tcp_ = tcp;
    return On() || (was && !Pass());
  }
  bool SetMode(usbmidi_os::Mode m) {
    bool was = Pass();
    mode_ = m == usbmidi_os::kInvalid ? usbmidi_os::kOff : m;
    aborted_ = false;			// a new setting starts over
    return was && !Pass();
  }

  // the OUT endpoint, at the end of a transfer: wait instead of re-arming (room: ToRytmPath::Room)
  bool Hold(bool room) const { return On() && !room; }
  // core 0's loop: re-arm a held endpoint
  bool Resume(bool held, bool room) const { return held && (!On() || room); }
  // the TCP link: the engine's records wait behind the computer's half-sent SysEx
  bool EngineWaits(bool computer_in_sysex) const { return On() && computer_in_sysex; }

  // the filter's count of OS-upgrade messages (MidiFilter::OsMessages), at core 0's loop
  // The clock is Circle's 32-bit microseconds (wraps every ~71.6 min): the window is a signed difference, and an old
  // sighting is forgotten once the window has passed, so a wrap never brings it back
  void Seen(uint32_t os_msgs, uint32_t now_us) {
    if (os_msgs != os_msgs_) {
      os_msgs_ = os_msgs;
      os_at_ = now_us;
      os_seen_ = true;
    } else if (os_seen_ && !Within(now_us)) {
      os_seen_ = false;
    }
  }
  bool Upgrading(uint32_t now_us) const { return os_seen_ && Within(now_us); }
  // data was lost on the way (send_failed) or the destination left with data still on the way (busy): TRUE when that
  // aborts an upgrade (cut the path then, and say so loudly)
  bool Lost(bool send_failed, bool busy, uint32_t os_msgs, uint32_t now_us) {
    Seen(os_msgs, now_us);
    if (aborted_ || !Pass() || !Upgrading(now_us) || !(send_failed || busy)) return false;
    aborted_ = true;
    aborts_++;
    return true;
  }
  // a host switch that cut the path (SetLink's TRUE): TRUE when an upgrade was under way, now aborted
  bool Switched(bool cut, uint32_t os_msgs, uint32_t now_us) {
    Seen(os_msgs, now_us);
    if (!cut || aborted_ || !On() || !Upgrading(now_us)) return false;
    aborted_ = true;
    aborts_++;
    return true;
  }
  // core 0's loop: TRUE once when an aborted upgrade's sender has gone quiet (the pass is back)
  bool Quiet(uint32_t os_msgs, uint32_t now_us) {
    Seen(os_msgs, now_us);
    if (!aborted_ || Upgrading(now_us)) return false;
    aborted_ = false;
    return true;
  }

 private:
  usbmidi_os::Mode mode_ = usbmidi_os::kOff;
  bool Within(uint32_t now_us) const {
    int32_t d = (int32_t)(now_us - os_at_);
    return d >= 0 && d < (int32_t)kQuietUs;
  }
  bool tcp_ = false, aborted_ = false, os_seen_ = false;
  uint32_t os_msgs_ = 0, os_at_ = 0;
  unsigned aborts_ = 0;
};

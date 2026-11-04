// pi/engine/midiout_q.h - what waits for one USB-A MIDI device (kernel.cpp's AsampMidiSend, core 0). The sends are asynchronous, one transfer in flight per
// device, so a device that does not drain costs nothing but its queue. Tiers: when the queue is short of room, what matters least goes first:
// clock (kRealtimeMax), values and everything else (kValueMax), note-ons (kNoteMax); a note-off may use all of it and pushes out the oldest packet
// that is not one, so a stuck device still gets its note-offs if it ever comes back. A failed transfer puts its note-offs back in front and
// holds the device kHoldUs. Portable: tests/test_midiout_q.cpp. One thread.
#pragma once
#include <stdint.h>
#include <string.h>

class MidiOutQ {
 public:
  static constexpr unsigned kCap = 64, kBatch = 16;
  static constexpr unsigned kRealtimeMax = 8, kValueMax = 32, kNoteMax = 48;
  static constexpr uint32_t kHoldUs = 100000;

  // A USB MIDI event packet as a little-endian word: CIN in the low nibble, status in byte 1, data in bytes 2 and 3
  static bool is_off(uint32_t w) {
    unsigned cin = w & 0x0F;
    return cin == 0x8 || (cin == 0x9 && (w >> 24) == 0);
  }
  static bool is_on(uint32_t w) { return (w & 0x0F) == 0x9 && (w >> 24) != 0; }
  static bool is_clock(uint32_t w) { return (w & 0x0F) == 0x0F && ((w >> 8) & 0xFF) == 0xF8; }

  void clear() { h_ = n_ = sent_ = 0; armed_ = false; }
  unsigned size() const { return n_; }
  unsigned dropped() const { return dropped_; }
  unsigned offs_dropped() const { return offs_dropped_; }

  // now_us: a 32-bit clock, it wraps every 71.6 minutes. Armed by hold() only: a time compared when no hold was ever set, or one left from
  // long ago, reads as the future for half of each wrap (the device silent for 36 minutes at a time).
  void hold(uint32_t now_us) { hold_end_ = now_us + kHoldUs, armed_ = true; }
  bool held(uint32_t now_us) {
    if (armed_ && (int32_t)(hold_end_ - now_us) > 0) return true;
    armed_ = false;
    return false;
  }

  void push(uint32_t w) {
    if (is_off(w)) {
      if (n_ == kCap && !evict()) {
        ++dropped_, ++offs_dropped_;
        return;
      }
    } else if (n_ >= (is_clock(w) ? kRealtimeMax : is_on(w) || (w & 0x0F) == 0x0F ? kNoteMax : kValueMax)) {
      ++dropped_;
      return;
    }
    q_[(h_ + n_++) % kCap] = w;
  }
  // The next transfer: up to kBatch packets into out, kept here until done(); their count, 0 none
  unsigned pop(uint32_t* out) {
    sent_ = n_ < kBatch ? n_ : kBatch;
    for (unsigned i = 0; i < sent_; ++i) {
      out[i] = q_[h_];
      last_[i] = q_[h_];
      h_ = (h_ + 1) % kCap;
    }
    n_ -= sent_;
    return sent_;
  }
  // The last pop's transfer failed: its note-offs in front again, in order (what is there anyway keeps its place)
  void failed() {
    for (unsigned i = sent_; i-- > 0;) {
      if (!is_off(last_[i])) continue;
      if (n_ == kCap && !evict()) {
        ++dropped_, ++offs_dropped_;
        continue;
      }
      h_ = (h_ + kCap - 1) % kCap;
      q_[h_] = last_[i];
      ++n_;
    }
    sent_ = 0;
  }

 private:
  bool evict() {   // the oldest packet that is not a note-off goes
    for (unsigned i = 0; i < n_; ++i) {
      if (is_off(q_[(h_ + i) % kCap])) continue;
      for (unsigned k = i; k + 1 < n_; ++k) q_[(h_ + k) % kCap] = q_[(h_ + k + 1) % kCap];
      --n_;
      ++dropped_;
      return true;
    }
    return false;
  }
  uint32_t q_[kCap], last_[kBatch];
  unsigned h_ = 0, n_ = 0, sent_ = 0, dropped_ = 0, offs_dropped_ = 0;
  uint32_t hold_end_ = 0;
  bool armed_ = false;
};

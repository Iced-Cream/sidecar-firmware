// pi/engine/midi_in.h - MIDI bytes (USB MIDI packets' bytes in order, or a TCP stream) -> whole SysEx messages
// (F0..F7, up to 512 bytes) and channel messages. Running status kept; realtime bytes (F8..FF) pass through a
// SysEx and are counted (clock); a status byte inside a SysEx drops it (counted). The optional realtime callback gets
// F8 / FA / FB / FC (spp 0) and F2 Song Position (spp = its 14-bit value, in 16ths): STUTTER's clock.
#pragma once
#include <stdint.h>

class MidiIn {
 public:
  typedef void (*SysExFn)(const uint8_t* d, int len);
  typedef void (*ChannelFn)(uint8_t status, uint8_t d1, uint8_t d2);
  typedef void (*RealtimeFn)(uint8_t status, uint16_t spp);
  MidiIn(SysExFn sx, ChannelFn ch, RealtimeFn rt = nullptr) : sx_fn_(sx), ch_fn_(ch), rt_fn_(rt) {}
  void Feed(uint8_t b);
  uint32_t Clocks() const { return clocks_; }
  uint32_t SysExDropped() const { return dropped_; }
  uint32_t SysExCount() const { return count_; }

 private:
  static const int kMax = 512;
  SysExFn sx_fn_;
  ChannelFn ch_fn_;
  RealtimeFn rt_fn_;
  uint8_t sx_[kMax];
  int sxn_ = -1;          // -1: outside a SysEx
  bool over_ = false;
  uint8_t status_ = 0, d_[2] = {0, 0};
  int need_ = 0, have_ = 0;
  uint32_t clocks_ = 0, dropped_ = 0, count_ = 0;
};

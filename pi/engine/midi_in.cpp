// pi/engine/midi_in.cpp - see midi_in.h.
#include "midi_in.h"

void MidiIn::Feed(uint8_t b) {
  if (b >= 0xF8) {   // realtime: anywhere, even inside a SysEx
    if (b == 0xF8) ++clocks_;
    if (rt_fn_ && (b == 0xF8 || b == 0xFA || b == 0xFB || b == 0xFC)) rt_fn_(b, 0);
    return;
  }
  if (b == 0xF0) {
    if (sxn_ >= 0) ++dropped_;
    sxn_ = 0;
    over_ = false;
    sx_[sxn_++] = b;
    status_ = 0;
    return;
  }
  if (b == 0xF7) {
    if (sxn_ >= 0) {
      if (over_) {
        ++dropped_;
      } else {
        sx_[sxn_++] = b;
        ++count_;
        sx_fn_(sx_, sxn_);
      }
    }
    sxn_ = -1;
    return;
  }
  if (b & 0x80) {   // another status
    if (sxn_ >= 0) {
      ++dropped_;
      sxn_ = -1;
    }
    if (b == 0xF2) {   // Song Position: two data bytes, no running status
      status_ = b, have_ = 0, need_ = 2;
      return;
    }
    if (b >= 0xF0) {   // other system common: no running status, its data ignored
      status_ = 0;
      return;
    }
    status_ = b;
    have_ = 0;
    need_ = ((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0) ? 1 : 2;
    return;
  }
  if (sxn_ >= 0) {   // SysEx data
    if (sxn_ < kMax - 1) sx_[sxn_++] = b;
    else over_ = true;
    return;
  }
  if (!status_) return;
  d_[have_++] = b;
  if (have_ == need_) {
    if (status_ == 0xF2) {
      if (rt_fn_) rt_fn_(0xF2, (uint16_t)(d_[0] | d_[1] << 7));
      status_ = 0;
    } else {
      ch_fn_(status_, d_[0], need_ > 1 ? d_[1] : 0);
    }
    have_ = 0;
  }
}

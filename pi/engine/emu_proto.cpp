// pi/engine/emu_proto.cpp - see emu_proto.h.
#include "emu_proto.h"

bool EmuParser::Feed(const uint8_t* d, int n) {
  if (bad_) return false;
  for (int i = 0; i < n;) {
    if (kind_ == 0) {
      kind_ = d[i++];
      have_ = 0;
      body_ = 0;
      switch (kind_) {
        case 'N': need_ = 4; break;
        case 'T': need_ = 3; break;
        case 'P': need_ = 9; break;
        case 'R': need_ = 4; break;
        case 'H': need_ = 2; break;
        case 'M': need_ = 2; break;
        default: kind_ = 0; bad_ = true; return false;
      }
      if (kind_ == 'T' || kind_ == 'P' || kind_ == 'R') ++skipped_;
      continue;
    }
    if (have_ < need_) {
      head_[have_ < 4 ? have_ : 3] = d[i++];
      ++have_;
      if (kind_ == 'T' || kind_ == 'P' || kind_ == 'R') ++skipped_;
      if (have_ < need_) continue;
      if (kind_ == 'N') {
        sink_->Want((uint32_t)head_[0] | (uint32_t)head_[1] << 8 | (uint32_t)head_[2] << 16 | (uint32_t)head_[3] << 24);
        kind_ = 0;
      } else if (kind_ == 'H') {
        sink_->Hello(head_[1]);
        kind_ = 0;
      } else if (kind_ == 'M') {
        body_ = (uint64_t)head_[0] | (uint64_t)head_[1] << 8;
        if (!body_) kind_ = 0;
      } else if (kind_ == 'R') {
        body_ = 48ull * ((uint64_t)head_[0] | (uint64_t)head_[1] << 8 | (uint64_t)head_[2] << 16 | (uint64_t)head_[3] << 24);
        if (!body_) kind_ = 0;
      } else {
        kind_ = 0;   // T, P: done
      }
      continue;
    }
    uint32_t k = (uint64_t)(n - i) < body_ ? (uint32_t)(n - i) : (uint32_t)body_;   // a payload
    if (kind_ == 'M') sink_->Midi(d + i, (int)k);
    else skipped_ += k;
    i += (int)k;
    body_ -= k;
    if (!body_) kind_ = 0;
  }
  return true;
}

int emu_hello(uint8_t* out, uint8_t flags) {
  out[0] = 'H';
  out[1] = 1;
  out[2] = flags;
  return 3;
}

int emu_midi_head(uint8_t* out, uint16_t len) {
  out[0] = 'M';
  out[1] = len & 0xFF;
  out[2] = len >> 8;
  return 3;
}

int emu_frames_head(uint8_t* out, uint8_t kind, uint32_t n) {
  out[0] = kind;
  for (int i = 0; i < 4; ++i) out[1 + i] = (uint8_t)(n >> (8 * i));
  return 5;
}

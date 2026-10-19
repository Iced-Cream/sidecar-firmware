// pi/engine/records.h - the consumer's side of g_MidiOut's records (kernel.cpp ToHost: u16 LE length, then the
// bytes). Core 3 pushes a record byte by byte, so a consumer can find half of one; this reader keeps the partial
// record across calls and across consumers (the USB transport, the self-test, the TCP link, each one's drop path),
// so a switch never cuts a record and the stream never desyncs. One instance per ring (shared.cpp's g_ToHost), its
// state never reset; one consumer context at a time (core 0's loop). Portable (Mac-tested: tests/test_records.cpp).
#pragma once
#include <stdint.h>

template <class Ring, unsigned kMax = 4096>
class RecordReader {
 public:
  explicit RecordReader(Ring* r) : ring_(r) {}
  // The next whole record (its bytes, *len its length), or nullptr when none is whole yet. The pointer stays valid
  // until the next call. A record longer than kMax (ToHost never writes one) is skipped, counted in Oversize().
  const uint8_t* Next(int* len) {
    uint8_t b;
    while (ring_->pop(&b)) {
      if (lenBytes_ < 2) {
        if (lenBytes_++ == 0) { want_ = b; continue; }
        want_ |= unsigned(b) << 8;
        have_ = 0;
        if (want_ == 0) { lenBytes_ = 0; continue; }   // an empty record (ToHost never writes one)
        continue;
      }
      if (have_ < kMax) buf_[have_] = b;
      if (++have_ < want_) continue;
      lenBytes_ = 0;                                   // whole: the next byte starts a new record
      if (want_ > kMax) { oversize_ += want_; continue; }
      *len = int(want_);
      return buf_;
    }
    return nullptr;
  }
  // Drop every whole record now in the ring (a consumer that sends nothing); returns the bytes dropped.
  unsigned Drain() {
    unsigned n = 0;
    int len;
    while (Next(&len)) n += unsigned(len);
    return n;
  }
  uint64_t Oversize() const { return oversize_; }

 private:
  Ring* ring_;
  unsigned lenBytes_ = 0, want_ = 0, have_ = 0;
  uint64_t oversize_ = 0;
  uint8_t buf_[kMax];
};

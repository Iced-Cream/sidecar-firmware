// pi/engine/vl805_probe.h - a probe for the Pi 4's VL805 periodic stall (obprobe=1). Every periodic endpoint of both boxes
// stops together for 2.048 s (one MFINDEX wrap, 2^14 microframes), URBs queued, no error; restarting an endpoint does not
// release it (QK1, KB T491). This records what is needed to tell why:
//   - the microframe index (MFINDEX) of every box stream event (the xHCI IRQ reads it: one PCIe read an event), so a stall's
//     last event before and first event after can be compared: a controller waiting for its next service time to come
//     round again comes back at (almost) the same MFINDEX it stopped at, 2^14 microframes later;
//   - the main loop's MFINDEX against the Pi's clock: a controller whose counter itself stops or jumps shows as a deviation;
//   - per 10 ms (the last kBuckets before a stall): events, the longest gap between two events at the IRQ (the Pi late to
//     take them, or the controller quiet), the fewest URBs queued at an event (1 = that ring ran dry after it), the MFINDEX
//     deviation;
//   - per second: the same, one line.
// The glue (ob_usb.cpp) logs the controller's registers and the endpoints' output contexts at the stall, every kStillUs while
// it lasts, and the first events after it. Reads only: the probe changes nothing on the controller. Portable:
// tests/test_vl805_probe.cpp.
#pragma once
#include <stdint.h>

namespace vl805 {

constexpr uint32_t kMfMask = 0x3FFF;		// MFINDEX: 14 bits of 125 us microframes, 2.048 s a wrap
constexpr uint32_t kMfUs = 125, kWrapUs = (kMfMask + 1) * kMfUs;
constexpr uint32_t kBucketUs = 10000;		// one history bucket: 10 ms
constexpr unsigned kBuckets = 32;		// the last 320 ms before a stall
constexpr uint32_t kStallUs = 20000;		// no box event for 20 ms with URBs queued: a stall (the 8 URBs last 1 ms)
constexpr uint32_t kStillUs = 500000;		// a line every 500 ms while it lasts
constexpr unsigned kBoxes = 2, kDirs = 2;	// box slot 0/1, IN 0 / OUT 1
constexpr unsigned kOthers = 32;		// the last transfer events of anything else (EP0, MIDI, hub, relay ...)
constexpr uint32_t kOtherUs = 50000;		// logged at a stall: those from 50 ms before its last box event on

// microframes from a to b, forward (mod 2^14)
inline uint32_t mf_ahead(uint32_t a, uint32_t b) { return (b - a) & kMfMask; }

struct Bucket {
  uint32_t start_us = 0;
  uint16_t events = 0;
  uint16_t gap_max_us = 0;	// capped at 65535
  uint8_t pend_min = 255;	// fewest URBs queued at an event (this one included), any box stream
  uint16_t mf_dev_us = 0;	// largest |MFINDEX step - clock step| of the main loop's samples, capped
};

// a transfer event that is not a box stream's completed transfer: another endpoint, or a box stream's other code
struct Other {
  uint32_t us = 0, mf = 0, len = 0;
  uint8_t slot = 0, ep = 0, code = 0;
};

struct Probe {
  // written by the IRQ (OnEvent), read by the main loop
  volatile uint32_t last_us = 0, last_mf = 0, events = 0;
  volatile uint32_t ep_us[kBoxes][kDirs] = {}, ep_mf[kBoxes][kDirs] = {}, ep_n[kBoxes][kDirs] = {};
  volatile uint32_t gap_max_us = 0, gap_at_us = 0;	// this second's
  volatile uint8_t pend_min[kBoxes][kDirs] = {{255, 255}, {255, 255}};	// this second's
  Bucket hist[kBuckets];
  volatile unsigned cur = 0;			// the bucket being filled

  Other other[kOthers];
  volatile uint32_t others = 0;			// written so far (the next one at others % kOthers)

  // the main loop's
  uint32_t samp_us = 0, samp_mf = 0;
  bool have_samp = false;
  uint32_t dev_max_us = 0, dev_at_us = 0;	// this second's

  // a stall: 0 none; detected at stall_us (the last event before it at stall_last_us / stall_last_mf)
  bool stalled = false;
  uint32_t stall_us = 0, stall_last_us = 0, stall_last_mf = 0, still_us = 0, stalls = 0;
  uint32_t stall_ep_mf[kBoxes][kDirs] = {}, stall_ep_n[kBoxes][kDirs] = {};

  // obstallrs=1: one Run/Stop cycle a stall, at most kRsMax a boot, and none for a stall whose last event came less than
  // kRsQuietUs after a cycle (a cycle can restart MFINDEX at 0: a stall it caused could be cycled into another, and so on)
  static constexpr uint32_t kRsMax = 8, kRsQuietUs = 3000000;
  uint32_t rs_stall = 0, rs_us = 0, rs_n = 0;
  bool WantRunStop(uint32_t now_us) const {
    (void)now_us;
    return stalled && rs_stall != stalls && rs_n < kRsMax && (rs_n == 0 || stall_last_us - rs_us >= kRsQuietUs);
  }
  void DidRunStop(uint32_t now_us) { rs_stall = stalls, rs_us = now_us, ++rs_n; }

  static uint32_t clamp16(uint32_t v) { return v > 65535 ? 65535 : v; }

  Bucket& Roll(uint32_t now_us) {	// the bucket for now (IRQ or main loop: a torn roll costs one bucket's numbers)
    Bucket& b = hist[cur % kBuckets];
    if (now_us - b.start_us < kBucketUs) return b;
    unsigned n = (cur + 1) % kBuckets;
    hist[n] = Bucket();
    hist[n].start_us = now_us;
    cur = n;
    return hist[n];
  }

  // a box stream's transfer event (xHCI IRQ): box 0/1, dir 0 IN / 1 OUT, pend = URBs queued at it (this one included)
  void OnEvent(unsigned box, unsigned dir, uint32_t now_us, uint32_t mf, unsigned pend) {
    if (box >= kBoxes || dir >= kDirs) return;
    const uint32_t gap = events != 0 ? now_us - last_us : 0;
    if (gap > gap_max_us) gap_max_us = gap, gap_at_us = now_us;
    Bucket& b = Roll(now_us);
    if (b.events < 65535) ++b.events;
    if (gap > b.gap_max_us) b.gap_max_us = (uint16_t)clamp16(gap);
    const uint8_t p = (uint8_t)(pend > 255 ? 255 : pend);
    if (p < b.pend_min) b.pend_min = p;
    if (p < pend_min[box][dir]) pend_min[box][dir] = p;
    ep_us[box][dir] = now_us, ep_mf[box][dir] = mf & kMfMask, ++ep_n[box][dir];
    last_us = now_us, last_mf = mf & kMfMask, ++events;
  }

  // any other transfer event (xHCI IRQ)
  void OnOther(uint32_t now_us, uint32_t mf, unsigned slot, unsigned ep, unsigned code, uint32_t len) {
    Other& o = other[others % kOthers];
    o.us = now_us, o.mf = mf & kMfMask, o.len = len;
    o.slot = (uint8_t)slot, o.ep = (uint8_t)ep, o.code = (uint8_t)code;
    ++others;
  }
  // the other events from from_us on, oldest first (at most kOthers; a race with the IRQ can tear the newest one)
  unsigned Others(uint32_t from_us, Other* out) const {
    const uint32_t n = others, first = n > kOthers ? n - kOthers : 0;
    unsigned k = 0;
    for (uint32_t i = first; i < n; i++) {
      const Other& o = other[i % kOthers];
      if ((int32_t)(o.us - from_us) >= 0) out[k++] = o;
    }
    return k;
  }

  // the main loop's MFINDEX sample: its step against the clock's (only over less than half a wrap: ambiguous beyond)
  void Sample(uint32_t now_us, uint32_t mf) {
    mf &= kMfMask;
    if (have_samp) {
      const uint32_t dt = now_us - samp_us;
      if (dt < kWrapUs / 2) {
        const int32_t dev = (int32_t)(mf_ahead(samp_mf, mf) * kMfUs) - (int32_t)dt;
        const uint32_t a = (uint32_t)(dev < 0 ? -dev : dev);
        if (a > dev_max_us) dev_max_us = a, dev_at_us = now_us;
        Bucket& b = Roll(now_us);
        if (a > b.mf_dev_us) b.mf_dev_us = (uint16_t)clamp16(a);
      }
    }
    samp_us = now_us, samp_mf = mf, have_samp = true;
  }

  enum Say { None, Began, Still, Back };
  // the main loop, after Sample: pending = a streaming box has URBs queued. Began: log the stall (history, registers,
  // contexts); Still: a line while it lasts; Back: the first events after it (BackUs, BackMf)
  Say Check(uint32_t now_us, bool pending) {
    const uint32_t last = last_us;
    if (!stalled) {
      if (!pending || events == 0 || (int32_t)(now_us - last) <= (int32_t)kStallUs) return None;
      stalled = true, ++stalls;
      stall_us = now_us, still_us = now_us, stall_last_us = last, stall_last_mf = last_mf;
      for (unsigned k = 0; k < kBoxes; k++)
        for (unsigned d = 0; d < kDirs; d++) stall_ep_mf[k][d] = ep_mf[k][d], stall_ep_n[k][d] = ep_n[k][d];
      return Began;
    }
    if (last != stall_last_us) { stalled = false; return Back; }
    if (now_us - still_us >= kStillUs) { still_us = now_us; return Still; }
    return None;
  }
  // after Back: the quiet time and the microframes from the last event before to the first after (mod 2^14)
  uint32_t BackUs() const { return last_us - stall_last_us; }
  uint32_t BackMf() const { return mf_ahead(stall_last_mf, last_mf); }
  // the microframes the quiet time stands for, mod 2^14: BackMf near this = MFINDEX ran on through the stall
  uint32_t BackMfByClock() const { return (BackUs() / kMfUs) & kMfMask; }

  // the per-second line's numbers, then cleared (main loop; a race with the IRQ loses one event's worth)
  void TakeSecond(uint32_t* gap, uint32_t* dev, uint8_t pmin[kBoxes][kDirs]) {
    *gap = gap_max_us, gap_max_us = 0;
    *dev = dev_max_us, dev_max_us = 0;
    for (unsigned k = 0; k < kBoxes; k++)
      for (unsigned d = 0; d < kDirs; d++) pmin[k][d] = pend_min[k][d], pend_min[k][d] = 255;
  }

  // the history, oldest first: the n buckets before (and including) the current one
  unsigned History(Bucket* out, unsigned n) const {
    if (n > kBuckets) n = kBuckets;
    const unsigned c = cur;
    unsigned k = 0;
    for (unsigned i = 0; i < n; i++) {
      const Bucket& b = hist[(c + kBuckets - (n - 1 - i)) % kBuckets];
      if (b.start_us == 0 && b.events == 0) continue;
      out[k++] = b;
    }
    return k;
  }
};

}  // namespace vl805

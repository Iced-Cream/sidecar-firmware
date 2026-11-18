// stall_trace.h - a ring of timestamped USB events, printed when a box's audio endpoint goes quiet, to find what comes
// before the VL805's start-of-stream stalls (every periodic endpoint stopped for one MFINDEX wrap, 2.048 s;
// stall-restart.md). The saved logs had no timestamps on the two leads: the computer's control requests forwarded to a
// box (USB-C gadget and Teensy relay) and the Pi's vendor requests to the Teensy (its 10 s stats read). Each entry
// also carries the controller's MFINDEX, so the onset and the resume can be placed on the 2.048 s wrap.
//
// Portable: the caller passes the time (us) and the MFINDEX. Add is safe from IRQ and task level (one atomic slot
// claim; an entry overwritten while it is read is skipped by its sequence number). Format writes the entries of a
// window as text, a few per line.
#pragma once

#include <stdint.h>
#include <stdio.h>

namespace stall_trace {

enum Kind : uint8_t {
  kGadgetSetup = 'G',   // the computer's control request on the USB-C gadget (a: bmRequestType, b: bRequest, d: wValue, e: wIndex)
  kRelaySetup = 'R',    // the same, from the relay's computer (Teensy setup event)
  kBoxCtl = 'B',        // a forwarded request submitted to a box's EP0 (c: slot, a/b/d/e as above)
  kBoxCtlDone = 'b',    // its completion (c: slot, d: result length, e: 1 ok / 0 failed)
  kVendor = 'V',        // the Pi's vendor request to the Teensy (a: 1 IN / 0 OUT, b: request, d: length)
  kVendorDone = 'v',    // its return (b: request, d: result as int16, e: time taken in 100 us)
  kXhciCmd = 'X',       // an xHCI command rung (b: TRB type, c: slot)
  kQuiet = 'Q',         // a box endpoint found quiet (a: 1 IN / 0 OUT, c: slot)
  kResume = 'r',        // a box's IN completed again after a gap over kResumeGapUs (c: slot, d: gap in ms)
};

constexpr unsigned kResumeGapUs = 100000;

struct Entry {
  uint32_t seq;  // 0: never written; otherwise the claim number + 1, written last
  uint32_t us;
  uint16_t mf;
  uint8_t kind, a, b, c;
  uint16_t d, e;
};

template <unsigned N = 256>
struct Ring {
  static_assert((N & (N - 1)) == 0, "N a power of two");
  Entry ev[N] = {};
  uint32_t head = 0;  // claims so far
  uint32_t last_dump_us = 0;
  unsigned dumps = 0;

  void Add(uint32_t us, uint16_t mf, uint8_t kind, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0, uint16_t d = 0,
           uint16_t e = 0) {
    uint32_t n = __atomic_fetch_add(&head, 1u, __ATOMIC_RELAXED);
    Entry& x = ev[n & (N - 1)];
    __atomic_store_n(&x.seq, 0u, __ATOMIC_RELAXED);
    x.us = us, x.mf = mf, x.kind = kind, x.a = a, x.b = b, x.c = c, x.d = d, x.e = e;
    __atomic_store_n(&x.seq, n + 1, __ATOMIC_RELEASE);
  }

  // one dump per stall: true when none was printed in the last hold_us (and fewer than max_dumps so far)
  bool DumpDue(uint32_t now_us, uint32_t hold_us = 2500000, unsigned max_dumps = 40) {
    if (dumps >= max_dumps || (dumps != 0 && now_us - last_dump_us < hold_us)) return false;
    last_dump_us = now_us, ++dumps;
    return true;
  }

  // Calls line(text) for the entries from ref_us - before_us on (oldest first; only the newest max_entries of them),
  // per_line entries a line, each as "<kind><ms from ref_us>@<mf hex> <fields>". Returns the entries written.
  template <class Line>
  unsigned Format(uint32_t ref_us, uint32_t before_us, unsigned per_line, Line line, unsigned max_entries = ~0u) const {
    uint32_t h = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
    uint32_t first = h > N ? h - N : 0;
    unsigned in_window = 0;
    for (uint32_t n = first; n < h; ++n)
      if ((int32_t)(ev[n & (N - 1)].us - ref_us) >= -(int32_t)before_us) ++in_window;
    unsigned skip = in_window > max_entries ? in_window - max_entries : 0;
    char buf[400];
    unsigned len = 0, on_line = 0, total = 0;
    for (uint32_t n = first; n < h; ++n) {
      const Entry& x = ev[n & (N - 1)];
      if (__atomic_load_n(&x.seq, __ATOMIC_ACQUIRE) != n + 1) continue;
      Entry y = x;
      if (__atomic_load_n(&x.seq, __ATOMIC_ACQUIRE) != n + 1) continue;  // overwritten while copied
      int32_t rel = (int32_t)(y.us - ref_us);
      if (rel < -(int32_t)before_us) continue;
      if (skip) { --skip; continue; }
      int ms10 = rel / 100;  // 0.1 ms
      char sign = rel < 0 ? '-' : '+';
      unsigned m = (unsigned)(ms10 < 0 ? -ms10 : ms10);
      int w = snprintf(buf + len, sizeof buf - len, "%s%c%c%u.%u@%04x %02x %02x %u %04x %04x", on_line ? "; " : "",
                       (char)y.kind, sign, m / 10, m % 10, (unsigned)y.mf, y.a, y.b, y.c, y.d, y.e);
      if (w < 0 || (unsigned)w >= sizeof buf - len) break;
      len += (unsigned)w, ++on_line, ++total;
      if (on_line == per_line) line(buf), len = 0, on_line = 0;
    }
    if (on_line) line(buf);
    return total;
  }
};

}  // namespace stall_trace

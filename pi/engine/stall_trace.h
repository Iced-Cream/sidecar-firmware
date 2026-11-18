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

// a small text writer (the Pi kernel has no C library, so no snprintf)
struct Text {
  char* p;
  unsigned n, cap;  // written, room (one byte kept for the 0)
  bool ok() const { return n < cap; }
  void c(char ch) { if (n + 1 < cap) p[n++] = ch, p[n] = 0; else n = cap; }
  void s(const char* t) { while (*t) c(*t++); }
  void u(unsigned v) { char d[10]; unsigned k = 0; do d[k++] = (char)('0' + v % 10), v /= 10; while (v); while (k) c(d[--k]); }
  void x(unsigned v, unsigned digits) { for (int sh = (int)digits * 4 - 4; sh >= 0; sh -= 4) c("0123456789abcdef"[(v >> sh) & 15]); }
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
    buf[0] = 0;
    unsigned on_line = 0, total = 0;
    Text t{buf, 0, sizeof buf};
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
      if (on_line) t.s("; ");
      t.c((char)y.kind), t.c(sign), t.u(m / 10), t.c('.'), t.u(m % 10), t.c('@'), t.x(y.mf, 4);
      t.c(' '), t.x(y.a, 2), t.c(' '), t.x(y.b, 2), t.c(' '), t.u(y.c), t.c(' '), t.x(y.d, 4), t.c(' '), t.x(y.e, 4);
      if (!t.ok()) break;  // (a line holds far more than per_line entries: not reached)
      ++on_line, ++total;
      if (on_line == per_line) line(buf), t.n = 0, buf[0] = 0, on_line = 0;
    }
    if (on_line) line(buf);
    return total;
  }
};

}  // namespace stall_trace

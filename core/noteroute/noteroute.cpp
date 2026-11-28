// core/noteroute/noteroute.cpp - see noteroute.h.
#include "noteroute.h"

#include <string.h>

namespace core {
namespace noteroute {

namespace {
constexpr int kKeys = kUsbADevices + 1;   // held notes kept per source: the USB-A devices, then the box's USB-C port
constexpr int kKeyUsbC = kUsbADevices;

struct R {
  bool open;
  uint8_t owner, src, chan, slot;
  uint32_t held[kKeys][16][4];   // notes delivered and not yet off: per source, channel, 128 bits
  uint16_t sus[kKeys];           // channels this route left with CC 64 down
};
struct Answer { uint8_t n; uint8_t d[128]; };
constexpr int kAnswers = 8;
struct Box {
  R r[kRoutes];
  Answer ans[kAnswers];
  int rd, wr;
  Stats st;
};
Box s_box[kBoxes];
SinkFn s_sink;
NameFn s_names;

bool box_ok(int b) { return b >= 0 && b < kBoxes; }
bool src_ok(uint8_t s) { return s < kUsbADevices || s == kUsbAAny || s == kUsbC; }
bool chan_ok(uint8_t c) { return c < 16 || c == kAnyChan; }

void count_open(Box& x) {
  int n = 0;
  for (const R& r : x.r) n += r.open;
  x.st.open = n;
}
void send(int box, int slot, uint8_t s, uint8_t a, uint8_t b) {
  if (s_sink) s_sink(box, slot, s, a, b);
}
int held_count(const R& r) {
  int n = 0;
  for (const auto& k : r.held)
    for (const auto& ch : k)
      for (uint32_t w : ch) n += __builtin_popcount(w);
  return n;
}
// route r's held notes (and sustain) from source key k (-1: every source) end: note-offs (and CC 64 0) to its slot when
// tell, else just forgotten
void release(int box, R& r, int k, bool tell) {
  Box& x = s_box[box];
  for (int kk = 0; kk < kKeys; ++kk) {
    if (k >= 0 && kk != k) continue;
    for (int ch = 0; ch < 16; ++ch) {
      for (int w = 0; w < 4; ++w) {
        uint32_t bits = r.held[kk][ch][w];
        if (!bits) continue;
        for (int i = 0; i < 32; ++i)
          if (bits & 1u << i) {
            if (tell) send(box, r.slot, (uint8_t)(0x80 | ch), (uint8_t)(w * 32 + i), 0);
            ++x.st.released;
          }
        r.held[kk][ch][w] = 0;
      }
      if (r.sus[kk] & 1u << ch) {
        if (tell) send(box, r.slot, (uint8_t)(0xB0 | ch), 64, 0);
        r.sus[kk] &= (uint16_t)~(1u << ch);
      }
    }
  }
}

void queue(int box, const uint8_t* body, int len) {
  Box& x = s_box[box];
  if (x.wr - x.rd >= kAnswers || len + 7 > (int)sizeof x.ans[0].d) return;
  Answer& a = x.ans[x.wr % kAnswers];
  const uint8_t head[6] = {0xF0, 0x7D, 0x52, 0x54, 0x51, 1};
  memcpy(a.d, head, 6);
  memcpy(a.d + 6, body, len);
  a.d[6 + len] = 0xF7;
  a.n = (uint8_t)(len + 7);
  ++x.wr;
}
void noted(int box, uint8_t seq, uint8_t owner, int r) {
  const uint8_t b[] = {kNoted, seq, owner, (uint8_t)r};
  queue(box, b, sizeof b);
}
void refused(int box, uint8_t seq, uint8_t owner, Code c) {
  Box& x = s_box[box];
  ++x.st.refused;
  x.st.last_refused = c;
  const uint8_t b[] = {kRefused, seq, owner, c};
  queue(box, b, sizeof b);
}
void devices(int box, uint8_t seq) {
  uint8_t b[3 + kUsbADevices * (2 + kNameLen)];
  int n = 3, count = 0;
  for (int s = 0; s < kUsbADevices; ++s) {
    const char* nm = s_names ? s_names(s) : nullptr;
    if (!nm || !*nm) continue;
    int len = 0;
    while (nm[len] && len < kNameLen) ++len;
    b[n++] = (uint8_t)s;
    b[n++] = (uint8_t)len;
    for (int i = 0; i < len; ++i) b[n++] = (uint8_t)(nm[i] >= 0x20 && nm[i] < 0x7F ? nm[i] : '_');
    ++count;
  }
  b[0] = kDeviceList, b[1] = seq, b[2] = (uint8_t)count;
  queue(box, b, n);
}
// does route r (of the box being fed) take source key k?
bool takes(const R& r, int k) {
  if (!r.open) return false;
  if (k == kKeyUsbC) return r.src == kUsbC;
  return r.src == kUsbAAny || r.src == k;
}
void feed_box(int box, int k, uint8_t s, uint8_t a, uint8_t b) {
  Box& x = s_box[box];
  uint8_t type = s & 0xF0, ch = s & 0x0F;
  bool on = type == 0x90 && b, off = type == 0x80 || (type == 0x90 && !b);
  uint32_t bit = 1u << (a & 31);
  int w = a >> 5;
  uint32_t got = 0;   // slots this message reached
  for (R& r : x.r) {
    if (!takes(r, k) || (r.chan != kAnyChan && r.chan != ch)) continue;
    uint32_t sb = 1u << r.slot;
    if (off) {   // only to the slot the note-on went to
      if (!(r.held[k][ch][w] & bit)) continue;
      r.held[k][ch][w] &= ~bit;
      if (got & sb) continue;
    } else if (got & sb) {
      continue;
    } else if (on) {
      r.held[k][ch][w] |= bit;
    } else if (type == 0xB0 && a == 64) {
      if (b >= 64) r.sus[k] |= (uint16_t)(1u << ch);
      else r.sus[k] &= (uint16_t)~(1u << ch);
    }
    got |= sb;
    send(box, r.slot, s, a, b);
    ++x.st.delivered;
  }
}
}  // namespace

void set_sink(SinkFn f) { s_sink = f; }
void set_names(NameFn f) { s_names = f; }
bool is_op(uint8_t op) { return op >= kNotes && op <= kDevices; }

bool sysex(int box, const uint8_t* m, int n) {
  if (!box_ok(box) || !m || n < 8 || m[0] != 0xF0 || m[1] != 0x7D || m[2] != 0x52 || m[3] != 0x54 || m[4] != 0x51 ||
      m[5] != 1 || m[n - 1] != 0xF7 || !is_op(m[6]))
    return false;
  Box& x = s_box[box];
  const uint8_t* p = m + 7;
  int len = n - 8;
  for (int i = 0; i < len; ++i)
    if (p[i] & 0x80) { ++x.st.ops_bad; return false; }
  switch (m[6]) {
    case kNotes: {
      if (len != 5) { ++x.st.ops_bad; return false; }
      uint8_t seq = p[0], owner = p[1], src = p[2], chan = p[3], slot = p[4];
      if (!src_ok(src) || !chan_ok(chan) || slot >= kSlots) { refused(box, seq, owner, kBad); return false; }
      for (int i = 0; i < kRoutes; ++i) {
        R& r = x.r[i];
        if (r.open) continue;
        memset(&r, 0, sizeof r);
        r.open = true, r.owner = owner, r.src = src, r.chan = chan, r.slot = slot;
        ++x.st.opened;
        count_open(x);
        noted(box, seq, owner, i);
        return true;
      }
      refused(box, seq, owner, kFull);
      return false;
    }
    case kRenote: {
      if (len != 6) { ++x.st.ops_bad; return false; }
      uint8_t seq = p[0], owner = p[1], ri = p[2], src = p[3], chan = p[4], slot = p[5];
      if (ri >= kRoutes || !x.r[ri].open || x.r[ri].owner != owner || !src_ok(src) || !chan_ok(chan) || slot >= kSlots) {
        refused(box, seq, owner, kBad);
        return false;
      }
      R& r = x.r[ri];
      release(box, r, -1, true);   // the old slot hears its notes end before the route points elsewhere
      r.src = src, r.chan = chan, r.slot = slot;
      ++x.st.moved;
      noted(box, seq, owner, ri);
      return true;
    }
    case kUnnote: {
      if (len != 2) { ++x.st.ops_bad; return false; }
      uint8_t owner = p[0], ri = p[1];
      bool any = false;
      for (int i = 0; i < kRoutes; ++i) {
        R& r = x.r[i];
        if (!r.open || r.owner != owner || (ri != kEvery && ri != i)) continue;
        release(box, r, -1, true);
        r.open = false;
        ++x.st.closed;
        any = true;
      }
      count_open(x);
      if (!any) ++x.st.ops_bad;
      return any;
    }
    case kDevices:
      if (len != 1) { ++x.st.ops_bad; return false; }
      devices(box, p[0]);
      return true;
  }
  return false;
}

int take_answer(int box, uint8_t* out, int cap) {
  if (!box_ok(box)) return 0;
  Box& x = s_box[box];
  if (x.rd == x.wr) return 0;
  const Answer& a = x.ans[x.rd % kAnswers];
  ++x.rd;
  if (a.n > cap) { ++x.st.ops_bad; return 0; }   // dropped, never left to block the queue (a 6A takes up to 114)
  memcpy(out, a.d, a.n);
  return a.n;
}

void feed(int src, int box, uint8_t s, uint8_t a, uint8_t b) {
  if (s < 0x80 || s >= 0xF0) return;   // channel voice messages only
  a &= 0x7F, b &= 0x7F;
  if (src == kUsbC) {
    if (box_ok(box)) feed_box(box, kKeyUsbC, s, a, b);
    return;
  }
  if (src < 0 || src >= kUsbADevices) return;
  for (int bx = 0; bx < kBoxes; ++bx) feed_box(bx, src, s, a, b);
}

void gone(int src, int box) {
  if (src == kUsbC) {
    if (!box_ok(box)) return;
    for (R& r : s_box[box].r) if (r.open) release(box, r, kKeyUsbC, true);
    return;
  }
  if (src < 0 || src >= kUsbADevices) return;
  for (int bx = 0; bx < kBoxes; ++bx)
    for (R& r : s_box[bx].r) if (r.open) release(bx, r, src, true);
}

void plugged() {
  for (int bx = 0; bx < kBoxes; ++bx) devices(bx, kUnasked);
}

void reset(int box) {
  if (!box_ok(box)) return;
  Box& x = s_box[box];
  for (R& r : x.r) {
    if (!r.open) continue;
    release(box, r, -1, true);
    r.open = false;
    ++x.st.closed;
  }
  count_open(x);
  x.rd = x.wr;   // answers to the old link's requests are not the new one's
}

void slot_freed(int box, int slot) {
  if (!box_ok(box)) return;
  for (R& r : s_box[box].r) if (r.open && r.slot == slot) release(box, r, -1, false);
}

Route route(int box, int r) {
  if (!box_ok(box) || r < 0 || r >= kRoutes) return {};
  const R& x = s_box[box].r[r];
  return {x.open, x.owner, x.src, x.chan, x.slot, x.open ? held_count(x) : 0};
}
Stats stats(int box) { return box_ok(box) ? s_box[box].st : Stats{}; }
void clear_all() { memset(s_box, 0, sizeof s_box); }

}  // namespace noteroute
}  // namespace core

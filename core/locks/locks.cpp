// core/locks/locks.cpp - see locks.h.
#include "locks.h"

#include <string.h>

namespace core {
namespace locks {

#ifndef ARDUINO   // the Pi and the Mac core; Sidecar Mini (Teensy 4.1) has no processor slots and no RAM to spare: stubs below

struct Set {
  bool on;
  uint8_t track, slot, first, n;
  bool held;                       // a step with locks applied; its locks hold until the next trig or lock step
  uint8_t held_step;
  uint16_t base[kParams];
  uint16_t sent[kParams];
  uint16_t lock[kSteps][kParams];
};
static Set s_sets[kSets];
static Sink s_sink = nullptr;

void set_sink(Sink s) { s_sink = s; }
bool is_lock_op(uint8_t op) { return op >= kSet && op <= kDrop; }

static void clear_set(Set& s) {
  memset(&s, 0, sizeof s);
  for (int p = 0; p < kParams; ++p) s.sent[p] = kNone;
  memset(s.lock, 0xFF, sizeof s.lock);
}

void reset() {
  for (auto& s : s_sets) clear_set(s);
}

static const bool s_init = [] { reset(); return true; }();

static void emit(Set& s, int p, uint16_t v) {
  if (s.sent[p] == v) return;
  s.sent[p] = v;
  if (s_sink) s_sink(s.slot, s.first + p, v);
}

// what parameter p plays now: the held step's lock, else the base
static uint16_t now(const Set& s, int p) {
  if (s.held && s.lock[s.held_step][p] != kNone) return s.lock[s.held_step][p];
  return s.base[p];
}

void tick(int track, int step, bool trig) {
  if (track < 0 || track >= kTracks || step < 0 || step >= kSteps) return;
  for (auto& s : s_sets) {
    if (!s.on || s.track != track) continue;
    bool any = false;
    for (int p = 0; p < s.n && !any; ++p) any = s.lock[step][p] != kNone;
    if (!trig && !any) continue;   // a step with neither keeps what plays
    s.held = any;
    s.held_step = (uint8_t)step;
    for (int p = 0; p < s.n; ++p) emit(s, p, now(s, p));
  }
}

static uint16_t v14(const uint8_t* b) { return (uint16_t)(b[0] << 7 | b[1]); }

bool sysex(const uint8_t* m, int n) {
  if (n < 9 || m[0] != 0xF0 || m[1] != 0x7D || m[2] != 0x52 || m[3] != 0x54 || m[4] != 0x51 || m[5] != 1 || m[n - 1] != 0xF7)
    return false;
  const uint8_t* b = m + 7;
  int bn = n - 8;
  for (int i = 0; i < bn; ++i) if (b[i] & 0x80) return false;
  int set = b[0];
  if (set >= kSets) return false;
  Set& s = s_sets[set];
  switch (m[6]) {
    case kSet: {
      if (bn != 5 || b[1] >= kTracks || b[3] > 63 || b[4] < 1 || b[4] > kParams || b[3] + b[4] > 64) return false;
      clear_set(s);
      s.on = true;
      s.track = b[1]; s.slot = b[2]; s.first = b[3]; s.n = b[4];
      return true;
    }
    case kBase: {
      if (!s.on || bn < 3 || bn != 3 + 2 * b[2] || b[1] + b[2] > s.n) return false;
      for (int i = 0; i < b[2]; ++i) {
        int p = b[1] + i;
        s.base[p] = v14(b + 3 + 2 * i);
        if (!s.held || s.lock[s.held_step][p] == kNone) emit(s, p, s.base[p]);
      }
      return true;
    }
    case kLock: {
      if (!s.on || bn != 5 || b[1] >= kSteps || b[2] >= s.n) return false;
      s.lock[b[1]][b[2]] = v14(b + 3);
      if (s.held && s.held_step == b[1]) emit(s, b[2], now(s, b[2]));   // the held step edited: heard at once
      return true;
    }
    case kClear: {
      if (!s.on || bn != 3 || (b[1] >= kSteps && b[1] != 0x7F) || (b[2] >= s.n && b[2] != 0x7F)) return false;
      for (int st = 0; st < kSteps; ++st)
        if (b[1] == 0x7F || b[1] == st)
          for (int p = 0; p < s.n; ++p)
            if (b[2] == 0x7F || b[2] == p) s.lock[st][p] = kNone;
      if (s.held)
        for (int p = 0; p < s.n; ++p) emit(s, p, now(s, p));
      return true;
    }
    case kTable: {
      if (!s.on || bn < 2) return false;
      // check the whole message before writing any of it
      int at = 2;
      for (int e = 0; e < b[1]; ++e) {
        if (at + 4 > bn || b[at] >= kSteps || b[at + 1] > 3) return false;
        uint32_t mask = (uint32_t)b[at + 1] << 14 | (uint32_t)b[at + 2] << 7 | b[at + 3];
        if (mask >> s.n) return false;
        at += 4 + 2 * __builtin_popcount(mask);
      }
      if (at != bn) return false;
      at = 2;
      for (int e = 0; e < b[1]; ++e) {
        int st = b[at];
        uint32_t mask = (uint32_t)b[at + 1] << 14 | (uint32_t)b[at + 2] << 7 | b[at + 3];
        at += 4;
        for (int p = 0; p < s.n; ++p)
          if (mask >> p & 1) { s.lock[st][p] = v14(b + at); at += 2; }
      }
      if (s.held)
        for (int p = 0; p < s.n; ++p) emit(s, p, now(s, p));
      return true;
    }
    case kDrop: {
      if (bn != 1) return false;
      clear_set(s);
      return true;
    }
  }
  return false;
}

bool declared(int set) { return set >= 0 && set < kSets && s_sets[set].on; }
uint16_t value(int set, int param) { return set >= 0 && set < kSets && param >= 0 && param < kParams ? s_sets[set].sent[param] : kNone; }
uint16_t lock_at(int set, int step, int param) {
  return set >= 0 && set < kSets && step >= 0 && step < kSteps && param >= 0 && param < kParams ? s_sets[set].lock[step][param] : kNone;
}

#else
void set_sink(Sink) {}
bool is_lock_op(uint8_t op) { return op >= kSet && op <= kDrop; }
void tick(int, int, bool) {}
void reset() {}
bool sysex(const uint8_t*, int) { return false; }
bool declared(int) { return false; }
uint16_t value(int, int) { return kNone; }
uint16_t lock_at(int, int, int) { return kNone; }
#endif

}  // namespace locks
}  // namespace core

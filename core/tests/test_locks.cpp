// core/tests/test_locks.cpp - the lock service (core/locks): a set declared on the FX track, base values, locks
// played on their steps and held to the next trig, trigless locks, a lock edited on the held step, CLEAR and TABLE,
// malformed messages leaving everything as it was, reset at a hello.
#include <cstring>
#include <vector>

#include "check.h"
#include "locks/locks.h"

using namespace core::locks;

struct Hit { int slot, param, v; };
static std::vector<Hit> g_hits;
static void sink(int slot, int param, int v) { g_hits.push_back({slot, param, v}); }

static bool send(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x51, 0x01};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return sysex(m.data(), (int)m.size());
}
static int last(int param) {   // the last value the sink got for param, -1 none
  for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it) if (it->param == param) return it->v;
  return -1;
}

int main() {
  set_sink(sink);
  // set 0: the FX track (12), processor slot 1, its parameters 4..11 (8 knobs, as send FX P2..P5 of both FX)
  CHECK(send({kSet, 0, 12, 1, 4, 8}));
  CHECK(declared(0) && !declared(1));
  CHECK(!send({kSet, 1, 13, 1, 0, 4}));   // track 13: no such track
  CHECK(!send({kSet, 1, 0, 1, 60, 8}));   // past parameter 63
  CHECK(!declared(1));

  // base values: each reaches the sink at once, at slot 1, parameter first + i
  CHECK(send({kBase, 0, 0, 8, 0, 10, 0, 11, 0, 12, 0, 13, 0, 14, 0, 15, 0, 16, 0, 17}));
  CHECK(g_hits.size() == 8 && g_hits[0].slot == 1 && g_hits[0].param == 4 && g_hits[0].v == 10 && last(11) == 17);
  CHECK(!send({kBase, 0, 6, 3, 0, 1, 0, 1, 0, 1}));   // past the set's 8
  CHECK(value(0, 6) == 16);

  // locks on step 4 (P0 = 100) and step 8 (P1 = 200, 14 bits: 1 72)
  CHECK(send({kLock, 0, 4, 0, 0, 100}));
  CHECK(send({kLock, 0, 8, 1, 1, 72}));
  CHECK(lock_at(0, 8, 1) == 200 && lock_at(0, 5, 1) == kNone);
  g_hits.clear();

  // other tracks' ticks change nothing; a step with no trig and no lock keeps what plays
  tick(0, 4, true);
  tick(12, 1, false);
  CHECK(g_hits.empty());
  // step 4 with a trig: P0 locked, the rest base (only the change goes out)
  tick(12, 4, true);
  CHECK(g_hits.size() == 1 && last(4) == 100);
  // steps 5..7 no trig: the lock holds
  tick(12, 5, false);
  tick(12, 6, false);
  CHECK(g_hits.size() == 1 && value(0, 0) == 100);
  // step 8, a trigless lock: P1 = 200, and P0 back to its base (the held step moved on)
  tick(12, 8, false);
  CHECK(last(5) == 200 && last(4) == 10);
  // a base change on a locked parameter is not heard while the lock holds; on an unlocked one at once
  g_hits.clear();
  CHECK(send({kBase, 0, 1, 1, 0, 50}));
  CHECK(g_hits.empty() && value(0, 1) == 200);
  CHECK(send({kBase, 0, 2, 1, 0, 51}));
  CHECK(g_hits.size() == 1 && last(6) == 51);
  // a lock edited on the held step (a knob turned with its trig held): heard at once
  CHECK(send({kLock, 0, 8, 1, 0, 77}));
  CHECK(last(5) == 77);
  // a trig on a step without locks: every parameter back to its base
  g_hits.clear();
  tick(12, 12, true);
  CHECK(last(5) == 50 && value(0, 0) == 10);

  // CLEAR one lock, then every lock; on the held step the base comes back
  tick(12, 8, true);
  CHECK(value(0, 1) == 77);
  CHECK(send({kClear, 0, 8, 1}));
  CHECK(value(0, 1) == 50 && lock_at(0, 8, 1) == kNone && lock_at(0, 4, 0) == 100);
  CHECK(send({kClear, 0, 0x7F, 0x7F}));
  CHECK(lock_at(0, 4, 0) == kNone);
  CHECK(!send({kClear, 0, 64, 0}));

  // TABLE: two steps, step 0 P0 + P7, step 63 P3; a malformed one (mask past the set) changes nothing
  CHECK(send({kTable, 0, 2, 0, 0, 1, 1, 0, 30, 0, 37, 63, 0, 0, 8, 0, 33}));
  CHECK(lock_at(0, 0, 0) == 30 && lock_at(0, 0, 7) == 37 && lock_at(0, 63, 3) == 33);
  CHECK(!send({kTable, 0, 1, 5, 0, 2, 0, 0, 9}));        // mask bit 8 of an 8-parameter set
  CHECK(!send({kTable, 0, 1, 5, 0, 0, 1, 0}));           // one value short
  CHECK(lock_at(0, 5, 0) == kNone);
  g_hits.clear();
  tick(12, 0, true);
  CHECK(last(4) == 30 && last(11) == 37);

  // a status byte inside, the wrong version: refused
  { std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x51, 0x01, kLock, 0, 1, 0, 0x80, 0, 0xF7}; CHECK(!sysex(m.data(), (int)m.size())); }
  { std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x51, 0x02, kDrop, 0, 0xF7}; CHECK(!sysex(m.data(), (int)m.size())); }

  // DROP and reset
  CHECK(send({kDrop, 0}));
  CHECK(!declared(0));
  g_hits.clear();
  tick(12, 0, true);
  CHECK(g_hits.empty());
  CHECK(send({kSet, 3, 0, 2, 0, 2}));
  reset();
  CHECK(!declared(3));
  CHECK(is_lock_op(kTable) && !is_lock_op(0x01));

  printf("test_locks: PASS\n");
  return 0;
}

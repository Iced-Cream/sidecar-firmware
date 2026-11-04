// core/tests/test_locks_bind.cpp - the lock service on the engine's generic parts: a 2C tick (5-byte, trig flag) through
// core::clock reaches core/locks, and the resolved values reach the processor in a core::proc slot.
#include <cstring>
#include <vector>

#include "check.h"
#include "clock/clock.h"
#include "locks/locks.h"
#include "proc/proc.h"

using namespace core;

struct Probe : proc::Proc {
  int v[proc::kParams];
  Probe() { for (int& x : v) x = -1; }
  void process(const proc::Block&) override {}
  void set_param(int p, int value) override { v[p] = value; }
  const char* name() const override { return "probe"; }
};

static std::vector<uint8_t> msg(uint8_t cmd, std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, cmd, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
static void run() {   // one render block: the queued params land before process()
  static float buf[proc::kChannels][32];
  proc::Block b;
  for (int c = 0; c < proc::kChannels; ++c) { b.in[c] = buf[c]; b.out[c] = buf[c]; }
  b.frames = 32;
  proc::process(b);
}

int main() {
  Probe pr;
  CHECK(proc::register_slot(2, &pr));
  CHECK(locks::attach());
  auto lk = [](std::vector<uint8_t> body) { auto m = msg(0x51, body); return locks::sysex(m.data(), (int)m.size()); };
  // a set on the FX track feeding slot 2's params 8..9; base 20 / 21; a lock on step 3, param 0 = 90
  CHECK(lk({locks::kSet, 0, 12, 2, 8, 2}));
  CHECK(lk({locks::kBase, 0, 0, 2, 0, 20, 0, 21}));
  CHECK(lk({locks::kLock, 0, 3, 0, 0, 90}));
  run();
  CHECK(pr.v[8] == 20 && pr.v[9] == 21);
  // step 3 fires on track 12 (5-byte tick, trig flag 1): param 8 locked
  auto t = msg(0x2C, {0, 0, 7, 0x68, 1, 12, 3, 16, 2, 1});
  CHECK(clock::sysex(t.data(), (int)t.size()));
  run();
  CHECK(pr.v[8] == 90 && pr.v[9] == 21);
  // step 4, a trig and no lock (4-byte ticks from an older sender carry no trig flag; this one does): base again
  t = msg(0x2C, {0, 0, 7, 0x69, 1, 12, 4, 16, 2, 1});
  CHECK(clock::sysex(t.data(), (int)t.size()));
  run();
  CHECK(pr.v[8] == 20);
  // a 4-byte tick on the lock step (no trig flag): the lock still plays, a trigless lock
  t = msg(0x2C, {0, 0, 7, 0x6A, 1, 12, 3, 16, 2});
  CHECK(clock::sysex(t.data(), (int)t.size()));
  run();
  CHECK(pr.v[8] == 90);
  printf("test_locks_bind: PASS\n");
  return 0;
}

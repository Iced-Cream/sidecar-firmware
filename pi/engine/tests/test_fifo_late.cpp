// fifo_late.h: the FIFO's lateness for the measured FX guard (cores.cpp).
#include <stdio.h>

#include "check.h"
#include "fifo_late.h"

int main() {
  const uint32_t kBlock = 12, kTarget = 96;
  {  // after a stream start: no lateness for the grace, even from an empty FIFO
    fifo_late::State s;
    for (unsigned i = 0; i < fifo_late::State::kGrace; ++i) CHECK(!s.step(0, 0, kBlock, true));
    CHECK(s.step(0, 0, kBlock, true));
    CHECK(!s.step(0, 1, kBlock, true));   // a new stream: the grace again
  }
  {  // LT12h P5's shape: each control pass renders 8 blocks, the first from a FIFO near empty, the rest refilled.
     // Every block now counts late while it lasts (4 in a row: the late rule acts)
    fifo_late::State s;
    for (unsigned i = 0; i < fifo_late::State::kGrace; ++i) s.step(kTarget, 0, kBlock, true);
    unsigned run = 0, best = 0;
    for (int pass = 0; pass < 10; ++pass)
      for (uint32_t b = 0; b < 8; ++b) {
        const bool late = s.step(3 + b * kBlock, 0, kBlock, true);
        run = late ? run + 1 : 0;
        if (run > best) best = run;
      }
    CHECK(best >= 4);
    printf("test_fifo_late: a drained FIFO refilled each pass: %u late blocks in a row\n", best);
  }
  {  // a healthy FIFO: never late; one dip: late for kHold passes, then on time again
    fifo_late::State s;
    for (unsigned i = 0; i < fifo_late::State::kGrace; ++i) s.step(kTarget, 0, kBlock, true);
    for (int i = 0; i < 1000; ++i) CHECK(!s.step(60 + i % 36, 0, kBlock, true));
    bool dip = false;
    CHECK(s.step(5, 0, kBlock, true, &dip) && dip);
    for (unsigned i = 1; i < fifo_late::State::kHold; ++i) CHECK(s.step(90, 0, kBlock, true, &dip) && !dip);
    CHECK(!s.step(90, 0, kBlock, true));
  }
  {  // fxguard=fifo:off: never, and the hold ends at once
    fifo_late::State s;
    for (unsigned i = 0; i < fifo_late::State::kGrace; ++i) s.step(kTarget, 0, kBlock, true);
    CHECK(s.step(0, 0, kBlock, true));
    CHECK(!s.step(0, 0, kBlock, false));
    CHECK(!s.step(90, 0, kBlock, true));
  }
  printf("test_fifo_late: PASS\n");
  return 0;
}

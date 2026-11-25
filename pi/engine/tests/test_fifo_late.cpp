// fifo_late.h: the FIFO's lateness for the measured FX guard (cores.cpp).
#include <stdio.h>

#include "check.h"
#include "fifo_late.h"

static const uint32_t kBlock = 12, kTarget = 96;
static void warm(fifo_late::State& s) {
  for (unsigned i = 0; i < fifo_late::State::kGrace + fifo_late::State::kHold; ++i) s.step(kTarget, 0, kBlock, true);
}

int main() {
  {  // after a stream start: no lateness for the grace, even from an empty FIFO
    fifo_late::State s;
    for (unsigned i = 0; i < fifo_late::State::kGrace; ++i) CHECK(!s.step(0, 0, kBlock, true));
    CHECK(!s.step(0, 1, kBlock, true));   // a new stream: the grace again
  }
  {  // LT12h P5's shape: each control pass renders 8 blocks, the first from a FIFO near empty, the rest refilled.
     // From the second pass every block counts late (4 in a row: the late rule acts)
    fifo_late::State s;
    warm(s);
    unsigned run = 0, best = 0, first = 0, n = 0;
    for (int pass = 0; pass < 10; ++pass)
      for (uint32_t b = 0; b < 8; ++b, ++n) {
        const bool late = s.step(3 + b * kBlock, 0, kBlock, true);
        if (late && !first) first = n;
        run = late ? run + 1 : 0;
        if (run > best) best = run;
      }
    CHECK(best >= 4 && first == 8);
    printf("test_fifo_late: a FIFO short at every pass: late from block %u, %u in a row\n", first, best);
  }
  {  // one long control pass in normal play: the FIFO dips once (2 low blocks while it refills), never late
    fifo_late::State s;
    warm(s);
    for (int i = 0; i < 1000; ++i) CHECK(!s.step(60 + i % 36, 0, kBlock, true));
    bool dip = false;
    CHECK(!s.step(5, 0, kBlock, true, &dip) && dip);
    CHECK(!s.step(17, 0, kBlock, true, &dip) && !dip);   // (the same low run: not a second dip)
    for (int i = 0; i < 200; ++i) CHECK(!s.step(29 + i % 60, 0, kBlock, true));
    // ... and another long pass well after it: still one dip in the window
    CHECK(!s.step(10, 0, kBlock, true, &dip) && dip);
    for (int i = 0; i < 40; ++i) CHECK(!s.step(60, 0, kBlock, true));
  }
  {  // the mark: 2 blocks (LT12i P5): 23 frames is low, 24 is not
    fifo_late::State s;
    warm(s);
    CHECK(!s.step(24, 0, kBlock, true) && !s.step(60, 0, kBlock, true) && !s.step(24, 0, kBlock, true));
    bool dip = false;
    s.step(23, 0, kBlock, true, &dip);
    CHECK(dip);
    s.step(60, 0, kBlock, true);
    CHECK(s.step(23, 0, kBlock, true));   // the second dip within the window: late
    for (unsigned i = 1; i < fifo_late::State::kHold; ++i) CHECK(s.step(90, 0, kBlock, true));
    CHECK(!s.step(90, 0, kBlock, true));
  }
  {  // fxguard=fifo:off: never, and the hold ends at once
    fifo_late::State s;
    warm(s);
    s.step(0, 0, kBlock, true), s.step(60, 0, kBlock, true);
    CHECK(s.step(0, 0, kBlock, true));
    CHECK(!s.step(0, 0, kBlock, false));
    CHECK(!s.step(90, 0, kBlock, true));
  }
  {  // held(): the hold seen from the other FIFO's passes, never in the grace
    fifo_late::State s;
    CHECK(!s.held());
    for (unsigned i = 0; i < fifo_late::State::kGrace; ++i) s.step(90, 0, kBlock, true);
    CHECK(!s.held());
    s.step(0, 0, kBlock, true), s.step(90, 0, kBlock, true);
    CHECK(!s.held());
    CHECK(s.step(0, 0, kBlock, true) && s.held());   // the second dip: held for the other passes too
    for (unsigned i = 1; i < fifo_late::State::kHold; ++i) s.step(90, 0, kBlock, true);
    CHECK(!s.held());
    printf("test_fifo_late: held() for the other FIFO's passes\n");
  }
  printf("test_fifo_late: PASS\n");
  return 0;
}

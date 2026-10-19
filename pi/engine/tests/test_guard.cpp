// pi/engine/tests/test_guard.cpp - the voice guard's bookkeeping (guard.h's inline part): three blocks in a row over
// the budget mute a voice, one block under resets the count; a fault's Recover mutes the voice that was rendering;
// each mute is news once; Unmute clears every voice and the counts; the first fatal reason wins.
#include <cstring>
#include <initializer_list>

#include "check.h"
#include "guard.h"

int main() {
  static CGuard g;
  g.SetBudget(100);
  CHECK(g.Budget() == 100 && g.MutedMask() == 0);
  // over, over, under, over, over: never three in a row
  for (u32 t : {101u, 200u, 50u, 101u, 101u}) g.Leave(1, 2, t);
  CHECK(!g.Muted(2) && g.TakeNews() == 0);
  CHECK(g.MaxTicks(2) == 200);
  g.Leave(1, 2, 150);   // the third in a row
  CHECK(g.Muted(2) && g.MutedMask() == (1u << 2));
  CHECK(strcmp(g.Why(2), "over budget") == 0);
  CHECK(g.TakeNews() == (1u << 2) && g.TakeNews() == 0);   // logged once
  CHECK(!g.Armed(1));
  // exactly the budget is within it
  for (int i = 0; i < 5; i++) g.Leave(2, 5, 100);
  CHECK(!g.Muted(5));
  // a fault: Recover mutes the core's current voice (set by Enter, which is Pi-only: guard.cpp; before any Enter,
  // voice 0) and disarms the core
  CHECK(g.Recover(2) == 0 && g.Muted(0) && strcmp(g.Why(0), "crashed") == 0 && !g.Armed(2));
  CHECK(g.TakeNews() == 1u);
  // Unmute: every voice back, the over-budget counts too (two more overs do not mute)
  g.Leave(1, 3, 500);
  g.Leave(1, 3, 500);
  g.Unmute();
  CHECK(g.MutedMask() == 0 && g.MaxTicks(2) == 0 && g.MaxTicks(3) == 0);
  g.Leave(1, 3, 500);
  g.Leave(1, 3, 500);
  CHECK(!g.Muted(3));
  g.Leave(1, 3, 500);
  CHECK(g.Muted(3));
  // the first fatal reason is kept
  CHECK(g.FatalWhy() == nullptr);
  g.Fatal("first");
  g.Fatal("second");
  CHECK(strcmp(g.FatalWhy(), "first") == 0);
  printf("test_guard: PASS\n");
  return 0;
}

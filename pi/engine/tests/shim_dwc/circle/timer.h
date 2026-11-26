// shim_dwc/circle/timer.h - the clock of test_gadget_ep0: one microsecond per register read (g_FakeClock, FakeRead)
#pragma once
extern unsigned g_FakeClock;
class CTimer {
public:
  static void SimpleusDelay (unsigned) {}
  static unsigned GetClockTicks (void) { return g_FakeClock; }
};

#pragma once
#define CLOCKHZ 1000000
class CTimer {
 public:
  static CTimer* Get() { static CTimer t; return &t; }
  static unsigned GetClockTicks() { return s_now; }
  static unsigned long long GetClockTicks64() { return s_now64 ? s_now64 : s_now; }
  static inline unsigned long long s_now64 = 0;
  void MsDelay(unsigned) {}
  static inline unsigned s_now = 0;
};

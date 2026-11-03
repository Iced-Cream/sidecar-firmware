// pi/engine/a4_in.h - the A4's MAIN on its way into the Rytm's PRE pair: drift_src.h between the A4's IN completion and the Rytm's OUT fill. Hosted (no
// Circle headers), as sendfx_pi.h: ob_follow.cpp calls it. Both calls run in the xHCI interrupts of core 0, which do not nest, and neither waits.
#pragma once
#include <stdint.h>

namespace a4in {
void Init(unsigned left_channel, unsigned att_db);                 // once, task level, before the first stream: the filter's table
void In(const uint8_t* block);                                     // the A4's IN completion: a 256-byte block
void Mix(int32_t (*out)[12], unsigned n);                          // the OUT fill: n (at most 7) frames onto the device's PRE pair (saturating)
struct Stats {
  int fill, ppm;                                                   // frames in the ring, smoothed; the Rytm's rate against the A4's
  uint32_t under, low, high, full;                                 // drift_src.h's counters
};
Stats Read();
}  // namespace a4in

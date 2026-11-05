// core/locks/locks_bind.cpp - the lock service on the engine's generic parts: the step clock's ticks in
// (core::clock::on_tick; the tick's fifth byte, bit 0, says the step holds a trig - a sender without it gets locks on
// their own steps only) and resolved values out to the processor slots (core::proc::set_param, the control core's,
// where the host link and the ticks arrive).
#include "../clock/clock.h"
#include "../proc/proc.h"
#include "../route/route.h"
#include "locks.h"

namespace core {
namespace locks {

// the tick's values to the route's gains land by the tick's block (core/route stamp); processor slots take them at once
static void on_tick(const clock::Tick& t) {
  route::stamp(t.block);
  tick(t.track, t.step, t.extra & 1);
  route::stamp(-1);
}
// target route::kTarget (4) is the audio route's gains: param = the point (core/route)
static void to_proc(int slot, int param, int v) {
  if (slot == route::kTarget) route::set_gain(param, v);
  else proc::set_param(slot, param, v);
}

bool attach() {
#ifndef ARDUINO
  set_sink(to_proc);
  return clock::on_tick(on_tick);
#else
  (void)on_tick; (void)to_proc;
  return false;
#endif
}

}  // namespace locks
}  // namespace core

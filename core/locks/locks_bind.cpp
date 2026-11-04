// core/locks/locks_bind.cpp - the lock service on the engine's generic parts: the step clock's ticks in
// (core::clock::on_tick; the tick's fifth byte, bit 0, says the step holds a trig - a sender without it gets locks on
// their own steps only) and resolved values out to the processor slots (core::proc::set_param, the control core's,
// where the host link and the ticks arrive).
#include "../clock/clock.h"
#include "../proc/proc.h"
#include "locks.h"

namespace core {
namespace locks {

static void on_tick(const clock::Tick& t) { tick(t.track, t.step, t.extra & 1); }
static void to_proc(int slot, int param, int v) { proc::set_param(slot, param, v); }

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

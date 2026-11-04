// core/locks/locks.h - the Sidecar's generic parameter locks for Rytm mods (host-link 51 STORE's lock ops, Storage
// phase 2026-11-04). A mod on the Rytm declares a parameter set (a track, a processor slot and a range of that slot's
// parameters); the Rytm keeps the set's base values and the working pattern's lock table (its cache is the master: it
// saves them through core/kv records) and mirrors both here; the Pi plays the locks on the Rytm's step clock and hands
// each resolved value to the sink (core::proc::set_param in the engine). Nothing here knows what a parameter means.
//
// Playing follows stock's rule: a step that holds a trig or a lock applies that step's locks, and every parameter
// without one takes its base; the values hold until the track's next such step, through STOP and a pattern change.
// A base change reaches a parameter that is not locked at once.
//
// Portable (Pi, Mac core, Linux tests). One core drives it (the Pi's core 3, where the host link and the clock's
// ticks arrive); the sink queues to the render core itself.
#pragma once
#include <stdint.h>

namespace core {
namespace locks {

constexpr int kSets = 8, kParams = 16, kSteps = 64, kTracks = 13;
constexpr uint16_t kNone = 0xFFFF;   // no lock on that step

using Sink = void (*)(int slot, int param, int value14);
void set_sink(Sink s);   // null: values are resolved and dropped (the tests' default)
// locks_bind.cpp: the engine's wiring - ticks from core::clock, values to core::proc::set_param; false: no listener room
bool attach();

// 51 01 1x ...: the lock ops (the same message shape as core/kv; seq-less, the Rytm resends a set on the Pi's HELLO)
//   10 SET   <set 0..7> <track 0..12> <slot> <first param 0..63> <n 1..16>   declares (or redeclares: cleared) a set
//   11 BASE  <set> <first> <n> {<hi> <lo>} x n                              base values, 14 bits
//   12 LOCK  <set> <step 0..63> <param> <hi> <lo>                          one lock (a knob turned with a trig held)
//   13 CLEAR <set> <step | 7F every step> <param | 7F every param>
//   14 TABLE <set> <n> {<step> <mask hi2> <mask mid7> <mask lo7> {<hi> <lo>} x popcount}   locks added in bulk (a
//            pattern load sends CLEAR 7F 7F, then TABLEs)
//   15 DROP  <set>                                                         the set goes (its mod left the build)
enum Op : uint8_t { kSet = 0x10, kBase = 0x11, kLock = 0x12, kClear = 0x13, kTable = 0x14, kDrop = 0x15 };
bool is_lock_op(uint8_t op);
bool sysex(const uint8_t* m, int n);   // a whole 51 message with a lock op; false: malformed, nothing changed

// The step clock (core::clock's on_tick): track 0..12 fired step 0..63; trig: the step holds a trig
void tick(int track, int step, bool trig);
// The Rytm's hello (a reconnect): every set forgotten, the Rytm declares them again
void reset();

// tests / /status
bool declared(int set);
uint16_t value(int set, int param);   // the value last handed to the sink (kNone: never)
uint16_t lock_at(int set, int step, int param);

}  // namespace locks
}  // namespace core

// core/dsp/dsp.h - the DSP library and its graphs (the send FX phase, decided 2026-11-05: a library of general
// primitives in the engine; a Rytm mod sends a graph that says which primitives sit in which processor slots, how route
// points join them and their params; the Pi builds it). Nothing here is tied to one mod: the Pi knows "a DELAY in slot
// 3, wired to slot 4", never what the mod calls the effect. Shipped code (a block whose code comes from the Rytm) is a
// later fallback on the same slots.
//
// A graph is:
//   blocks       PLACE puts a library block into a processor slot (core/proc); FREE takes it out
//   connections  route points (core/route): a stem into a slot's input, a slot's output into another slot's input,
//                a slot's output onto OUT; the route runs slots in graph order and refuses loops
//   params       the lock service (core/locks) with target = the slot and param = the block's param: BASE for the
//                static values, LOCK for p-locks
//
// Host link (51 STORE, version 1; the control core's, as the route ops):
//   Rytm -> Pi  30 PLACE <seq> <owner> <slot 0..15 | 7F any free> <name: 1..8 ASCII>   answer 70 or 71
//               31 FREE  <owner> <slot | 7F every slot of owner>                       no answer: the slot's points
//                        fade out (whoever owns them), then the block goes and the slot is free again
//               32 INFO  <seq> <what> <a> <b>                                          answer 72
//                        what 0: library block a (a 0..): its name and param count; past the end: an empty name
//                        what 1: block a's param b: its name and default (hi, lo)
//   Pi -> Rytm  70 PLACED <seq> <owner> <slot>      71 REFUSED <seq> <owner> <code> (3 full, 5 bad, 9 unknown block)
//               72 TEXT <seq> <what> <a> <b> <n> <n bytes>  (what 0: name, then <nparams>; what 1: name, then <hi> <lo>)
// Lifecycle: the companion's HELLO frees every slot (the route closes every point with it), as for the lock sets.
//
// Memory: each slot has a fixed piece of one arena (begin()), where a block is built by placement new on PLACE and
// destroyed once FREE's fade is over and the render side has let go of it (core/proc released). No allocation.
// Threads: everything here on the control core; the blocks run on the render side through core/proc.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../proc/proc.h"

namespace core {
namespace dsp {

constexpr int kSlots = proc::kSlots;
constexpr int kNameMax = 8;
constexpr size_t kSlotBytes = 1u << 20;   // a slot's memory: the largest block (DELAY, ~776 KB) fits
enum Op : uint8_t { kPlace = 0x30, kFree = 0x31, kInfo = 0x32, kPlaced = 0x70, kRefused = 0x71, kText = 0x72 };
enum Code : uint8_t { kFull = 3, kBad = 5, kUnknown = 9 };

// the library
struct ParamInfo { const char* name; uint16_t def; };
struct BlockInfo {
  const char* name;            // PLACE's name: upper case, at most kNameMax
  int nparams;
  const ParamInfo* params;
  size_t bytes;                // sizeof the block
  proc::Proc* (*make)(void* mem, const void* ctx);   // built into mem (kSlotBytes, 64-byte aligned); null: refused
  const void* ctx = nullptr;   // make's own (a shipped block: its loaded blob, core/code)
  int owner = -1;              // -1 any owner may PLACE it; -2 none; else only this owner (a shipped block's offerer)
};
int library_size();                       // the built-ins, then the shipped blocks loaded now (core/code)
const BlockInfo* library(int i);          // null past the end
const BlockInfo* find(const char* name);  // null: unknown
// Shipped code (core/code, control core): a loaded blob joins the library under its name until removed. add: false
// when the name is taken or there is no room (kShipped). The caller keeps *b alive while it is in the library.
constexpr int kShipped = 16;
bool add_block(const BlockInfo* b);
void remove_block(const BlockInfo* b);
bool builtin(const char* name);           // a built-in block's name
int users(const BlockInfo* b);            // slots holding b (in any state but empty)

// arena: kSlots x kSlotBytes, 64-byte aligned (the Pi: from the heap at boot; tests: any buffer). False: too small.
bool begin(void* arena, size_t bytes);

bool is_dsp_op(uint8_t op);
bool sysex(const uint8_t* m, int n);      // a whole 51 message with a dsp op; false: malformed or refused
int take_answer(uint8_t* out, int cap);   // the next queued answer (70 / 71 / 72), whole F0..F7; 0: none
void poll();                              // control core, often (host_step): FREEs move on (fade, release, destroy)
void reset();                             // the companion's HELLO: every slot freed

// tests / /status / console
enum State : uint8_t { kEmpty = 0, kPlacedState = 1, kFading = 2, kReleasing = 3 };
struct SlotView { State state; uint8_t owner; const char* block; };
SlotView slot(int s);
struct Stats { uint32_t placed, freed, refused, infos, ops_bad; int last_refused; int used; };
Stats stats();

}  // namespace dsp
}  // namespace core

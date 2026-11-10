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
//               39 CHAIN <seq> <owner> <slot 0..15 | 7F> <notes 1..8> <weight> <nb 1..4> {<len> <name>} x nb <nm 0..4>
//                        {<src> <dst>} x nm                                           answer 70 or 71
//                        (2026-11-10) a POLY (poly.h) into the slot: nb library blocks chained, built once a note;
//                        weight b << 4 | ctl, the block whose control output weighs a note in the headroom (7F: 1 a
//                        note); the internal mod points src b << 4 | ctl onto dst b << 4 | param, their depths POLY's
//                        params 68..71. Refused 5 (bad, a name POLY), 9 (unknown name), 3 (no slot, or too big for
//                        a slot's memory). Then the slot is as a PLACEd one: its params 16 b + p, 64.., TRIG, FREE.
//               37 TRIG  <owner> <slot> <kind> <note> <vel> <blk4> [<first> <n> {<hi> <lo>} x n]   no answer
//                        (2026-11-10, Pi voices, option B): a Rytm voice machine's trig, to the block in that slot
//                        (placed by owner) as its midi() with src proc::kSrcRytm: kind 0 a trig = note on (vel 0 sent
//                        as 1), 1 a release = note off. The optional tail sets the block's params first..first+n-1
//                        (14 bits) before the note, in the same block: the values the trig plays with (its p-locks).
//                        blk4: the Rytm's audio block (1500 a second, 4 x 7 bits, MSB first) the trig started in;
//                        carried for sequenced trigs sent ahead, not used yet
//               38 PARAM <owner> <slot> <first> <n> {<hi> <lo>} x n                     no answer: the block's params
//                        straight to it, for values the Rytm resolves itself (a voice machine's knobs as stock plays
//                        them: p-locks, LFO, slides); the lock service's sets are for values the Pi plays
//               TRIG / PARAM to a slot not placed or not the owner's, or past the block's params: dropped (ops_bad)
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
enum Op : uint8_t { kPlace = 0x30, kFree = 0x31, kInfo = 0x32, kChainOp = 0x39, kTrig = 0x37, kParam = 0x38, kPlaced = 0x70, kRefused = 0x71, kText = 0x72 };
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
struct Stats { uint32_t placed, freed, refused, infos, ops_bad, trigs, params; int last_refused; int used; uint32_t last_blk; };
Stats stats();

}  // namespace dsp
}  // namespace core

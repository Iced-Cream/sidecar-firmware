// core/proc/proc.h - the generic audio return (2026-11-04, the engine strip-down): kSlots processor slots between the
// Rytm's Overbridge IN and what the Sidecar sends back on Overbridge OUT. The engine has no effect of its own: a slot is
// whatever registers in it (C++ today; packages later, not decided). An empty slot costs nothing, and with every slot
// empty OUT is bit for bit the pass-through it was. The master FX, the send FX, the inserts and the voices that used to
// write into OUT stay in the tree, unwired.
//
// Parameters: one generic sink, set_param(slot, param, value): what the slot holds decides what a param means. The
// Pi-side lock service and the host link feed it, never knowing what a slot holds. Values are 14 bits (0..16383, as the
// host link's two 7-bit bytes); params 0..kParams-1. A value for an empty slot, or past kParams, is dropped.
//
// Threads: register_slot / clear_slot / set_param / midi on the control side (the Pi's core 3, one producer). process() on the
// render side (one consumer): it drains the parameter queue first, then runs each filled slot in order. Portable.
#pragma once
#include <stdint.h>

namespace core {
namespace proc {

constexpr int kSlots = 16;          // 4 until the send FX phase (2026-11-05): graphs of library blocks (core/dsp)
constexpr int kParams = 128;      // 64 until 2026-11-10 (POLY: four blocks of 16 per note, and its own)
constexpr int kChannels = 12;       // Overbridge: 12 IN, 12 OUT
constexpr int kMaxFrames = 256;     // frames a process() call may carry

constexpr int kCtls = 4;            // control outputs a processor may give (mod points, 2026-11-10)
constexpr int kMods = 32;           // mod points (core/route's op 24 MOD places them)
constexpr int kSlotIO = 2;          // each slot's own input and output channels (L, R): the route's slot channels
// One block: in[c][i] the Rytm's IN channel c (read only), out[c][i] what goes back on OUT channel c. A processor adds to
// out (others may have written there before it); it never clears it. sin[k] is this slot's own input channel k (what
// the route's points to the slot summed; read only), sout[k] its own output channel k (zeroed before the slot runs;
// the route's points from the slot take it on to OUT, core/route). process() sets sin / sout for each slot in turn;
// both null when the engine has no slot channels.
struct Block {
  const float* in[kChannels];
  float* out[kChannels];
  int frames;
  const float* sin[kSlotIO];
  float* sout[kSlotIO];
};
class Proc {
 public:
  virtual ~Proc() {}
  virtual void process(const Block& b) = 0;            // render side
  virtual void set_param(int param, int value14) = 0;  // render side, from the queue, before process()
  virtual void link_reset() {}                         // the Rytm's link went down / came up: tails cleared
  // a message from a USB-A MIDI device (core/devmidi; src its number), render side, from the queue, before process()
  virtual void midi(int src, uint8_t status, uint8_t d1, uint8_t d2) {}
  virtual const char* name() const = 0;
  // control output k (0..kCtls-1), -1..1, as this block's process() left it (an envelope, an LFO): what a mod point
  // carries onto another slot's param. Render side. 0 for a processor that has none.
  virtual float ctl(int k) const { return 0.0f; }
  // a note it played has ended and it is silent until the next (an envelope at rest, a one-shot past its end): a host
  // of notes (core/dsp's POLY) frees the note's chain. Render side. False for a processor that cannot tell.
  virtual bool done() const { return false; }
  // A host of notes (core/dsp's POLY) builds one copy of a block a note. share(first): this copy is not the first, and
  // first (the first copy of the same block) holds what the copies share (a live input's ring); voice(b, last): the
  // first copy, once every block before any note's copy runs (sounding or not), with the host's input in b.sin and
  // its own output of the last block (L; null: none) - the voice-wide work (recording that ring). Neither is called
  // outside a host: such a block does its voice-wide work in process(). Control side / render side.
  virtual void share(Proc* first) {}
  virtual void voice(const Block& b, const float* last) {}
};

bool register_slot(int slot, Proc* p);   // false: slot out of range or taken
void clear_slot(int slot);               // the slot empty from the next block on (the Proc is not deleted here)
Proc* slot(int slot);                    // what is registered, null for empty
bool any();                              // a slot is filled: process() has work
bool set_param(int slot, int param, int value14);   // queued; false: dropped (empty slot, range, queue full)
void link_reset();                       // every filled slot's link_reset(), on the render side (queued)
bool midi(int src, uint8_t status, uint8_t d1, uint8_t d2);   // to every filled slot (queued); false: none, queue full
// One slot's own note event (core/dsp's TRIG op: a Rytm voice machine's trig, 2026-11-10): the slot's midi() with src
// kSrcRytm, render side, from the same queue, before process(). False: empty slot, queue full.
constexpr int kSrcRytm = 0x7F;      // midi()'s src for the Rytm's own trigs (devmidi's device numbers stay below it)
bool note(int slot, uint8_t status, uint8_t d1, uint8_t d2);

// Mod points (2026-11-10, Connor's pick for the voice graphs: "mod points in the route"): mod point id (0..kMods-1)
// carries slot src's control output ctl onto slot dst's param, at a depth (14 bits, 8192 none, 0 = -1, 16383 = +1 x
// the param's whole range). Before dst runs, each of its modulated params is set to its base (the last set_param, or
// the block's default from set_default) plus the sum of its points' ctl x depth, clamped; when the last point on a
// param goes, the base again. core/route places them (loop refusal, graph order: src runs before dst); queued as the
// params. False: range, queue full.
bool set_mod(int id, int src, int ctl, int dst, int param, int depth14);
bool set_mod_depth(int id, int depth14);
bool clear_mod(int id);
// a param's base before any set_param (core/dsp's PLACE: the block's defaults), not sent to the block
bool set_default(int slot, int param, int value14);

// Render side, once a block: begin() drains the parameter queue, run() runs one slot (if filled) with its own channels
// (slot_in / slot_out: kSlots x kSlotIO buffers, slot s channel k at [kSlotIO s + k], route.h's slot channel
// numbering; null: none), end() closes the block. The engine calls run() in the route's graph order (core/route order).
void begin();
void run(int slot, const Block& b, float* const* slot_in, float* const* slot_out);
void end();
// all three, every slot in slot order
void process(const Block& b, float* const* slot_in = nullptr, float* const* slot_out = nullptr);
// control side, after clear_slot(s): the render side can no longer be inside s's Proc (it is between blocks, or began
// one after the clear); false: ask again later. What a cleared Proc's memory may be reused after.
bool released(int slot);

struct Stats { uint32_t params, dropped, blocks, midi, notes, mods; };
Stats stats();
}  // namespace proc
}  // namespace core

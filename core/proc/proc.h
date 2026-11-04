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

constexpr int kSlots = 4;
constexpr int kParams = 64;
constexpr int kChannels = 12;       // Overbridge: 12 IN, 12 OUT
constexpr int kMaxFrames = 256;     // frames a process() call may carry

// One block: in[c][i] the Rytm's IN channel c (read only), out[c][i] what goes back on OUT channel c. A processor adds to
// out (others may have written there before it); it never clears it.
struct Block {
  const float* in[kChannels];
  float* out[kChannels];
  int frames;
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
};

bool register_slot(int slot, Proc* p);   // false: slot out of range or taken
void clear_slot(int slot);               // the slot empty from the next block on (the Proc is not deleted here)
Proc* slot(int slot);                    // what is registered, null for empty
bool any();                              // a slot is filled: process() has work
bool set_param(int slot, int param, int value14);   // queued; false: dropped (empty slot, range, queue full)
void link_reset();                       // every filled slot's link_reset(), on the render side (queued)
bool midi(int src, uint8_t status, uint8_t d1, uint8_t d2);   // to every filled slot (queued); false: none, queue full

void process(const Block& b);            // render side

struct Stats { uint32_t params, dropped, blocks, midi; };
Stats stats();
}  // namespace proc
}  // namespace core

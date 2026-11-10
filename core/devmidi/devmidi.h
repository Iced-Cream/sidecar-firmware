// core/devmidi/devmidi.h - the MIDI devices in the Pi's USB-A ports (2026-11-04, the engine strip-down): a keyboard, a
// controller, an interface, anything that is not the Rytm and not another Elektron box. A generic service: no routing
// of its own (the full engine's core/keys picked the Rytm's selected track, played the poly voices and recorded into the
// Pi's sequencer: cut). Each device's messages go
//   1. to the Rytm, unchanged (thru, on by default): the Sidecar as a USB MIDI hub for the Rytm;
//   2. to the processor slots (core/proc: Proc::midi), when a slot is filled;
//   3. to the listeners (on_midi), on the control side: what a later service wants of them.
// Channel messages and the one-byte realtime ones (clock, start, continue, stop); no SysEx, no song position. A device
// pulled out (or thru turned off) with notes still held: their note-offs go everywhere the note-ons went.
//
// Threads: the control side (the Pi's core 3) for everything here; the platform queues the USB packets to it and sends
// what goes to the Rytm (set_forward) between its own messages. Portable.
#pragma once
#include <stdint.h>

namespace core {
namespace devmidi {

constexpr int kSrcs = 4;   // devices, in plug order
constexpr int kNameLen = 24;
using ForwardFn = void (*)(uint8_t status, uint8_t d1, uint8_t d2);
using ListenFn = void (*)(int src, uint8_t status, uint8_t d1, uint8_t d2);

void set_forward(ForwardFn f);   // the platform's way to the Rytm (null: nothing goes to it)
bool on_midi(ListenFn f);        // 4 listeners; false: no room
void set_thru(bool on);          // to the Rytm or not (off: the held notes' note-offs sent first)
bool thru();

void plugged(int src, bool on, const char* name);              // a device came (on) or went
void midi(int src, uint8_t status, uint8_t d1, uint8_t d2);    // one message from device src

bool present(int src);
const char* name(int src);       // "" when none
// Out to the devices (2026-11-10, MIDIOUT on the library): the render side queues channel messages for the USB-A
// devices (one producer: the render side; one consumer: the platform's USB loop, core 0 on the Pi, which sends them
// without waiting: pi/engine/midiout_q.h). dst 0 every device, 1..kSrcs device dst. False: the queue is full (dropped,
// counted). take_out: the next as a USB MIDI event packet word (little-endian: CIN in the low nibble, the dst in the
// cable nibble, the bytes above; the platform sends it on cable 0).
bool out(int dst, uint8_t status, uint8_t d1, uint8_t d2);
bool take_out(uint32_t* w);
struct Stats { uint32_t messages, forwarded, to_slots, released, ignored, out, out_dropped; };
Stats stats();
}  // namespace devmidi
}  // namespace core

// core/noteroute/noteroute.h - note routes (port plan S8l, decided 2026-11-28, scope section 8 "Keyboards (keys-in)"):
// a box's mod asks for a keyboard's notes - a USB-A MIDI device (core/devmidi) or the computer's MIDI on the box's own
// USB-C port - by device and channel, to go to one of its own box's processor slots (core/proc: an instrument, a
// TRIG -> MIDIOUT graph). sidecar-main rebuilds the old SELECTED / CHANNEL keys-in by moving its route. Nothing else
// changes: thru to both boxes (devmidi's step 1, usba=thru|off) stays as it is, this service only reads what the
// platform feeds it and never consumes, delays or alters what thru forwards.
//
// What a route carries: the channel voice messages (8x note off, 9x note on, Ax poly pressure, Bx control change, Cx
// program, Dx channel pressure, Ex pitch bend) of its source and channel, so bend, the mod wheel and sustain play with
// the notes. Not realtime (clock, transport: core/clock and the USB-A output rules), no SysEx.
//
// Decisions (2026-11-28, S8l):
//   - "its own box's slots" = the asking box's processor slots, numbered 0..15 in that box. A route can only name a slot
//     of the box that asked: the box is the link the request came on, never a field of the request.
//   - Sources: 0..3 a USB-A device in plug order (devmidi's src), 0F any USB-A device, 10 the asking box's own USB-C
//     MIDI port. Another box's USB-C port cannot be routed (the S8c rule: the computer's MIDI reaches only that port's
//     box). 2B DEVICES names the USB-A devices, so a mod can pick a keyboard by name.
//   - Channel 0..15, or 7F any channel.
//   - One message reaches a slot once, however many of the box's routes match it (no doubled note-ons).
//   - Held notes: a note-on a route delivered is remembered (per route, per source, per channel); its note-off goes to
//     the slot that got the note-on, and only there. When a route moves (29), closes (2A), its box's link resets (a
//     box HELLO / another box) or its source goes (unplug, USB-C port gone), every note it still holds gets its
//     note-off (8x n 0) and every channel it left sustained a CC 64 0, at once. A slot that was freed (core/dsp FREE)
//     loses its held notes without note-offs (no processor to tell); its routes stay open for a new processor.
//   - Both boxes keep their own table of kRoutes routes; route numbers are per box.
//
// Host link (51 STORE, version 1, box <-> Pi; sim/protocol.py "note routes"):
//   box -> Pi  28 NOTES   <seq> <owner> <src> <chan> <slot>            a new route; answer 68 or 69
//              29 RENOTE  <seq> <owner> <route> <src> <chan> <slot>    re-aim an owned route; answer 68 or 69
//              2A UNNOTE  <owner> <route | 7F every route of owner>    no answer
//              2B DEVICES <seq>                                        answer 6A
//   Pi -> box  68 NOTED   <seq> <owner> <route>
//              69 REFUSED <seq> <owner> <code>      3 full, 5 bad (a field out of range, not the owner, no such route)
//              6A DEVICES <seq | 7F> <n> {<src 0..3> <len> <name, up to 24>} x n   the USB-A devices present; also
//                         unasked (seq 7F) to every box after a plug or unplug
//   6B is kept free for the service.
//
// Wiring (after S2, which reworks kernel / bridge / cores / ob_usb / core/device; not done in this step):
//   1. set_sink(): to the box's processor slot (the per-box core/proc S2 brings), replacing devmidi's step 2 (every
//      USB-A message to every filled slot): a slot gets keyboard notes only through a note route.
//   2. feed(): devmidi::on_midi(listener -> feed(src, -1, ...)) for the USB-A devices (its unplug note-offs come the
//      same way), and the USB-C gadget's per-cable MIDI OUT path (S8c) with the cable's box.
//   3. devmidi plug / unplug -> plugged() (6A to each box) and gone(); a USB-C port gone -> gone(kUsbC, box).
//   4. kv/link: is_op(m[6]) -> sysex(box, m, n) beside route::sysex; host_step drains take_answer(box) per box; the
//      box's store HELLO and link reset -> reset(box). core/dsp's FREE -> slot_freed(box, slot).
//
// Threads: everything on the control side (the Pi's core 3), one producer; the sink is called from feed / the
// releases on that side (core/proc queues to the render side). Portable (Pi, Mac core, Linux tests).
#pragma once
#include <stdint.h>

namespace core {
namespace noteroute {

constexpr int kBoxes = 2;        // the Pi's box slots
constexpr int kRoutes = 8;       // per box
constexpr int kSlots = 16;       // a box's processor slots (core/proc kSlots)
constexpr int kUsbADevices = 4;  // core/devmidi kSrcs
constexpr int kNameLen = 24;     // core/devmidi kNameLen
constexpr uint8_t kUsbAAny = 0x0F, kUsbC = 0x10, kAnyChan = 0x7F, kEvery = 0x7F, kUnasked = 0x7F;
enum Op : uint8_t { kNotes = 0x28, kRenote = 0x29, kUnnote = 0x2A, kDevices = 0x2B,
                    kNoted = 0x68, kRefused = 0x69, kDeviceList = 0x6A };
enum Code : uint8_t { kFull = 3, kBad = 5 };

// box, its processor slot, one channel voice message
using SinkFn = void (*)(int box, int slot, uint8_t status, uint8_t d1, uint8_t d2);
using NameFn = const char* (*)(int src);   // a USB-A device's name, "" or null when none (devmidi::name)

void set_sink(SinkFn f);
void set_names(NameFn f);

bool is_op(uint8_t op);
// a whole 51 message with a note-route op from box `box`; false: malformed, refused or not ours (answers queued
// for NOTES / RENOTE / DEVICES either way)
bool sysex(int box, const uint8_t* m, int n);
// box's next queued answer, whole F0..F7; 0: none
int take_answer(int box, uint8_t* out, int cap);

// one message from a source: src 0..3 a USB-A device (box ignored), kUsbC the USB-C port of `box`
void feed(int src, int box, uint8_t status, uint8_t d1, uint8_t d2);
void gone(int src, int box);        // the source left: its held notes released on every route
void plugged();                     // a USB-A device came or went: 6A (unasked) queued for every box
void reset(int box);                // the box's link reset / store HELLO: its routes released and closed
void slot_freed(int box, int slot); // the processor went: held notes into it forgotten, routes kept

// tests / /status / console
struct Route { bool open; uint8_t owner, src, chan, slot; int held; };   // held: notes held now
Route route(int box, int r);
struct Stats { uint32_t opened, moved, closed, refused, delivered, released, ops_bad; int open; int last_refused; };
Stats stats(int box);
void clear_all();                   // every box's routes closed without note-offs, answers and stats dropped (tests)

}  // namespace noteroute
}  // namespace core

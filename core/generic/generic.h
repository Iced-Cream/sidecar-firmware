// core/generic/generic.h - the stripped engine's core (2026-11-04, docs/pi-engine.md "Generic engine"): the Rytm's
// host-link SysEx routed to the generic services and nothing else. What it keeps of the engine core:
//   4F        the device hello (core/device: the Rytm's profile; its state folder)
//   10        the project's name: the identity storage keys on (project(), on_project)
//   35        the Rytm's hello: the link reset (on_link) and the remote screen (core/screen)
//   2C / 2D   the Rytm's step clock (core/clock)
//   30..3F    the remote screen's transport (core/screen), no page of its own
//   51..53    reserved for STORE (the Storage phase)
// Every other command is counted and dropped: the machines, the send FX, the sequencer, the pools and the rest of the
// mods' protocol have no Pi side in this engine (their code is in the tree, unwired). The card: core/fmt's atomic writes
// and the boot sweep, core/device's state folder, the card queue (core/msamp/cardq) the platform runs.
//
// Threads: begin() once at boot; sysex(), tick() and the idle steps on the control side (the Pi's core 3). Portable.
#pragma once
#include <stdint.h>

namespace core {
namespace gen {

bool begin();                            // the card's state (false: no card)
void sysex(const uint8_t* d, int len);   // a whole F0 .. F7 from the Rytm (or another box)
void tick(uint32_t ms);                  // every pass of the control loop
bool idle_save();                        // one step of the card's writes (true: it did work)

const char* project();                   // the Rytm's project name ("" none yet)
using ProjectFn = void (*)(const char* name);
using LinkFn = void (*)(const char* why);
bool on_project(ProjectFn f);            // listeners (4 each); false: no room
bool on_link(LinkFn f);

constexpr uint8_t kStoreFirst = 0x51, kStoreLast = 0x53;
using StoreFn = bool (*)(const uint8_t* d, int len);
void set_store(StoreFn f);               // the Storage phase's 51..53 handler (null: dropped)

struct Stats { uint32_t messages, dropped, projects, hellos; uint8_t last_dropped; };
Stats stats();
}  // namespace gen
}  // namespace core

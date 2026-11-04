// core/generic/generic.h - the stripped engine's core (2026-11-04, docs/pi-engine.md "Generic engine"): the Rytm's
// host-link SysEx routed to the generic services and nothing else. What it keeps of the engine core:
//   4F        the device hello (core/device: the Rytm's profile; its state folder)
//   0D        a folder of the card for the Rytm's file browser (core/browse: 0E answers; rytm-mods 0023)
//   35        the Rytm's hello: the link reset (on_link) and the remote screen (core/screen)
//   2C / 2D   the Rytm's step clock (core/clock)
//   30..3F    the remote screen's transport (core/screen), no page of its own
//   51..53    STORE (core/kv: records on the card, the Pi-side locks, 04 NAME the project's name and ID)
// Every other command is counted and dropped: no Rytm mod has a Pi side of its own here (the old engine's machines,
// send FX, sequencer and pools are in git history only). The card: core/fmt's atomic writes and the boot sweep,
// core/device's state folder, the card queue (core/msamp/cardq) the platform runs.
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

using LinkFn = void (*)(const char* why);
bool on_link(LinkFn f);                  // the link reset's listeners (4); false: no room

constexpr uint8_t kStoreFirst = 0x51, kStoreLast = 0x53;
using StoreFn = bool (*)(const uint8_t* d, int len);
void set_store(StoreFn f);               // 51..53's handler (core/kv's attach_generic; null: dropped)

struct Stats { uint32_t messages, dropped, hellos; uint8_t last_dropped; };
Stats stats();
}  // namespace gen
}  // namespace core

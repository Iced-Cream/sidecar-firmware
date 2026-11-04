// pi/engine/bridge.h - the engine core as plain functions for the kernel's Circle-compiled files (kernel, cores,
// guard, console): bridge.cpp is compiled hosted, with the core, onto core/generic, core/screen, core/clock, core/proc,
// core/kv, core/browse and core/devmidi (docs/pi-engine.md "Generic engine").
#pragma once
#include <stdint.h>

namespace bridge {
bool begin(void (*to_host)(const uint8_t* m, int n));   // core::hooks.to_rytm = to_host; the card's state (false: no card)
void sysex(const uint8_t* d, int len);                  // a whole F0 .. F7 from the Rytm (core::gen::sysex)
bool idle_save();                                       // one step of the card's writes and the answers owed (true: work)
bool take_restart();                       // another box needs its state folder (core/device/device.h): reboot
void tick();                               // the control core's loop, every pass: the queues below, then core::gen::tick

// Popups on the Rytm's own screen (core/screen): dropped when the Rytm cannot take one. rui_popup and screen_address are
// core 0's (the stats tick): they queue, and tick() hands them to the screen, which is the control core's alone.
// rui_popups counts the status popups shown.
void rui_popup(const char* line1, const char* line2);
uint32_t rui_popups();
void screen_address(const char* ip);       // the network's address for the connect splash
// The pairing code on the Rytm's screen (pi/engine/paircode.h, paircode_pi.h): a 30 s popup, over an open page too
bool pair_popup(const char* line);         // FALSE: the Rytm would not take it (paircode_pi::ShowFailed)
void pair_popup_end();                     // a device paired: take it off now
uint32_t rytm_hellos();                    // the Rytm's hellos so far (a new one: paircode_pi::RytmHello)
bool rytm_greeted();
void rytm_gone();                          // the Rytm's USB MIDI removed: the link reset
void rytm_resync();                        // records for the Rytm were lost: the screen sends what it owes again

// The card queue (core/msamp/cardq.h): card_worker(true) at boot (cmdline cardq=1, the default): core 0's loop runs the
// card requests (card_service, at most n a call: the count run; 51 STORE's ops and the card browse's reads too) while
// card_busy() holds uploads off. Off: the control core runs them itself.
void card_worker(bool on);
int card_service(int n);
bool card_busy();

// The USB-A MIDI devices (core/devmidi.h), from the control core: a plug / unplug, a message, the way to the Rytm
void devmidi_plugged(int src, bool on, const char* name);
void devmidi(int src, uint8_t st, uint8_t d1, uint8_t d2);
void devmidi_set_forward(void (*f)(uint8_t, uint8_t, uint8_t));

// "store":{files,bytes,ready,unwritten,oldestMs,held,heapFree} (docs/pi-engine.md); heap_free: the kernel's
// CMemorySystem::GetHeapFreeSpace(HEAP_LOW)
int store_json(char* out, int cap, unsigned long heap_free);
const char* project();                     // the Rytm's project name ("" none yet)
uint32_t to_host_dropped();
// The guard's fault and overrun lines name a voice's machine: none renders in this engine ("-", 0)
const char* machine_name(int voice);
uint32_t machine_gen(int voice);
// /status's generic members ("generic", "clock", "proc", "screen", "devmidi", "browse"); the console's gen, clock,
// proc, rui, rui=on|off, usba, usba=thru|off, browse (the control core; false: not one of them)
int generic_json(char* out, int cap);
bool generic_command(const char* p, char* out, int cap);
}  // namespace bridge

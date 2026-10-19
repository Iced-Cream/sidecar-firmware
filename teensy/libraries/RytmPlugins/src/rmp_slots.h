// rmp_slots.h - user slots U1..U5: which machine each plays in the current
// project (/.rytm/slots/<PROJECT>.txt on the SD card), loading the machines a
// project uses into the arena, and per-voice state. Portable: the Mac tests it.
#pragma once
#include <stdint.h>
#include "rmp_loader.h"
#define RMP_SLOTS 5
#define RMP_VOICES 8
struct rmp_slot_entry { char id[32]; uint32_t version; };   // id[0] == 0: slot empty; version a << 16 | b << 8 | c
struct rmp_slot_map { rmp_slot_entry slot[RMP_SLOTS]; };
// U1..U5 voice flags (voice byte & 0x78): 0x50 + 8 n.
inline int rmp_slot_of_flag(uint8_t f) { return (f >= 0x50 && f <= 0x70 && !(f & 7)) ? (f - 0x50) >> 3 : -1; }

// The slot map file: one line per used slot, "U<n> <id> <a.b.c>", fields separated by
// spaces/tabs, n in 1..5, id passing rmp_id_ok (at most 31 printable ASCII chars, no
// NUL), a, b, c each 1..3 digits, <= 255, no leading zero except "0" itself (so a
// parse + rmp_slots_format round trip is exact). Blank (whitespace-only) lines are skipped; a trailing '\r'
// (CRLF files) and a last line without '\n' are fine. Anything else - a stray line, a
// slot out of range or named twice, an unsafe id, a missing/extra/too large version
// part - makes the whole parse fail: false, and *m is left all zero (no slot used).
bool rmp_slots_parse(const char *txt, uint32_t len, rmp_slot_map *m);
// The map as that file, slots in order. NUL-terminated; returns the length, or 0
// when it doesn't fit in cap (out is then ""). An empty map also gives 0 and "".
uint32_t rmp_slots_format(const rmp_slot_map *m, char *out, uint32_t cap);

// Runtime: the platform functions the slots use. On the Teensy size_of/read_at are
// rmp_store_size/rmp_store_read_at (streaming: the package is never in RAM whole;
// voiceloop's heap is small) and the arena functions are rmp_arena_*.
struct rmp_host;
struct rmp_platform {
  bool (*size_of)(const char *id, uint32_t *len);                              // installed package's size; false = not installed
  bool (*read_at)(const char *id, uint32_t off, uint8_t *buf, uint32_t n);     // n bytes at package offset off; false on any failure
  uint8_t *(*arena_alloc)(uint32_t bytes);                                     // nullptr when full
  void (*arena_reset)();
  void (*arena_sync)(void *p, uint32_t bytes);                                 // after writing code (cache clean/invalidate)
  void (*log)(const char *msg);                                                // may be null
  const rmp_host *host;                                                        // for the caller's init() (voiceloop calls it through the guard); unused here
};
struct rmp_plugin;
enum rmp_slot_state { RMP_SLOT_EMPTY = 0, RMP_SLOT_READY, RMP_SLOT_MISSING, RMP_SLOT_BAD, RMP_SLOT_NO_ROOM, RMP_SLOT_MUTED, RMP_SLOT_LOADING };   // LOADING: an incremental load is in progress (plays nothing)
struct rmp_loaded { rmp_slot_state state; const rmp_plugin *plugin; uint32_t version; uint32_t from;
                    uint32_t need; };   // version: the installed one; from: its compatible_from; need: NO_ROOM, the bytes it asked the arena for
// Load what the map names into a freshly reset arena; out[s] gets each slot's state:
//   EMPTY    no id in the slot
//   MISSING  not installed, unsafe id, or the slot's version is outside the installed
//            package's [compatible_from, version]
//   BAD      unreadable, header/CRC wrong, header id != slot id, relocation error, or
//            the entry table's magic/api wrong (the arena space it took is not reclaimed)
//   NO_ROOM  the arena is full
//   READY    plugin points at the entry table in the arena
// Two slots naming the same id share one image (same plugin pointer); each slot's
// version is still checked against that package on its own. A package that fails as a
// whole (BAD, NO_ROOM) is tried once: later slots naming the same id get the same state
// without a reload (no repeated reads, no arena space lost twice); a version-range
// MISSING is per slot. The package is CRC'd in
// 512-byte stack chunks before anything is allocated, then placed with
// rmp_place_stream. init() is NOT called: the caller does that through the guard
// (voiceloop, through the guard); a READY slot is loaded, not yet initialised. Main loop only.
// The store must not change during the call: each package is read twice (the CRC
// pass, then the place pass), so an install in between could place bytes that were
// never CRC'd. Safe as long as installs and loads both run on the main loop
// (rmp_store's rule), never one from an ISR.
void rmp_slots_load(const rmp_platform *pf, const rmp_slot_map *m, rmp_loaded out[RMP_SLOTS]);

// ---- Incremental load: one slot, appended to the arena (no reset) ----
// For a map change of ONE slot while the others play. The other slots keep their images
// (out[] untouched but out[slot]); the replaced slot's old image stays allocated in the
// arena until the next full rmp_slots_load (a project change), so repeated assigns eat
// arena space: when rmp_slots_load_one / the last step leaves out[slot].state ==
// RMP_SLOT_NO_ROOM, the caller falls back to a full rmp_slots_load.
// If another slot of out[] is READY with the same id, its image is shared (the version
// is checked against that package's range; no read, no arena) - and then init() has
// already run for it. Otherwise the package is loaded as in rmp_slots_load.
// Stepped form, for spreading a load over loop passes (each step: the header, or up to
// max_bytes of the CRC pass, or the placement - see below):
//   rmp_load_job j;
//   if (!rmp_slots_one_begin(&j, pf, m, slot, out))      // false: a load is under way,
//     while (!rmp_slots_one_step(&j, out, 8192)) { ...other work... }
// A step on a finished job (or one whose begin returned true) does nothing and returns true.
// out[slot].state is RMP_SLOT_LOADING meanwhile (the voices stay silent) and the final
// state afterwards. Passes: 1 header (opens the file: ~3 ms on the Teensy's flash store),
// n CRC passes (max_bytes each), 1 placement (reads the image + relocations, applies them).
struct rmp_load_job {
  const rmp_platform *pf; int slot; char id[32]; uint32_t want;
  int phase; uint32_t len, off, crc; rmp_info info;
  rmp_slot_state state; const rmp_plugin *plugin; bool pkg_fail; uint32_t need;
};
bool rmp_slots_one_begin(rmp_load_job *j, const rmp_platform *pf, const rmp_slot_map *m, int slot, rmp_loaded out[RMP_SLOTS]);
bool rmp_slots_one_step(rmp_load_job *j, rmp_loaded out[RMP_SLOTS], uint32_t max_bytes);
void rmp_slots_load_one(const rmp_platform *pf, const rmp_slot_map *m, int slot, rmp_loaded out[RMP_SLOTS]);

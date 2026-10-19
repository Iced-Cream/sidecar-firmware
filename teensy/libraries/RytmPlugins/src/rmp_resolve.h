// rmp_resolve.h - which machine each user slot U1..U5 plays when a project loads: the slot list behaves as one
// instrument across projects, and a project shared between Sidecars finds its machines by the sounds' fingerprints.
// Portable (the Teensy's voiceloop, the Pi's core/user/um.cpp, the Mac's tests: extras/resolve_test.cpp).
// docs/user-machines-pi.md, "Which machine a slot plays".
//
// Precedence, per slot, when a project loads:
//   1. explicit empty (the user cleared the slot, or "start this project empty"): stays empty. Nothing below
//      fills it; the sounds' fingerprint is still noted for the UI.
//   2. fingerprints: the fingerprints the project's sounds use on the slot (0x25 SLOT PRINTS, or lazily a trig's
//      tag). The slot's assignment stands if any of its sounds uses it. Otherwise the most used fingerprint that
//      names exactly one installed package wins (count, then lower fp); several packages with one fingerprint (a
//      collision) take the one whose id the slot's note names (an imported list, an earlier resolve), else none:
//      flagged COLLISION. A fingerprint no package has: flagged UNKNOWN (the UI asks for a machine with it).
//      A saved or imported assignment the sounds replace is flagged UPDATED (logged, shown "updated from the
//      project's sounds"). An inherited assignment the sounds do not use is dropped even when nothing resolves.
//   3. the project's own map (saved, or set by an import): wins for a slot with no fingerprint information and
//      is never replaced otherwise.
//   4. inherit: a project with no map file and no note file at all gets the previous project's assignment for
//      each slot with no fingerprint, copied and saved, so it becomes the project's own.
//
// The note file /.rytm/slots/<PROJECT>.inf sits next to the map (/.rytm/slots/<PROJECT>.txt, rmp_slots_parse's
// format, unchanged so an older build still reads the map). One line per slot that has anything to note:
//   U<n> <src> <flags> <fp> <id or -> <a.b.c or -> [<name>]
// src one of none saved sounds inherited empty imported; flags a decimal bit set (RMP_RF_*); fp 0..16383 (the
// sounds' or the import's); id/version the wanted package (an import's, or the last resolve's); name up to 10
// characters, the rest of the line (may hold spaces). Unknown or bad lines are skipped (advisory file).
#pragma once
#include <stdint.h>
#include "rmp_slots.h"

enum rmp_src { RMP_SRC_NONE = 0, RMP_SRC_SAVED, RMP_SRC_SOUNDS, RMP_SRC_INHERITED, RMP_SRC_EMPTY, RMP_SRC_IMPORTED };
#define RMP_RF_COLLISION 1u   // the sounds' fingerprint names several installed packages, none known by id
#define RMP_RF_UNKNOWN   2u   // the sounds' fingerprint names no installed package
#define RMP_RF_UPDATED   4u   // the last resolve replaced a saved / imported / inherited assignment (the sounds won)
#define RMP_RF_MIXED     8u   // the slot's sounds use more than one fingerprint

struct rmp_slot_note {
  uint8_t src, flags;
  uint16_t fp;          // the sounds' top fingerprint (0: no information), or an import's
  uint16_t nfp;         // how many of the project's sounds use it (0: lazily from a trig, or an import)
  char id[32];          // the wanted package, if known (import, last resolve); "" none
  uint32_t version;
  char name[11];        // its name, if known
};
struct rmp_slot_notes { rmp_slot_note slot[RMP_SLOTS]; };

const char *rmp_src_name(int src);
bool rmp_notes_parse(const char *txt, uint32_t len, rmp_slot_notes *n);       // false: nothing usable (n zeroed)
uint32_t rmp_notes_format(const rmp_slot_notes *n, char *out, uint32_t cap);  // NUL-terminated length, 0 if no fit

// An installed package as the resolver sees it (the board's list: ok / loadable packages only).
struct rmp_pkg_ref { const char *id; uint32_t version; uint16_t fp; const char *name; };
// A 0x25 entry: slot 0..4, fp 1..16383, count.
struct rmp_print { uint8_t slot; uint16_t fp; uint8_t count; };
struct rmp_prints { uint8_t seq, flags, n; rmp_print e[16]; };
#define RMP_CMD_SLOT_PRINTS 0x25
// F0 7D 52 54 25 01 <seq> <flags> <n> {<slot 1..5> <fp hi> <fp lo> <count>} F7 (sim/protocol.py slot_prints).
bool rmp_parse_prints(const uint8_t *m, int len, rmp_prints *p);
// The same content (for "is this report new"): a hash of the entries, flags and n (not seq).
uint32_t rmp_prints_hash(const rmp_prints *p);

// After the project's files were read: have_map / have_notes whether each file existed. Normalises the notes
// (a map without notes: its slots saved; a note's src without an id in the map: none, explicit empty kept).
// True when the project has neither file (fresh: inherit applies, rmp_resolve_inherit).
bool rmp_resolve_open(rmp_slot_map *m, rmp_slot_notes *n, bool have_map, bool have_notes);
// Step 4: slots with no assignment, not explicitly empty and with no fingerprint information take prev's.
// Returns the slots changed (bit s).
uint32_t rmp_resolve_inherit(rmp_slot_map *m, rmp_slot_notes *n, const rmp_slot_map *prev);
// Step 2 with a whole report (0x25), for the slots in mask (bit s; RMP_ALL_SLOTS: a new report; after an install, the
// slots flagged UNKNOWN / COLLISION again). Returns the slots whose assignment changed; their notes are updated.
#define RMP_ALL_SLOTS ((1u << RMP_SLOTS) - 1u)
uint32_t rmp_resolve_prints(rmp_slot_map *m, rmp_slot_notes *n, const rmp_prints *p, const rmp_pkg_ref *pk, int npk,
                            uint32_t mask = RMP_ALL_SLOTS);
// The slots flagged UNKNOWN or COLLISION (worth resolving again when the installed packages change).
uint32_t rmp_resolve_open_slots(const rmp_slot_notes *n);
// Step 2 lazily (an older Rytm: no 0x25): a trig on slot s (0..4) with sound tag fp (nonzero). Fills only a slot
// with no assignment (not explicitly empty) or an inherited one the tag does not match. True when it changed.
bool rmp_resolve_tag(rmp_slot_map *m, rmp_slot_notes *n, int s, uint16_t fp, const rmp_pkg_ref *pk, int npk);
// The user's acts (the web, the console, a 1E): set slot s to id / version (id "" or null: explicit empty).
void rmp_resolve_set(rmp_slot_map *m, rmp_slot_notes *n, int s, const char *id, uint32_t version, int src);
// An imported entry: id / version as given (installed or not: then MISSING until it is), fp and name noted.
void rmp_resolve_import(rmp_slot_map *m, rmp_slot_notes *n, int s, const char *id, uint32_t version, uint16_t fp,
                        const char *name);

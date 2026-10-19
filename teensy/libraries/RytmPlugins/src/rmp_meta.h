// rmp_meta.h - generations of the user slots and of the installed list (the spec's 2.1): a slot's
// generation (1..127, then 1 again; 0 is the Rytm's "knows nothing") is bumped whenever what 1A says
// for it changes - a fingerprint of its protocol state, flags, id, version and description - and the
// store's whenever the installed list changes. Either makes a 1D due. Portable.
#pragma once
#include <stdint.h>
#include "rmp_manifest.h"
#define RMP_META_SLOTS 5
struct rmp_meta {
  uint8_t gen[RMP_META_SLOTS];       // the slots' generations
  uint32_t print[RMP_META_SLOTS];    // the fingerprint each generation was given for
  uint8_t store_gen;                 // the installed list's generation
  bool dirty;                        // a 1D is due
};
// Every slot EMPTY, a 1D due. seed 1 (the default, the Mac tests): every generation 1. Any other seed (the Teensy:
// its TRNG, else the cycle counter, at boot) gives each slot and the store a generation 1..127 derived from it, so
// a rebooted Teensy rarely restarts at a value a powered Rytm still holds for other content (spec 2.1).
void rmp_meta_init(rmp_meta *m, uint32_t seed = 1);
// FNV-1a of what 1A says: the protocol state (RMP_PST_*), flags, id (up to its NUL, 32 at most),
// version and, when d is not null, the description's names, page 2, title and knobs.
uint32_t rmp_meta_print(int pstate, int flags, const char *id, uint32_t version, const rmp_desc *d);
// Slot s (0..4) now says print: a new fingerprint bumps its generation and makes a 1D due (true).
bool rmp_meta_set(rmp_meta *m, int s, uint32_t print);
// Slot s (0..4) gets a new generation without a new fingerprint (a 1E answered while the slot is not
// fingerprinted yet, voiceloop plug_reply). Its print is invalidated (the old one inverted), so the next
// rmp_meta_set bumps it again even with the old print: a 1A sent at the bumped generation while the slot was
// unsettled (no presets flag yet, say) is always followed by a 1D that makes the Rytm ask again.
void rmp_meta_bump(rmp_meta *m, int s);
void rmp_meta_store(rmp_meta *m);    // the installed list changed
bool rmp_meta_take(rmp_meta *m);     // true once per due 1D (clears it)

// rmp_state.h - per-voice plugin state, outside the code arena: a pool the caller hands
// over with rmp_state_set_pool (before rmp_state_begin; none by default), then PSRAM when fitted. Bump allocation; rmp_state_reset() at a project change.
#pragma once
#include <stdint.h>
// Call rmp_state_begin() once at startup, after the PSRAM sample pool took its share
// (it resets the pools and takes the PSRAM block); rmp_state_reset() at a project change.
void rmp_state_begin();
void *rmp_state_alloc(uint32_t bytes);     // 8-aligned, zeroed; nullptr when neither pool has room
void rmp_state_reset();
uint32_t rmp_state_used_ocram();
uint32_t rmp_state_used_psram();
// A fast pool in the caller's RAM (e.g. rmp_test's 24 KB DMAMEM array); call before
// rmp_state_begin. Without it only PSRAM serves allocations.
void rmp_state_set_pool(void *p, uint32_t bytes);   // resets the pool; one smaller than its 8-byte alignment slack is none
uint32_t rmp_state_capacity();             // bytes the pools can serve in total (0: nothing, no PSRAM)

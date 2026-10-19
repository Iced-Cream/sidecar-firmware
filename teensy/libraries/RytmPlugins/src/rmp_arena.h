// rmp_arena.h - the executable RAM areas for plugin images (code + data + bss): 32 KB in DTCM, and one in PSRAM when fitted.
// MPU regions RMP_ARENA_REGION(2) make it readable, writable and executable
// (the Teensy core marks all RAM no-execute, in its MPU setup).
#pragma once
#include <stdint.h>
#ifndef RMP_ARENA_BYTES
#define RMP_ARENA_BYTES 32768u      /* whole 8 KB subregions, from docs/plugins.md */
#endif
#define RMP_ARENA_ALIGN 8192u        /* an MPU subregion: a 64 KB region has 8 */
#define RMP_ARENA_REGION 13          /* above the core's regions 0..10 (higher numbers win on overlap) */
#define RMP_ARENA_REGION2 14         /* the second 64 KB window when the arena crosses a 64 KB line */
void rmp_arena_begin();              // set up the MPU region once (setup())
uint8_t *rmp_arena_base();
// Allocation is a simple bump with whole reset: plugins are loaded together at
// a project change (rmp_slots), so a reset + reload is the only release needed.
uint8_t *rmp_arena_alloc(uint32_t bytes);   // 8-aligned, nullptr when full
void rmp_arena_reset();                     // both arenas
uint32_t rmp_arena_used();                  // the internal arena's (as before the PSRAM arena)
// ---- the second arena, in PSRAM (only with PSRAM fitted; rmp_arenas.h has the placement rule) ----
// rmp_arena_alloc takes the internal arena first, then this one. Its size is a power of two (bytes rounded
// up, 64 KB..8 MB); begin returns it, 0 without PSRAM (or no room in the PSRAM heap: then nothing changes).
#ifndef RMP_PSRAM_ARENA_BYTES
#define RMP_PSRAM_ARENA_BYTES (1024u * 1024u)   /* + 1/8 for the MPU alignment, from extmem_malloc */
#endif
#define RMP_PSRAM_REGION 11          /* above the core's FlexSPI2 region (9, XN); 11, 12 unused by the core */
#define RMP_PSRAM_REGION2 12
uint32_t rmp_arena_psram_begin(uint32_t bytes);   // setup(), after the sample pool and rmp_state_begin
uint32_t rmp_arena_psram_size();            // 0: no PSRAM arena
uint32_t rmp_arena_psram_used();
uint8_t *rmp_arena_psram_base();
uint8_t *rmp_arena_alloc_in(int where, uint32_t bytes);   // RMP_PLACE_INTERNAL / RMP_PLACE_PSRAM only
int rmp_arena_where(const void *p);         // RMP_PLACE_INTERNAL, RMP_PLACE_PSRAM, RMP_PLACE_NONE
// Both bump marks, and back to them (the bench's temporary copies; never below a later load)
void rmp_arena_mark(uint32_t *used_internal, uint32_t *used_psram);
void rmp_arena_release(uint32_t used_internal, uint32_t used_psram);
void rmp_arena_psram_mpu(uint32_t *rbar, uint32_t *rasr, uint32_t *rbar2, uint32_t *rasr2);   // regions 11, 12 readback
// After writing code into the arena: clean the D-cache, invalidate the I-cache.
void rmp_arena_sync(void *p, uint32_t bytes);
// Raw MPU readback of the arena's two regions (13, 14; region 14's RASR 0 when one
// window suffices), for tests.
void rmp_arena_mpu_readback(uint32_t *ctrl_before, uint32_t *ctrl_after, uint32_t *rbar, uint32_t *rasr,
                            uint32_t *rbar2, uint32_t *rasr2);

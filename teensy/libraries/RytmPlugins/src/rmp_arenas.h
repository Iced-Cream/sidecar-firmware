// rmp_arenas.h - the plugin code arenas' bookkeeping, portable (the Mac tests it: extras/arenas_test.cpp).
// Two bump arenas: the internal one (32 KB in DTCM, rmp_arena.cpp) and, on a Teensy with PSRAM fitted,
// a second one in PSRAM (FlexSPI2, executed through the caches). Placement: internal first (fastest),
// PSRAM when the internal one has no room; nullptr (NO_ROOM) only when both are full. Each arena is a
// bump with whole reset (rmp_arenas_reset at a full reload, as before): no fragmentation to manage.
// Without a PSRAM arena (size 0) every call behaves exactly as the single internal arena did.
#pragma once
#include <stdint.h>
enum { RMP_PLACE_NONE = -1, RMP_PLACE_INTERNAL = 0, RMP_PLACE_PSRAM = 1 };
struct rmp_bump { uint8_t *base; uint32_t size, used; };
struct rmp_arenas { rmp_bump a[2]; };   // [RMP_PLACE_INTERNAL], [RMP_PLACE_PSRAM] (size 0: none)
uint8_t *rmp_bump_alloc(rmp_bump *b, uint32_t bytes);                    // 8-aligned, nullptr when full
uint8_t *rmp_arenas_alloc(rmp_arenas *r, uint32_t bytes);                // internal, else PSRAM
uint8_t *rmp_arenas_alloc_in(rmp_arenas *r, int where, uint32_t bytes);  // that arena only
void rmp_arenas_reset(rmp_arenas *r);
int rmp_arenas_where(const rmp_arenas *r, const void *p);                // RMP_PLACE_*, NONE outside both
const char *rmp_place_name(int where);                                   // "internal", "psram", "-"

// MPU subregion disable mask for a window of 8 subregions (each sub bytes, window at w) so that only the
// subregions overlapping [a, e) are enabled (bit k set = subregion k disabled; 0xFF = window not needed).
uint32_t rmp_mpu_srd(uint32_t w, uint32_t sub, uint32_t a, uint32_t e);

// Guard limits for a call into a plugin whose image is in arena `where`. base: today's limit (the render's
// 2 x declared + 2000 cycles, or the fixed per-call limits). Internal: base, unchanged. PSRAM: the budget
// (what the overrun count compares the measured cycles to) is base x RMP_PSRAM_LIMIT_X; the hard limit
// (the guard's timeout) adds RMP_PSRAM_COLD_CYCLES on top, for a call that runs on cold caches (each 32-byte
// line from PSRAM ~480 cycles: 300000 cycles ~ 600 lines ~ 19 KB of code and data touched cold).
#ifndef RMP_PSRAM_LIMIT_X
#define RMP_PSRAM_LIMIT_X 3u
#endif
#ifndef RMP_PSRAM_COLD_CYCLES
#define RMP_PSRAM_COLD_CYCLES 300000u
#endif
inline uint32_t rmp_limit_budget(uint32_t base, int where) {
  if (where != RMP_PLACE_PSRAM || !base) return base;
  uint64_t b = (uint64_t)base * RMP_PSRAM_LIMIT_X;
  return b > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)b;
}
inline uint32_t rmp_limit_hard(uint32_t base, int where) {
  if (where != RMP_PLACE_PSRAM || !base) return base;
  uint64_t b = (uint64_t)rmp_limit_budget(base, where) + RMP_PSRAM_COLD_CYCLES;
  return b > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)b;
}

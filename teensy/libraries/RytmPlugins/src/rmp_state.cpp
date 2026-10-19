// rmp_state.cpp
#include "rmp_state.h"
#include <Arduino.h>
#include <string.h>
// The fast pool is the caller's memory (rmp_state_set_pool): voiceloop gives none (the state
// lives in each voice's own engine memory; PSRAM, when fitted, serves what does not fit).
static uint8_t *ocram;
static uint32_t cap_o, used_o, used_p, cap_p;
// Resets the fast pool (call it before any allocation): a pool smaller than its alignment slack is none.
void rmp_state_set_pool(void *p, uint32_t bytes) {
  used_o = 0;
  uint32_t slack = p ? (uint32_t)((8u - ((uintptr_t)p & 7u)) & 7u) : 0;
  if (!p || bytes <= slack) { ocram = nullptr; cap_o = 0; return; }
  ocram = (uint8_t *)p + slack;
  cap_o = bytes - slack;
}
uint32_t rmp_state_capacity() { return cap_o + cap_p; }
static uint8_t *psram;
#ifndef RMP_STATE_PSRAM_BYTES
#define RMP_STATE_PSRAM_BYTES (2u * 1024u * 1024u)
#endif
extern "C" uint8_t external_psram_size;   // MB, found by the core at boot
FLASHMEM void rmp_state_begin() {
  used_o = used_p = 0;
  if (external_psram_size && !psram) {
    uint8_t *m = (uint8_t *)extmem_malloc(RMP_STATE_PSRAM_BYTES);
    // extmem_malloc falls back to the heap when PSRAM cannot take it: only 0x7000_0000.. will do
    if (m && ((uint32_t)m >> 28) == 7) { psram = m; cap_p = RMP_STATE_PSRAM_BYTES; }
    else if (m) extmem_free(m);
  }
}
void *rmp_state_alloc(uint32_t bytes) {
  if (bytes > 0xFFFFFFF8u) return nullptr;
  static uint64_t zero_len;   // a zero-byte request is a valid, non-null block (nothing is stored in it)
  if (bytes == 0) return &zero_len;
  bytes = (bytes + 7u) & ~7u;
  if (bytes <= cap_o - used_o) { void *p = ocram + used_o; used_o += bytes; memset(p, 0, bytes); return p; }
  if (psram && bytes <= cap_p - used_p) { void *p = psram + used_p; used_p += bytes; memset(p, 0, bytes); return p; }
  return nullptr;
}
void rmp_state_reset() { used_o = used_p = 0; }
uint32_t rmp_state_used_ocram() { return used_o; }
uint32_t rmp_state_used_psram() { return used_p; }

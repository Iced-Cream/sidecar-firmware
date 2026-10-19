// tools/tests/test_rmp_api.c - the ABI's layout on the Mac matches the Teensy's
// 32-bit layout where it matters (offsets of the leading fields), and API 2's additions.
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "../../sdk/rmp/rmp_api.h"
static int32_t le(void *st, const uint8_t *b, int32_t n, int16_t v[16]) { (void)st; (void)b; (void)n; (void)v; return 0; }
static void ve(void *st) { (void)st; }
int main(void) {
  assert(offsetof(rmp_plugin, magic) == 0);
  assert(offsetof(rmp_plugin, api) == 4);
  assert(offsetof(rmp_plugin, state_bytes) == 8);
  assert(offsetof(rmp_plugin, cpu_permille) == 12);
  assert(offsetof(rmp_plugin, init) == 16);
  assert(offsetof(rmp_host, api) == 0);
  assert(RMP_API == 2u && RMP_API_MIN == 1u);
  assert(RMP_P_PARSE == 0 && RMP_P_LOAD_ENTRY == 1 && RMP_P_VOICE_ENTER == 2 && RMP_P_VOICE_LEAVE == 3);
  assert(RMP_H_POOL_INFO == 0 && RMP_H_POOL_READ == 1);
  assert(sizeof(rmp_pool_info) == 20 && offsetof(rmp_pool_info, head_frames) == 16);
  /* the accessors: a v1 table has no v2 functions, a v2 table the ones it fills */
  rmp_plugin p = {RMP_PLUGIN_MAGIC, 1u, 0, 0, 0, 0, 0, 0, 0, 0, {0, RMP_FN(le), RMP_FN(ve), RMP_FN(ve), 0, 0, 0, 0}};
  assert(rmp_load_entry_of(&p) == 0 && rmp_voice_enter_of(&p) == 0 && rmp_voice_leave_of(&p) == 0);
  p.api = 2u;
  assert(rmp_load_entry_of(&p) == le && rmp_voice_enter_of(&p) == ve && rmp_voice_leave_of(&p) == ve);
  rmp_host h = {1u, 48000.0f, 12, 0, 0, {0}};
  assert(rmp_pool_info_of(&h) == 0 && rmp_pool_read_of(&h) == 0);
  printf("ok\n");
  return 0;
}

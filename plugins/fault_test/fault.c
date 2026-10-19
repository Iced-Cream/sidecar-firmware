// plugins/fault_test/fault.c - a machine that misbehaves on demand (P1 zones):
// 0 plays silence, 1 writes through a null pointer, 2 loops forever, 3 a long
// render (~240k cycles measured: far over its declared CPU, but under rmp_test's
// 600k-cycle limit, so it returns OK there; a lower limit cuts it). For the
// guard's tests only.
#include "rmp/rmp_api.h"
typedef struct { int mode; } fs;
static int32_t f_init(const rmp_host *h) { (void)h; return 0; }
static void f_vinit(void *s) { (void)s; }
static void f_on(void *s, float n, float v) { (void)s; (void)n; (void)v; }
static void f_off(void *s) { (void)s; }
static void f_params(void *s, const float *p) { ((fs *)s)->mode = (int)p[0] * 4 / 128; }
static void f_render(void *s, float *out, int32_t n) {
  int m = ((fs *)s)->mode;
  if (m == 1) *(volatile int *)0 = 1;
  if (m == 2) for (;;) {}
  volatile float x = 0; if (m == 3) for (int i = 0; i < 20000; ++i) x += (float)i;
  for (int32_t i = 0; i < n; ++i) out[i] = 0.0f;
}
RMP_ENTRY const rmp_plugin rmp_entry = {RMP_PLUGIN_MAGIC, RMP_API, sizeof(fs), 2,
  f_init, f_vinit, f_on, f_off, f_params, f_render, {0}};

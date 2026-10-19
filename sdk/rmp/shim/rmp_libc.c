// shim/rmp_libc.c - what a freestanding plugin needs from a C library, in the
// plugin itself (a plugin may not reference host symbols).
#include <stddef.h>
#include <stdint.h>
#ifndef RMP_NATIVE
void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *p = d; const uint8_t *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, size_t n) {
  uint8_t *p = d; const uint8_t *q = s;
  if (p < q) { while (n--) *p++ = *q++; } else { p += n; q += n; while (n--) *--p = *--q; }
  return d;
}
static int rmp_errno;
int *__errno(void) { return &rmp_errno; }
void __cxa_pure_virtual(void) { for (;;) {} }       /* a pure virtual call: the guard's timer ends it */
#endif

// core/tests/bench/shim - the host port's Mac-only calls (host/um_host.cpp) on Linux, for the GCC bench (bench.mk)
#pragma once
#include <stddef.h>
#define MAP_JIT 0
#define MAP_ANON 0x20
static inline void pthread_jit_write_protect_np(int) {}
static inline void sys_icache_invalidate(void* p, size_t n) { __builtin___clear_cache((char*)p, (char*)p + n); }

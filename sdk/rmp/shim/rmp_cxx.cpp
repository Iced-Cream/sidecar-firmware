// shim/rmp_cxx.cpp - C++ support without a heap: objects live in the voice
// state (placement new); virtual destructors reference operator delete.
#include <stddef.h>
#ifndef RMP_NATIVE
void operator delete(void *, size_t) noexcept {}
void operator delete(void *) noexcept {}
#endif

// core/heap_buffers.h - the core's big fixed buffers (64 KB and up) on a port that keeps them out of its image.
// By default (the Teensy, the Mac) they are static arrays, as always. A port whose core_port.h defines
// CORE_HEAP_BUFFERS (the Pi: Circle's KERNEL_MAX_SIZE counts .bss) has them as pointers instead, set once at boot by
// heap_buffers_place() into memory the port reserved for them: the core still never allocates. Indexing is the same
// either way (a pointer to the array's row type); only `sizeof` of a whole array differs, so none is taken.
#pragma once
#include <stddef.h>

namespace core {

#ifdef CORE_HEAP_BUFFERS
// The bytes the port reserves (64-byte aligned pieces), then the placement: called once, before engine_begin() and
// before any render core runs, on memory 64-byte aligned and at least heap_buffers_bytes() long. Zeroes it (what a
// static array would be) and constructs the objects among them (pool_files).
size_t heap_buffers_bytes();
void heap_buffers_place(void* mem);
#endif

}  // namespace core

namespace card {
#ifdef CORE_HEAP_BUFFERS
size_t heap_bytes();          // card_index.cpp: its folder table
void heap_place(void* mem);   // zeroed memory, 64-byte aligned
#endif
}  // namespace card

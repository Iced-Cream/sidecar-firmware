// core/platform.h - what the engine core needs from a board (docs/superpowers/specs/2026-10-14-pi-engine-design.md,
// "Layout"). The core is portable C++17: no Arduino, no Circle, no allocation in the audio path. A board puts its
// core_port.h on the include path (teensy/platform/, core/host/, pi/engine/). It defines:
//
//   files      class core::File - the part of SdFat's FsFile the core uses, same names and meanings:
//                open(path, mode), open(File* dir, uint32_t index, mode), openNext(File* dir, mode), close(),
//                isOpen(), isDir(), getName(char*, size_t), dirIndex(), fileSize(), read(void*, size_t), read(),
//                write(const void*, size_t), write(uint8_t), seekSet(uint64_t), curPosition(), available(),
//                rewind(), isContiguous(), contiguousRange(uint32_t* first, uint32_t* last),
//                getModifyDateTime(uint16_t* fat_date, uint16_t* fat_time); copying a File gives a second handle
//                with its own position
//              class core::Fs - the card: begin(), mkdir(path), rename(from, to), remove(path), exists(path),
//                tree_hash(path, uint32_t* entries, uint32_t* hidden): a fingerprint of everything below path
//                ('.' entries left out; 0 when path is missing; *entries and *hidden count the entries hashed and
//                the '.' entries left out where the walk sees names - the Teensy's raw FAT32 walk leaves both 0),
//                type_name()
//              open modes core::kRead, core::kWrite (create, truncate), core::kAppend (create, append),
//                core::kReadWrite (create, truncate, then read and write anywhere)
//   placement  CORE_COLD (function attribute: rarely run code - program flash on the Teensy),
//              CORE_RAM2 (variable attribute: bulk RAM - the Teensy's RAM2), CORE_STR(s) (a format string constant
//              - in flash on the Teensy)
//   printf     sniprintf / vsniprintf (newlib's integer printf; the Mac maps them to snprintf / vsnprintf)
//
// and the functions below (the board's platform_*.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "core_port.h"

namespace core {

// A free-running microsecond clock (wraps every 71.6 minutes) and milliseconds.
uint32_t now_us();
uint32_t now_ms();
// The CPU-time counter for budgets and the stats (Teensy: DWT CYCCNT; Pi: CNTPCT; Mac: nanoseconds), and its rate.
uint32_t cycles();
uint32_t cycles_per_second();
// One log line, printf-style with integers only (%d %u %ld %lu %x %02x %s %c), no newline.
void log(const char* fmt, ...);
// A seed for anything random.
uint32_t random_seed();
// The sample RAM (DSP SAMPLE's samples held whole, pool/sample_ram_pool.h): the bytes the board can give it
// (0: none - DSP SAMPLE streams from the card), and that memory. Teensy: PSRAM; Pi: the heap; Mac: malloc.
uint32_t sample_ram_total();
void* sample_ram_alloc(uint32_t bytes);   // nullptr if the board cannot give it
void sample_ram_free(void* p);

}  // namespace core

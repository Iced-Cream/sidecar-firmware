// core/code/loader.h - the SCX checker and loader (core/code/scx.h): checks a blob's header, offsets, relocations and
// imports against its own size and the engine's export table, then places it in a piece of the code arena (copy,
// zeroed bss, R_AARCH64_RELATIVE relocations, import slots). Making the piece executable and running the constructors
// are the caller's (core/code/code.cpp), since the first is the board's and the second runs blob code. Portable, no
// allocation, no globals written except the allocation window (control core only).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "scx.h"

namespace core {
namespace code {

// REFUSED codes (75), the design's numbering
enum Code : uint8_t { kOk = 0, kFull = 3, kBad = 5, kAbi = 9, kHashBad = 10, kImport = 11, kClash = 12, kCrashed = 13 };
constexpr uint32_t kHeader = sizeof(scx_header);   // 64
constexpr uint32_t kMaxBlob = 1u << 20;            // a blob file: 1 MB (PLAITS ~170 KB)

// The whole file f (n bytes): kOk and *h its header, or the code to refuse it with. Checks magic, abi, flags, name,
// that the image, the relocation and import tables lie inside the file and end it, that every relocation and import
// slot lies inside the image and every relocation's addend inside image + bss, that every import is in the export
// table, and that entry and the constructors lie inside the image.
int check(const uint8_t* f, uint32_t n, scx_header* h);
// bytes the blob takes in memory: image + bss
inline uint32_t mem_bytes(const scx_header& h) { return h.image_size + h.bss_size; }
// where its read + execute part ends once loaded: data_off with SCX_F_DATA, else the whole of it
inline uint32_t rx_bytes(const scx_header& h) { return (h.flags & SCX_F_DATA) ? h.data_off : mem_bytes(h); }
// Places a checked file at mem (cap bytes, at least mem_bytes): copies the image, zeroes the bss, applies the
// relocations and fills the imports. kOk, or kFull / kBad.
int place(const uint8_t* f, uint32_t n, uint8_t* mem, uint32_t cap);
// The block descriptor of a placed blob, after its constructors ran: scx_entry() called and its answer checked (abi,
// name = the header's, params, state bytes up to max_state). Null: refused (*code says why).
const scx_block* describe(uint8_t* mem, const scx_header& h, uint32_t max_state, int* code);
// the constructors of a placed blob, in order
void run_init(uint8_t* mem, const scx_header& h);

// the export table (ABI 1)
const void* find_export(uint32_t hash);
int export_count();
const char* export_name(int i);

// scx_alloc / operator new hand out [p, p + n) while a window is open (make() on the control core), null otherwise
void alloc_window(void* p, size_t n);
size_t alloc_used();

}  // namespace code
}  // namespace core

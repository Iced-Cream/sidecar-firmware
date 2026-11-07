/* core/code/scx.h - SCX, the Sidecar's shipped-code blob (version 1; design "Sidecar code shipping", 2026-11-07).
 * A blob is one position-independent AArch64 image carried by a Rytm client mod, offered to the Pi by the companion,
 * kept on the Pi's card by its SHA-256 and loaded into the DSP library as one more block (core/code/code.h). This header
 * is C so that a blob's own sources can include it; the engine includes it from C++.
 *
 * File layout (little endian):
 *   0         struct scx_header (64 bytes)
 *   64        the image: code, read-only data and data, laid out from address 0 as the blob was linked
 *   64+image  relocs: reloc_count x uint32_t image offsets; each holds a 64-bit image offset, the loader adds the base
 *             (R_AARCH64_RELATIVE with the addend written in place; nothing else)
 *   then      imports: import_count x struct scx_import: the 64-bit slot at image offset `offset` gets the address of the
 *             engine export whose name hashes to `hash` (scx_hash), plus the addend already in the slot
 * The bss (bss_size zero bytes) follows the image in memory. Every offset in the header is an image offset except
 * reloc_off / import_off, which are file offsets.
 *
 * Rules every blob keeps from day one, so a sandbox (LFI, decided 2026-11-07) can be added to the loader later without
 * changing blob sources:
 *   1. no engine calls while rendering: process / set_param / midi / link_reset call nothing outside the blob (link
 *      memcpy and the math the blob needs into it); the export table is for make() and the constructors
 *   2. allocation only in make(): operator new / scx_alloc hand out the slot's own memory during make() and nothing
 *      after (null)
 *   3. memory reach = the blob's own image, the state make() built and the I/O buffers process() receives
 */
#ifndef SCX_H
#define SCX_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCX_MAGIC 0x31584353u /* "SCX1" */
#define SCX_ABI 1             /* the engine ABI: struct scx_io, struct scx_block and the export table below */
#define SCX_NAME_MAX 8
#define SCX_PARAMS_MAX 64
#define SCX_CHANNELS 12
#define SCX_SLOT_IO 2

struct scx_header {
  uint32_t magic;          /* SCX_MAGIC */
  uint16_t abi;            /* SCX_ABI the blob was built for: the Pi refuses any other */
  uint16_t flags;          /* 0 */
  char name[SCX_NAME_MAX]; /* PLACE's name: upper case ASCII, NUL padded */
  uint32_t image_size;     /* bytes of image after the header */
  uint32_t bss_size;       /* zero bytes after the image */
  uint32_t entry;          /* image offset of scx_entry(), the one exported function */
  uint32_t init_off;       /* image offset of the constructors (64-bit pointers, relocated) */
  uint32_t init_count;
  uint32_t reloc_off;      /* file offset */
  uint32_t reloc_count;
  uint32_t import_off;     /* file offset */
  uint32_t import_count;
  uint32_t reserved[3];
};

struct scx_import {
  uint32_t hash;           /* scx_hash of the export's name */
  uint32_t offset;         /* image offset of the 64-bit slot */
};

/* One audio block, the engine's core::proc::Block as it is (the engine checks the layout). in: the Rytm's Overbridge
 * IN channels (read only); out: OUT, added to (never cleared); sin / sout: this slot's own stereo input (what the
 * route summed into it) and output (zeroed before the call). A block normally reads sin and writes sout. */
struct scx_io {
  const float* in[SCX_CHANNELS];
  float* out[SCX_CHANNELS];
  int frames;
  const float* sin[SCX_SLOT_IO];
  float* sout[SCX_SLOT_IO];
};

struct scx_param {
  const char* name;        /* upper case, at most 8 */
  uint16_t def;            /* 0..16383 */
};

/* What scx_entry() returns: plain C, so the blob never depends on the engine's C++ layout. */
struct scx_block {
  uint32_t abi;            /* SCX_ABI */
  const char* name;        /* the same as the header's */
  uint32_t nparams;        /* up to SCX_PARAMS_MAX */
  const struct scx_param* params;
  uint32_t state_bytes;    /* make()'s state; it and what make() allocates fit the slot (1 MB less the adapter) */
  /* builds one voice / effect in mem (state_bytes, 64-byte aligned, zeroed); returns the state (normally mem) or null */
  void* (*make)(void* mem);
  void (*process)(void* state, const struct scx_io* io);           /* render side */
  void (*set_param)(void* state, int param, int value14);          /* render side, before process */
  void (*midi)(void* state, int src, int status, int d1, int d2);  /* render side, may be null */
  void (*link_reset)(void* state);                                 /* render side, may be null */
};

/* the blob's one entry point (its symbol name for the packer) */
const struct scx_block* scx_entry(void);

/* FNV-1a, 32 bits: the export names' hash */
static inline uint32_t scx_hash(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}

/* The export table, ABI 1 (engine side: core/code/loader.cpp). Names as the linker sees them.
 *   allocation (make() only):  scx_alloc(size) scx_free(p) _Znwm _Znam _ZdlPv _ZdaPv _ZdlPvm _ZdaPvm
 *   C memory:                  memcpy memmove memset memcmp strlen
 *   math (float):              sinf cosf tanf atanf atan2f expf exp2f logf log2f log10f powf sqrtf fabsf floorf ceilf
 *                              fmodf tanhf roundf
 *   C++ runtime:               __cxa_pure_virtual __cxa_atexit (no-op: a blob is never torn down by its destructors)
 */
void* scx_alloc(size_t n);
void scx_free(void* p);

#ifdef __cplusplus
}
#endif
#endif

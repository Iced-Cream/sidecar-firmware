// rmp_format.h - the .rmp file layout (tools/rmp_pack.py writes it; keep in step,
// tools/tests/test_rmp_pack.py compiles this header and compares the layout).
#pragma once
#include <stdint.h>
#define RMP_FILE_MAGIC 0x31504D52u       /* "RMP1": an ARM Thumb-2 image (the Teensy) */
#define RMP_FILE_MAGIC_A64 0x41504D52u   /* "RMPA": an AArch64 image (the Pi; 2026-10-17, docs/user-machines-pi.md): the
                                           same header, manifest and plugin ABI built for AArch64; relocations
                                           RMP_REL_ABS64 only, the image placed at a 4096-byte aligned address */
#define RMP_FORMAT 3u          /* written by rmp_pack.py: the manifest head and knobs below (format 3) */
#define RMP_FORMAT_MIN 1u      /* the oldest the loader accepts: 1 differs only in the manifest
                                  (20-byte knobs p, look, text, pad, long, short; no name table) */
#define RMP_HEADER_BYTES 96u
/* RMPA packages only: ABS64 a 64-bit word, moved by the code part's base when its value is below data_start, else
   by the data part's; A64_ADRP_DATA an ADRP in the code part whose target is in the data part (the page distance
   between the two parts changes). An RMPA header's reserved[0..3] is data_start (page-aligned): [0, data_start) is
   code + read-only data (executable memory), [data_start, image_size) + bss the writable part (RW memory). */
enum { RMP_REL_ABS32 = 0, RMP_REL_MOVW = 1, RMP_REL_MOVT = 2, RMP_REL_ABS64 = 3, RMP_REL_A64_ADRP_DATA = 4 };
enum { RMP_TARGET_ARM32 = 0, RMP_TARGET_A64 = 1 };   /* a package's target, from its magic */
#define RMP_A64_ALIGN 4096u   /* an RMPA image's base alignment (ADRP and the :lo12: offsets are page-relative) */
typedef struct __attribute__((packed)) {
  uint32_t magic; uint16_t format; uint16_t api;
  char id[32]; uint32_t version; uint32_t compatible_from;
  uint32_t image_size; uint32_t bss_size; uint32_t entry_offset;
  uint32_t reloc_count; uint32_t manifest_size; uint32_t flags; uint32_t crc32;
  uint8_t reserved[20];
} rmp_header;
typedef struct __attribute__((packed)) { uint32_t where_type; uint32_t value; } rmp_reloc;
/* enum_count names for this knob's values (look "list"): entries enum_first ..
   enum_first + enum_count - 1 of the name table that follows the knobs. */
typedef struct __attribute__((packed)) {
  uint8_t p, look, text, enum_first, enum_count, reserved; char long_name[12]; char short_name[4];
} rmp_knob;
/* Manifest: rmp_manifest_head, knob_count rmp_knob, then the name table (as many
   entries as the knobs' enum_count add up to, each RMP_ENUM_NAME_BYTES, NUL-padded
   ASCII, at most RMP_ENUM_NAME_BYTES - 1 characters). */
#define RMP_ENUM_NAME_BYTES 8
#define RMP_ENUM_MAX 16             /* names per knob */
typedef struct __attribute__((packed)) { char name[RMP_ENUM_NAME_BYTES]; } rmp_enum_name;
typedef struct __attribute__((packed)) {
  char name[10]; char short_name[4]; char licence[48]; uint8_t page2; uint8_t knob_count;
} rmp_manifest_head;
/* Format 3 (2026-10-10, sub-project 2): the head adds the page-2 title and the native presets'
   file extension; a knob adds its preset role and its range and default (0..127). Formats 1 and 2
   read with role free, min 0, max 127, default 64, no title, no native presets. */
typedef struct __attribute__((packed)) {
  uint8_t p, look, text, enum_first, enum_count, role, min, max, def, reserved; char long_name[12]; char short_name[4];
} rmp_knob3;
typedef struct __attribute__((packed)) {
  char name[10]; char short_name[4]; char licence[48]; uint8_t page2; uint8_t knob_count;
  char page2_title[13];   /* NUL-padded; "" = the machine's name */
  char preset_ext[7];     /* letters and digits, no dot, NUL-padded; "" = no native presets */
} rmp_manifest_head3;
enum { RMP_LOOK_DIAL = 0, RMP_LOOK_CENTRED, RMP_LOOK_FADER, RMP_LOOK_LIST };
enum { RMP_TEXT_NUMBER = 0, RMP_TEXT_BIPOLAR, RMP_TEXT_ENUM, RMP_TEXT_LIST, RMP_TEXT_NOTE, RMP_TEXT_TIME,
       RMP_TEXT_PRESET, RMP_TEXT_POOL };
enum { RMP_ROLE_FREE = 0, RMP_ROLE_PRESET, RMP_ROLE_OFFSET };
#ifdef __cplusplus
static_assert(sizeof(rmp_header) == RMP_HEADER_BYTES, "rmp header size");
static_assert(sizeof(rmp_reloc) == 8, "rmp reloc size");
#else
_Static_assert(sizeof(rmp_header) == RMP_HEADER_BYTES, "rmp header size");
_Static_assert(sizeof(rmp_reloc) == 8, "rmp reloc size");
#endif
#ifdef __cplusplus
static_assert(sizeof(rmp_knob) == 22, "rmp knob size");
#else
_Static_assert(sizeof(rmp_knob) == 22, "rmp knob size");
#endif
#ifdef __cplusplus
static_assert(sizeof(rmp_knob3) == 26, "rmp knob3 size");
static_assert(sizeof(rmp_manifest_head3) == 84, "rmp manifest head3 size");
#else
_Static_assert(sizeof(rmp_knob3) == 26, "rmp knob3 size");
_Static_assert(sizeof(rmp_manifest_head3) == 84, "rmp manifest head3 size");
#endif

// rmp_manifest.h - a package's manifest (formats 1..3) as a slot description, read through a
// streaming callback (the store's read_at: never the package whole in RAM). The Teensy serves
// 1A (metadata) and 1B (enum names) from it (sim/protocol.py). Portable: the Mac tests it.
#pragma once
#include <stdint.h>
#include "rmp_loader.h"
#include "rmp_format.h"
#define RMP_DESC_KNOBS 16
struct rmp_desc_knob {
  uint8_t p;                          // 1..16
  uint8_t look, kind, role;           // RMP_LOOK_*, RMP_TEXT_* (the manifest's "text"), RMP_ROLE_*
  uint8_t min, max, def;              // 0..127, min <= def <= max
  uint8_t enum_first, enum_count;     // into the package's name table (enum_count <= 16)
  char long_name[13], short_name[5];  // printable ASCII, NUL-padded and always NUL-terminated (12 / 4 characters)
};
struct rmp_desc {
  char name[11], short_name[5], p2_title[14];   // printable ASCII, NUL-padded and always NUL-terminated
                                                // (10 / 4 / 13 characters: the manifest's widths, plus the NUL)
  char preset_ext[8];                           // NUL-terminated, letters and digits; "" none
  uint8_t page2;                                // 1: P9..P16 are knobs ("params"), 0: the sample layer
  uint8_t nknobs;                               // knob[0..nknobs-1] in p order, each p once
  int8_t pre;                                   // the knob with kind preset (the first by p), -1 none
  uint8_t format;                               // the package's format, 1..3
  uint32_t names_at, names_end;                 // the name table's package offsets (0, 0: none)
  rmp_desc_knob knob[RMP_DESC_KNOBS];
};
// The manifest of a package whose header *info came from rmp_verify_header (format, image_size,
// reloc_count, manifest_size): false on a read error, a format outside 1..3, more than 32 knobs, or a
// manifest shorter than its head and knobs. Knobs with p outside 1..16, or a p seen before, are skipped;
// strings become printable ASCII ('_' for others), NUL-padded and NUL-terminated (every string field
// has one byte more than its manifest width). Format-3 min/max/default bytes above 127 saturate to
// 127; a range with min > max is read as 0..127 with the default 64; otherwise the default is clamped
// into min..max. Reads: the head, then one knob a read.
bool rmp_manifest_read(const rmp_info *info, rmp_read_fn rd, void *ctx, rmp_desc *d);
// Knob k's enum names (knob[k].enum_count of them) into names[][8], NUL-padded, at most 7 characters:
// the count, 0 when it has none, -1 on a read error or a name table that does not hold them.
int rmp_manifest_enum(const rmp_desc *d, int k, rmp_read_fn rd, void *ctx, char names[16][8]);
// The knob for P p (1..16): its index in knob[], -1 none.
int rmp_desc_find(const rmp_desc *d, int p);
// A description without a manifest (a missing package): the name is the id's last dot-separated
// part in upper case, at most 10 characters (befaco.np.atari -> "ATARI"), the short name its first 4
// (both NUL-terminated); no knobs.
void rmp_desc_from_id(const char *id, rmp_desc *d);

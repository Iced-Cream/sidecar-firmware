// rmp_store_probe.h - what the raw program flash holds where the plugin store goes, before anything mounts
// it. LittleFS 2.4's lfs_mount does not compare the superblock's block_count with the configuration, and the
// core's LittleFS_Program::begin formats whenever a mount fails: a build with another RMP_STORE_BYTES would
// mount a stale store's leftovers inside ours (a smaller store's base lies inside a larger one) or wipe ours.
// rmp_store.cpp mounts only OURS, formats only BLANK and leaves FOREIGN alone (an explicit wipe only).
// No Teensy dependency: extras/store_probe_test.cpp runs it on littlefs images built on the Mac.
#pragma once
#include <stdint.h>
// A littlefs superblock read straight from a metadata pair (blocks 0 and 1 of a region): the newest block
// (revision count) with a CRC-valid commit, as lfs_mount picks it; the superblock entry is the name tag
// "littlefs" (type 0x0ff) and the inline struct of the same id (type 0x201: version, block_size, block_count,
// name_max, file_max, attr_max). found false: no valid commit in either block, or none holding a superblock.
struct rmp_superblock { bool found; uint32_t version, block_size, block_count; };
rmp_superblock rmp_sb_parse(const uint8_t *blk0, const uint8_t *blk1, uint32_t block_size);

enum rmp_layout {
  RMP_LAYOUT_OURS,      // our superblock: littlefs 2.x, our block size and block count - mount it
  RMP_LAYOUT_BLANK,     // no superblock and the whole region erased (all 0xFF) - format it (first boot)
  RMP_LAYOUT_FOREIGN    // anything else - neither mount nor format
};
enum rmp_foreign_why {
  RMP_WHY_NONE = 0,
  RMP_WHY_LARGER,       // a CRC-valid commit at a larger known store's base {0, 1} (that store owns our region)
  RMP_WHY_MISMATCH,     // our superblock with another block count / size / version
  RMP_WHY_NOT_BLANK     // no superblock of ours, and the region is not erased
};
#define RMP_PROBE_MAX 4
// a store of `bytes` at top - bytes; valid: its pair {0, 1} holds a CRC-valid commit (with or without a superblock)
struct rmp_probe_at { uint32_t bytes; rmp_superblock sb; bool valid; };
struct rmp_probe {
  rmp_layout layout;
  rmp_foreign_why why;
  int n;                       // at[0] is ours; then each other known size that fits above the image
  rmp_probe_at at[RMP_PROBE_MAX];
};
// Each region's superblock is looked for as lfs_rawmount does: from the pair {0, 1} along the metadata tails (a
// superblock expansion leaves {0, 1} with only a hard tail), within the region, at most block_count / 2 pairs.
// top: the end of the store area (a store of B bytes is [top - B, top)); room: the bytes below top that may be
// read (down to the program image's end); known: the store sizes builds have used (ours may be among them);
// feed: called every block of the blank scan (the watchdog), may be null.
void rmp_store_probe(const uint8_t *top, uint32_t room, uint32_t ours, uint32_t block_size, const uint32_t *known,
                     int nknown, void (*feed)(), rmp_probe *out);

// rmp_store_probe.cpp - see rmp_store_probe.h. The metadata format is littlefs's SPEC.md (disk version 2.0),
// read the way lfs_dir_fetchmatch reads it.
#include "rmp_store_probe.h"
#include <string.h>
#include "rmp_loader.h"   // rmp_crc32_update (littlefs's lfs_crc: the same reflected CRC-32, no final xor)

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return p[3] | p[2] << 8 | p[1] << 16 | (uint32_t)p[0] << 24; }
static uint32_t tag_size(uint32_t t) { return t & 0x3ff; }
static uint32_t tag_dsize(uint32_t t) { return 4 + ((tag_size(t) + (tag_size(t) == 0x3ff)) & 0x3ff); }   // a delete: 0

// One block's commits: valid (one CRC-valid commit at least), and the superblock and the tail (a soft or hard
// tail tag, 0x600 / 0x601: the next metadata pair; 0xFFFFFFFF none) as of the last valid commit.
RMP_COLD static bool scan_block(const uint8_t *b, uint32_t bs, rmp_superblock *sb, uint32_t tail[2]) {
  bool valid = false;
  rmp_superblock cur = {}, tmp = {};
  uint32_t cur_tail[2] = {0xFFFFFFFFu, 0xFFFFFFFFu}, tmp_tail[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
  int tmp_name = -1;   // the superblock entry's id (its name tag seen in this block)
  uint32_t crc = rmp_crc32_update(0xFFFFFFFFu, b, 4);
  uint32_t ptag = 0xFFFFFFFFu, off = 0;
  for (;;) {
    off += tag_dsize(ptag);
    if (off + 4 > bs) break;
    uint32_t tag = be32(b + off) ^ ptag;
    crc = rmp_crc32_update(crc, b + off, 4);
    if (tag & 0x80000000u) break;                  // not programmed yet (or garbage)
    if (off + tag_dsize(tag) > bs) break;
    ptag = tag;
    uint32_t type = tag >> 20 & 0x7ff, id = tag >> 10 & 0x3ff, size = tag_size(tag);
    if ((type & 0x700) == 0x500) {                 // CRC: the commit ends here
      if (off + 8 > bs || le32(b + off + 4) != crc) break;
      ptag ^= (uint32_t)((tag >> 20) & 1) << 31;   // lfs_tag_chunk(tag) & 1
      valid = true;
      cur = tmp;
      cur_tail[0] = tmp_tail[0];
      cur_tail[1] = tmp_tail[1];
      crc = 0xFFFFFFFFu;
      continue;
    }
    crc = rmp_crc32_update(crc, b + off + 4, tag_dsize(tag) - 4);
    if (size == 0x3ff) continue;                   // a delete: no data
    const uint8_t *d = b + off + 4;
    if (type == 0x0ff && size == 8 && !memcmp(d, "littlefs", 8)) tmp_name = (int)id;
    else if (type == 0x201 && (int)id == tmp_name && size >= 12) {
      tmp.found = true;
      tmp.version = le32(d);
      tmp.block_size = le32(d + 4);
      tmp.block_count = le32(d + 8);
    } else if ((type & 0x7fe) == 0x600 && size == 8) {
      tmp_tail[0] = le32(d);
      tmp_tail[1] = le32(d + 4);
    }
  }
  if (valid) {
    *sb = cur;
    tail[0] = cur_tail[0];
    tail[1] = cur_tail[1];
  }
  return valid;
}

// One metadata pair: false when neither block has a valid commit.
RMP_COLD static bool parse_pair(const uint8_t *blk0, const uint8_t *blk1, uint32_t bs, rmp_superblock *sb,
                                uint32_t tail[2]) {
  const uint8_t *b[2] = {blk0, blk1};
  int r = (int32_t)(le32(blk1) - le32(blk0)) > 0 ? 1 : 0;   // lfs_scmp: the newer revision first
  for (int i = 0; i < 2; ++i)
    if (scan_block(b[(r + i) % 2], bs, sb, tail)) return true;   // the first block with a valid commit decides
  return false;
}

RMP_COLD rmp_superblock rmp_sb_parse(const uint8_t *blk0, const uint8_t *blk1, uint32_t bs) {
  rmp_superblock sb = {};
  uint32_t tail[2];
  parse_pair(blk0, blk1, bs, &sb, tail);
  return sb;
}

// A region of nblocks at base: the superblock as lfs_rawmount finds it - from the pair {0, 1} along the tails until
// a pair holds one. littlefs 2.4's superblock expansion (lfs_dir_compact, rev % (block_cycles + 1 | 1) == 0 on
// {0, 1} while under half the blocks are in use) moves the superblock entry into a new pair and leaves {0, 1}
// with only a hard tail to it. A tail outside the region, a pair without a valid commit or more than nblocks / 2
// pairs (mount's loop limit): none. *valid: {0, 1} holds a CRC-valid commit.
// The probe stops at the first pair holding a superblock, as a mount that succeeds does; lfs_rawmount then walks the
// rest of the chain (lfs_dir_fetchmatch on every pair, the gstate) and fails on a corrupt one later on: such a store
// reads OURS here and its mount fails - rmp_store leaves a failed mount unmounted (never formats it).
RMP_COLD static rmp_superblock parse_at(const uint8_t *base, uint32_t bs, uint32_t nblocks, bool *valid) {
  rmp_superblock sb = {};
  uint32_t p0 = 0, p1 = 1;
  *valid = false;
  for (uint32_t cycle = 0; cycle < nblocks / 2; ++cycle) {
    uint32_t tail[2];
    sb = rmp_superblock{};
    bool ok = parse_pair(base + p0 * bs, base + p1 * bs, bs, &sb, tail);
    if (cycle == 0) *valid = ok;
    if (!ok || sb.found) return sb;
    if (tail[0] >= nblocks || tail[1] >= nblocks) break;   // none (0xFFFFFFFF, lfs_pair_isnull) or outside
    p0 = tail[0];
    p1 = tail[1];
  }
  return rmp_superblock{};
}

RMP_COLD static bool blank(const uint8_t *p, uint32_t n, uint32_t bs, void (*feed)()) {
  for (uint32_t o = 0; o < n; o += bs) {
    if (feed) feed();
    const uint32_t *w = (const uint32_t *)(p + o);
    for (uint32_t i = 0; i < bs / 4; ++i)
      if (w[i] != 0xFFFFFFFFu) return false;
  }
  return true;
}

RMP_COLD void rmp_store_probe(const uint8_t *top, uint32_t room, uint32_t ours, uint32_t bs, const uint32_t *known,
                              int nknown, void (*feed)(), rmp_probe *out) {
  memset(out, 0, sizeof *out);
  out->at[0].bytes = ours;
  out->at[0].sb = parse_at(top - ours, bs, ours / bs, &out->at[0].valid);
  out->n = 1;
  for (int i = 0; i < nknown && out->n < RMP_PROBE_MAX; ++i) {
    uint32_t k = known[i];
    if (k == ours || k < 2 * bs || k > room) continue;
    out->at[out->n].bytes = k;
    out->at[out->n].sb = parse_at(top - k, bs, k / bs, &out->at[out->n].valid);
    ++out->n;
  }
  // A CRC-valid commit at a larger store's base {0, 1}, superblock found along the tails or not (an expanded
  // superblock's pair may be damaged or erased): that base is below ours, outside our region, so it is no file of
  // ours; its store holds our region as its own blocks. (A smaller store's base lies inside ours: our files, or
  // stale leftovers in our free blocks - it decides nothing while our own superblock is there.)
  for (int i = 1; i < out->n; ++i)
    if (out->at[i].bytes > ours && (out->at[i].sb.found || out->at[i].valid)) {
      out->layout = RMP_LAYOUT_FOREIGN;
      out->why = RMP_WHY_LARGER;
      return;
    }
  const rmp_superblock &s = out->at[0].sb;
  if (s.found) {
    bool same = (s.version >> 16) == 2 && s.block_size == bs && s.block_count == ours / bs;
    out->layout = same ? RMP_LAYOUT_OURS : RMP_LAYOUT_FOREIGN;
    out->why = same ? RMP_WHY_NONE : RMP_WHY_MISMATCH;
    return;
  }
  bool b = blank(top - ours, ours, bs, feed);
  out->layout = b ? RMP_LAYOUT_BLANK : RMP_LAYOUT_FOREIGN;
  out->why = b ? RMP_WHY_NONE : RMP_WHY_NOT_BLANK;
}

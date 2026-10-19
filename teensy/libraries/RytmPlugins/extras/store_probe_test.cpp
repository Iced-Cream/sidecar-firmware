// extras/store_probe_test.cpp - the store's layout probe (rmp_store_probe) on littlefs images made on the Mac
// with the Teensy core's own littlefs (2.4), in a RAM copy of the top of program flash.
// build: L=$(ls -d ~/Library/Arduino15/packages/teensy/hardware/avr/*/libraries/LittleFS/src/littlefs | tail -1) &&
//   cc -O1 -c -I$L $L/lfs.c -o /tmp/lfs.o && cc -O1 -c -I$L $L/lfs_util.c -o /tmp/lfs_util.o &&
//   c++ -std=c++17 -O1 -Wall -I../src -I$L store_probe_test.cpp ../src/rmp_store_probe.cpp ../src/rmp_loader.cpp
//   /tmp/lfs.o /tmp/lfs_util.o -o /tmp/store_probe_test && /tmp/store_probe_test
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include "rmp_store_probe.h"
#include "lfs.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static const uint32_t BS = 65536, MB = 1u << 20;
static const uint32_t KNOWN[] = {1 * MB, 4 * MB};
static std::vector<uint8_t> flash;   // the top 4 MB of program flash (the store area): top = flash.data() + size
static uint8_t *top() { return flash.data() + flash.size(); }

static int rd(const lfs_config *c, lfs_block_t b, lfs_off_t o, void *buf, lfs_size_t n) {
  memcpy(buf, (uint8_t *)c->context + b * BS + o, n); return 0;
}
// A power cut: after `cut` more programmed bytes nothing is programmed or erased any more (the last prog torn
// midway); -1 none. `progged` counts every programmed byte.
static long cut = -1, progged = 0;
static int pg(const lfs_config *c, lfs_block_t b, lfs_off_t o, const void *buf, lfs_size_t n) {
  uint8_t *d = (uint8_t *)c->context + b * BS + o;
  for (lfs_size_t i = 0; i < n; ++i) {
    if (cut == 0) return 0;
    if (cut > 0) --cut;
    ++progged;
    d[i] &= ((const uint8_t *)buf)[i];   // NOR: program clears bits only
  }
  return 0;
}
static int er(const lfs_config *c, lfs_block_t b) {
  if (cut != 0) memset((uint8_t *)c->context + b * BS, 0xFF, BS);
  return 0;
}
static int sy(const lfs_config *) { return 0; }
// the Teensy's configuration (LittleFS_Program on a 4.1, rmp_store's metadata_max) for a store of `bytes` at top - bytes
static lfs_config cfg_for(uint32_t bytes) {
  lfs_config c = {};
  c.context = top() - bytes;
  c.read = rd; c.prog = pg; c.erase = er; c.sync = sy;
  c.read_size = 128; c.prog_size = 128; c.block_size = BS; c.block_count = bytes / BS;
  c.block_cycles = 800; c.cache_size = 128; c.lookahead_size = 128; c.name_max = LFS_NAME_MAX; c.metadata_max = 4096;
  return c;
}
// put without the checks (a power cut makes littlefs's read-back fail): stops at the first error
static void put_try(lfs_t *L, const char *path, uint32_t n, uint8_t seed) {
  lfs_file_t f;
  if (lfs_file_open(L, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) < 0) return;
  std::vector<uint8_t> d(n, seed);
  lfs_file_write(L, &f, d.data(), n);
  lfs_file_close(L, &f);
}
static void put(lfs_t *L, const char *path, uint32_t n, uint8_t seed) {
  lfs_file_t f;
  CHECK(lfs_file_open(L, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) >= 0);
  std::vector<uint8_t> d(n);
  for (uint32_t i = 0; i < n; ++i) d[i] = (uint8_t)(seed + i * 7);
  CHECK(lfs_file_write(L, &f, d.data(), n) == (lfs_ssize_t)n);
  CHECK(lfs_file_close(L, &f) >= 0);
}
// format a store of `bytes`, then the use rmp_store makes of it: /machines, packages, an install file renamed,
// removes, and enough commits that the root and /machines compact (metadata_max) more than once
static void make_store(uint32_t bytes) {
  lfs_config c = cfg_for(bytes);
  lfs_t L;
  CHECK(lfs_format(&L, &c) >= 0);
  CHECK(lfs_mount(&L, &c) >= 0);
  CHECK(lfs_mkdir(&L, "/machines") >= 0);
  char p[64];
  for (int i = 0; i < (bytes > 1 * MB ? 8 : 4); ++i) {   // (a 1 MB store: 16 blocks, one a file at least)
    put(&L, "/machines/.install", 5000 + i * 7000, (uint8_t)i);
    snprintf(p, sizeof p, "/machines/pkg%d.rmp", i);
    CHECK(lfs_rename(&L, "/machines/.install", p) >= 0);
  }
  for (int r = 0; r < 40; ++r) {
    snprintf(p, sizeof p, "/machines/tmp%d.rmp", r % 5);
    put(&L, p, 300, (uint8_t)r);
    if (r % 3 == 0) CHECK(lfs_remove(&L, p) >= 0);
  }
  for (int r = 0; r < 150; ++r) {   // root commits: the superblock's own pair compacts too
    put(&L, "/x", 16, (uint8_t)r);
    CHECK(lfs_remove(&L, "/x") >= 0);
  }
  CHECK(lfs_unmount(&L) >= 0);
  uint32_t r0, r1;
  memcpy(&r0, c.context, 4); memcpy(&r1, (uint8_t *)c.context + BS, 4);
  CHECK(r0 + r1 > 5);   // compacted more than once (a format leaves revisions 1 and 2)
}
static void probe(uint32_t ours, rmp_probe *pr) { rmp_store_probe(top(), (uint32_t)flash.size(), ours, BS, KNOWN, 2, nullptr, pr); }
static int feeds;
static void feed() { ++feeds; }
// root commits until littlefs 2.4 expands the superblock (block_cycles 800: at {0, 1}'s revision 801, the store
// under half full): {0, 1} then holds no superblock, only a hard tail to the pair that does. The store stays mounted.
static void expand(lfs_t *L, lfs_config *c) {
  for (long i = 0; i < 400000; ++i) {
    put(L, "/x", 16, (uint8_t)i);
    CHECK(lfs_remove(L, "/x") >= 0);
    if (i % 100 == 0 && !rmp_sb_parse((uint8_t *)c->context, (uint8_t *)c->context + BS, BS).found) return;
  }
  CHECK(!"the superblock never expanded");
}
// Mount and check what make_store left (pkg0..), then unmount: false when littlefs refuses to mount.
static bool mounts_with_files(uint32_t bytes, int npkg) {
  lfs_config c = cfg_for(bytes);
  lfs_t L; lfs_info in;
  if (lfs_mount(&L, &c) < 0) return false;
  char p[64];
  for (int i = 0; i < npkg; ++i) {
    snprintf(p, sizeof p, "/machines/pkg%d.rmp", i);
    CHECK(lfs_stat(&L, p, &in) >= 0 && in.size == 5000u + i * 7000u);
  }
  lfs_unmount(&L);
  return true;
}

int main() {
  rmp_probe pr;
  // blank: format (first boot); every block of the scan fed
  flash.assign(4 * MB, 0xFF);
  feeds = 0;
  rmp_store_probe(top(), 4 * MB, 4 * MB, BS, KNOWN, 2, feed, &pr);
  CHECK(pr.layout == RMP_LAYOUT_BLANK && !pr.at[0].sb.found && pr.n == 2 && pr.at[1].bytes == 1 * MB && feeds == 64);
  probe(1 * MB, &pr);
  CHECK(pr.layout == RMP_LAYOUT_BLANK);

  // our 4 MB store in use: OURS for the 4 MB build; foreign to a 1 MB or 2 MB build (the larger store's superblock)
  make_store(4 * MB);
  probe(4 * MB, &pr);
  CHECK(pr.layout == RMP_LAYOUT_OURS && pr.at[0].sb.found && pr.at[0].sb.block_count == 64 && pr.at[0].sb.block_size == BS &&
        pr.at[0].sb.version >> 16 == 2);
  probe(1 * MB, &pr);
  CHECK(pr.layout == RMP_LAYOUT_FOREIGN && pr.why == RMP_WHY_LARGER);
  probe(2 * MB, &pr);
  CHECK(pr.layout == RMP_LAYOUT_FOREIGN && pr.why == RMP_WHY_LARGER && pr.n == 3);
  {   // and littlefs agrees it mounts as ours, with the files
    lfs_config c = cfg_for(4 * MB);
    lfs_t L; lfs_info in;
    CHECK(lfs_mount(&L, &c) >= 0 && lfs_stat(&L, "/machines/pkg7.rmp", &in) >= 0 && in.size == 5000 + 7 * 7000);
    lfs_unmount(&L);
  }
  // a damaged newest block (its first commit's CRC): the older block of the pair decides, as in lfs_mount
  {
    std::vector<uint8_t> keep = flash;
    uint8_t *b0 = top() - 4 * MB, *b1 = b0 + BS;
    uint32_t r0, r1; memcpy(&r0, b0, 4); memcpy(&r1, b1, 4);
    uint8_t *newer = (int32_t)(r1 - r0) > 0 ? b1 : b0;
    newer[20] ^= 0x01;
    probe(4 * MB, &pr);
    CHECK(pr.at[0].sb.found && pr.at[0].sb.block_count == 64);
    // both blocks damaged: no superblock, not blank - foreign, never formatted
    (newer == b1 ? b0 : b1)[20] ^= 0x01;
    probe(4 * MB, &pr);
    CHECK(pr.layout == RMP_LAYOUT_FOREIGN && pr.why == RMP_WHY_NOT_BLANK && !pr.at[0].sb.found);
    flash = keep;
  }
  // a 1 MB build that ran after the 4 MB store existed: its own superblock pair at top - 1 MB, inside ours. The
  // 1 MB build sees the 4 MB superblock (foreign); the 4 MB build still sees its own (it cannot see the damage)
  make_store(1 * MB);
  probe(1 * MB, &pr);
  CHECK(pr.layout == RMP_LAYOUT_FOREIGN && pr.why == RMP_WHY_LARGER);
  probe(4 * MB, &pr);
  CHECK(pr.layout == RMP_LAYOUT_OURS && pr.at[1].sb.found && pr.at[1].sb.block_count == 16);

  // only the old 1 MB store (the 4 MB area otherwise erased): the 4 MB build must not format over it
  flash.assign(4 * MB, 0xFF);
  make_store(1 * MB);
  probe(4 * MB, &pr);
  CHECK(pr.layout == RMP_LAYOUT_FOREIGN && pr.why == RMP_WHY_NOT_BLANK && pr.at[1].sb.found && pr.at[1].sb.block_count == 16);
  probe(1 * MB, &pr);
  CHECK(pr.layout == RMP_LAYOUT_OURS);

  // our base holding a superblock of another size (a 2 MB store formatted at the 4 MB base): mismatch
  flash.assign(4 * MB, 0xFF);
  {
    lfs_config c = cfg_for(4 * MB);
    c.block_count = 32;
    lfs_t L;
    CHECK(lfs_format(&L, &c) >= 0);
  }
  probe(4 * MB, &pr);
  CHECK(pr.layout == RMP_LAYOUT_FOREIGN && pr.why == RMP_WHY_MISMATCH && pr.at[0].sb.block_count == 32);

  // garbage, and one stray byte in an otherwise erased area: foreign
  flash.assign(4 * MB, 0xFF);
  srand(1);
  for (uint32_t i = 0; i < 2 * BS; ++i) flash[i] = (uint8_t)rand();
  probe(4 * MB, &pr);
  CHECK(pr.layout == RMP_LAYOUT_FOREIGN && pr.why == RMP_WHY_NOT_BLANK && !pr.at[0].sb.found);
  flash.assign(4 * MB, 0xFF);
  flash[3 * MB + 12345] = 0x7F;
  probe(4 * MB, &pr);
  CHECK(pr.layout == RMP_LAYOUT_FOREIGN && pr.why == RMP_WHY_NOT_BLANK);

  // a known size above the room (the program image ends higher) is not probed
  flash.assign(4 * MB, 0xFF);
  rmp_store_probe(top(), 2 * MB, 1 * MB, BS, KNOWN, 2, nullptr, &pr);
  CHECK(pr.n == 1 && pr.layout == RMP_LAYOUT_BLANK);
  // superblock expansion: {0, 1} with only a hard tail - still ours (as lfs_mount finds it), and still the larger
  // store to a 1 MB build, also with a stale 1 MB superblock of its own inside (no false "ours")
  flash.assign(4 * MB, 0xFF);
  make_store(1 * MB);   // the stale 1 MB store, 3 MB up in the 4 MB region
  make_store(4 * MB);
  {
    lfs_config c = cfg_for(4 * MB);
    lfs_t L;
    CHECK(lfs_mount(&L, &c) >= 0);
    expand(&L, &c);
    CHECK(lfs_unmount(&L) >= 0);
  }
  CHECK(!rmp_sb_parse(top() - 4 * MB, top() - 4 * MB + BS, BS).found);   // {0, 1} alone: no superblock
  probe(4 * MB, &pr);
  CHECK(pr.layout == RMP_LAYOUT_OURS && pr.at[0].sb.found && pr.at[0].sb.block_count == 64);
  CHECK(pr.at[1].sb.found && pr.at[1].sb.block_count == 16);   // the stale 1 MB superblock is still there
  CHECK(mounts_with_files(4 * MB, 8));
  probe(1 * MB, &pr);
  CHECK(pr.layout == RMP_LAYOUT_FOREIGN && pr.why == RMP_WHY_LARGER && pr.at[1].sb.block_count == 64);
  {   // the expanded superblock's pair gone (every block below the 1 MB region but {0, 1} erased): the walk finds no
      // superblock, {0, 1}'s commit still makes it the larger store - never the stale 1 MB one as ours
    std::vector<uint8_t> keep = flash;
    memset(top() - 4 * MB + 2 * BS, 0xFF, 3 * MB - 2 * BS);
    probe(1 * MB, &pr);
    CHECK(!pr.at[1].sb.found && pr.at[1].valid && pr.at[0].sb.found);
    CHECK(pr.layout == RMP_LAYOUT_FOREIGN && pr.why == RMP_WHY_LARGER);
    flash = keep;
  }

  // a torn trailing commit: power cut at every 97th byte of commits in the root ({0, 1}) and in /machines (an
  // install's file and rename), and at every byte of the first few hundred: the probe says ours exactly when
  // lfs_mount mounts (littlefs is power-safe: always), with every package there
  flash.assign(4 * MB, 0xFF);
  make_store(4 * MB);
  {
    std::vector<uint8_t> keep = flash;
    auto op = []() {
      lfs_config c = cfg_for(4 * MB);
      lfs_t L;
      if (lfs_mount(&L, &c) < 0) return;
      put_try(&L, "/y", 40, 3);
      put_try(&L, "/machines/.install", 9000, 9);
      lfs_rename(&L, "/machines/.install", "/machines/pkg9.rmp");
      lfs_remove(&L, "/y");
      lfs_unmount(&L);
    };
    progged = 0;
    op();
    long total = progged;
    CHECK(total > 9000);
    int n = 0;
    for (long k = 0; k < total; k += k < 600 ? 1 : 97, ++n) {
      flash = keep;
      cut = k;
      op();
      cut = -1;
      probe(4 * MB, &pr);
      bool m = mounts_with_files(4 * MB, 8);
      if (pr.layout != RMP_LAYOUT_OURS || !m)
        fprintf(stderr, "cut at %ld of %ld bytes: probe %d why %d, mount %d\n", k, total, pr.layout, pr.why, m);
      CHECK(pr.layout == RMP_LAYOUT_OURS && m);
    }
    printf("torn commits: %d cuts of %ld bytes, all ours\n", n, total);
    flash = keep;
  }

  // an interrupted wipe (rmp_store_wipe in a 1 MB build over the 4 MB store: ours formatted and mounted with
  // /machines first, then the larger store's pair erased): cut anywhere in the format, the larger superblock is
  // still there - FOREIGN (larger), refused; cut after the first erase too (the pair's other block); OURS only
  // once both are erased
  flash.assign(4 * MB, 0xFF);
  make_store(4 * MB);
  {
    std::vector<uint8_t> keep = flash;
    auto wipe = []() {
      lfs_config c = cfg_for(1 * MB);
      lfs_t L;
      if (lfs_format(&L, &c) < 0 || lfs_mount(&L, &c) < 0) return;
      lfs_mkdir(&L, "/machines");
      lfs_unmount(&L);
    };
    progged = 0;
    wipe();
    long total = progged;
    int n = 0;
    for (long k = 0; k <= total; k += k < 300 ? 1 : 31, ++n) {
      flash = keep;
      cut = k;
      wipe();
      cut = -1;
      probe(1 * MB, &pr);
      CHECK(pr.layout == RMP_LAYOUT_FOREIGN && pr.why == RMP_WHY_LARGER);
    }
    flash = keep;
    wipe();
    probe(1 * MB, &pr);
    CHECK(pr.layout == RMP_LAYOUT_FOREIGN && pr.why == RMP_WHY_LARGER);   // formatted, the pair not erased yet
    uint8_t *b0 = top() - 4 * MB, *b1 = b0 + BS;
    memset(b0, 0xFF, BS);
    probe(1 * MB, &pr);
    CHECK(pr.layout == RMP_LAYOUT_FOREIGN && pr.why == RMP_WHY_LARGER);   // one block of the pair erased
    memset(b1, 0xFF, BS);
    probe(1 * MB, &pr);
    CHECK(pr.layout == RMP_LAYOUT_OURS);
    printf("interrupted wipe: %d cuts of %ld bytes, all refused\n", n, total);
  }
  printf("store probe ok\n");
  return 0;
}

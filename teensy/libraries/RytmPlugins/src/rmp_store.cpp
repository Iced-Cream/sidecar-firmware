// rmp_store.cpp - see rmp_store.h (flash-stall warning, limits, error mapping).
#include "rmp_store.h"
#include <LittleFS.h>
#include <string.h>
#include <stdio.h>
#include <Arduino.h>
#include "rmp_guard.h"
#include "rmp_store_probe.h"
// Teensy 4.1 program flash as the core's LittleFS_Program uses it (LittleFS.cpp's FLASH_SIZE / SECTOR_SIZE).
#if !defined(ARDUINO_TEENSY41)
#error "rmp_store: the plugin store's layout is the Teensy 4.1's"
#endif
#define RMP_FLASH_AREA 0x7C0000u
#define RMP_FLASH_TOP (0x60000000u + RMP_FLASH_AREA)
#define RMP_SECTOR 65536u
extern unsigned long _flashimagelen;
extern "C" void eepromemu_flash_write(void *addr, const void *data, uint32_t len);   // the core's eeprom.c (from RAM,
extern "C" void eepromemu_flash_erase_64K_block(void *addr);                         // interrupts masked)
RMP_COLD __attribute__((weak)) void rmp_store_on_erase() {}
// LittleFS_Program's file API on our own configuration of the same region and the same flash routines: mounted
// only when rmp_store_probe found our own superblock, formatted only when it found the region erased (the core's
// begin() formats whenever a mount fails, and LittleFS 2.4's mount does not check block_count). lfs_t and the
// config are LittleFS's protected members: the metadata log bound and the read path and listing on lfs_* directly
// (no stat before each open, no open of each directory entry).
class RmpFS : public LittleFS_Program {
public:
  lfs_t *raw() { return &lfs; }
  lfs_config *cfg() { return &config; }
  // LittleFS_Program::begin's configuration (its size rounding and image check) with config.metadata_max set
  // (LittleFS 2.4 has it; the core leaves it 0, a whole block). The Teensy 4.1's blocks are 64 KB, so a
  // directory's commit log grew to tens of KB before it was compacted, and every lookup scans it: an open took
  // 2.2-5.4 ms, a directory entry 1.1 ms, usedSize 6.9 ms (measured 2026-10-10 on the 9-package store), 0.1 ms /
  // 10 us / 0.2 ms once compacted. Bounded, a commit past the bound compacts (an erase of one 64 KB block).
  bool configure(uint32_t size, uint32_t meta_max) {
    if (mounted) lfs_unmount(&lfs);
    configured = mounted = false;
    size = (size + 0xFFFF) & 0xFFFF0000;
    uint32_t image = (uint32_t)&_flashimagelen;
    if (!size || image >= RMP_FLASH_AREA || size > RMP_FLASH_AREA - image) return false;
    base = RMP_FLASH_TOP - size;
    memset(&lfs, 0, sizeof lfs);
    memset(&config, 0, sizeof config);
    config.read = rd; config.prog = pg; config.erase = er; config.sync = sy;
    config.read_size = 128; config.prog_size = 128; config.block_size = RMP_SECTOR; config.block_count = size / RMP_SECTOR;
    config.block_cycles = 800; config.cache_size = 128; config.lookahead_size = 128; config.name_max = LFS_NAME_MAX;
    config.metadata_max = meta_max;
    configured = true;
    return true;
  }
  bool mount() { mounted = configured && lfs_mount(&lfs, &config) >= 0; return mounted; }
  bool format_mount() {
    if (!configured) return false;
    if (mounted) lfs_unmount(&lfs);
    mounted = false;
    rmp_guard_feed();   // (each erase is fed too: er)
    return lfs_format(&lfs, &config) >= 0 && mount();
  }
private:
  static uint32_t base;
  static int rd(const struct lfs_config *, lfs_block_t b, lfs_off_t off, void *buf, lfs_size_t n) {
    memcpy(buf, (const void *)(base + b * RMP_SECTOR + off), n);
    return 0;
  }
  static int pg(const struct lfs_config *, lfs_block_t b, lfs_off_t off, const void *buf, lfs_size_t n) {
    eepromemu_flash_write((void *)(base + b * RMP_SECTOR + off), buf, n);
    return 0;
  }
  // Every block erase feeds the watchdog first: a 64 KB erase takes ~0.25-0.28 s (measured) with interrupts masked
  // (the core's eeprom.c, from RAM), far more at the flash's datasheet maximum, and one LittleFS operation may erase
  // more than one block (a file's new block, then a compaction) - each gets the whole RYTM_WDOG_MS (2 s) to itself.
  // rmp_store_on_erase (weak; voiceloop's keeps its cycle-counter clock) runs first too. (From flash, before the erase.)
  static int er(const struct lfs_config *, lfs_block_t b) {
    rmp_guard_feed();
    rmp_store_on_erase();
    eepromemu_flash_erase_64K_block((void *)(base + b * RMP_SECTOR));
    return 0;
  }
  static int sy(const struct lfs_config *) { return 0; }
};
uint32_t RmpFS::base;
static RmpFS fs;
static rmp_store_stats_t stats;
// the streaming access's one-entry handle cache (see rd_get below); malloc'd, as the File wrapper's were
static lfs_file_t *rd;
static char rd_id[32];
static void rd_drop() {
  if (rd) { lfs_file_close(fs.raw(), rd); free(rd); rd = nullptr; }
  rd_id[0] = 0;
}

static bool mounted;
static File inst;
static uint32_t inst_total, inst_done;
static const char *TMP = "/machines/.install";

// An id becomes a file name: it must stay inside /machines (the rule is rmp_id_ok, shared with the slot map).
static bool id_ok(const char *id) { return rmp_id_ok(id); }
static bool id_ok_pkg(const char *id32) { return memchr(id32, 0, 32) && id_ok(id32); }

RMP_COLD void rmp_store_stats_reset() { memset(&stats, 0, sizeof stats); }
RMP_COLD rmp_store_stats_t rmp_store_stats() { return stats; }
// The store sizes builds have used (rmp_store.h): each one's base is probed (rmp_store_probe).
static const uint32_t known_sizes[] = {RMP_STORE_KNOWN_SIZES};
static rmp_probe probe;
static bool probed;
RMP_COLD static uint32_t flash_room() { return RMP_FLASH_AREA - (uint32_t)&_flashimagelen; }
RMP_COLD static void feed() { rmp_guard_feed(); }
RMP_COLD static bool finish_mount() {
  if (!fs.exists("/machines") && !fs.mkdir("/machines")) return false;
  fs.remove(TMP);   // a leftover from an install that never committed (power loss, reset)
  return true;
}
RMP_COLD bool rmp_store_begin() {
  rd_drop();
  rmp_store_iter_end();
  if (inst) inst.close();
  inst_total = inst_done = 0;
  if (!mounted) {
    if (!fs.configure(RMP_STORE_BYTES, RMP_STORE_META_MAX)) return false;
    rmp_store_probe((const uint8_t *)RMP_FLASH_TOP, flash_room(), RMP_STORE_BYTES, RMP_SECTOR, known_sizes,
                    (int)(sizeof known_sizes / sizeof *known_sizes), feed, &probe);
    probed = true;
    if (probe.layout == RMP_LAYOUT_OURS) mounted = fs.mount();              // a failed mount stays unmounted
    else if (probe.layout == RMP_LAYOUT_BLANK) mounted = fs.format_mount(); // first boot: the region erased
    // FOREIGN: neither mounted nor formatted (rmp_store_wipe)
  }
  if (!mounted) return false;
  if (!finish_mount()) { mounted = false; return false; }
  return true;
}
RMP_COLD bool rmp_store_foreign() { return probed && probe.layout == RMP_LAYOUT_FOREIGN; }
RMP_COLD void rmp_store_describe(char *buf, uint32_t n) {
  if (!probed) { RMP_SNPRINTF(buf, n, PSTR("not probed")); return; }
  const char *why = PSTR("");
  if (probe.why == RMP_WHY_LARGER) why = PSTR(" (a larger store's metadata)");
  else if (probe.why == RMP_WHY_MISMATCH) why = PSTR(" (our superblock, another layout)");
  else if (probe.why == RMP_WHY_NOT_BLANK) why = PSTR(" (no superblock, not erased)");
  uint32_t k = RMP_SNPRINTF(buf, n, PSTR("%s%s"), probe.layout == RMP_LAYOUT_OURS ? PSTR("ours") :
                            probe.layout == RMP_LAYOUT_BLANK ? PSTR("blank") : PSTR("FOREIGN"), why);
  for (int i = 0; i < probe.n && k < n; ++i) {
    const rmp_probe_at &a = probe.at[i];
    uint32_t at = RMP_FLASH_TOP - a.bytes;
    if (a.sb.found)
      k += RMP_SNPRINTF(buf + k, n - k, PSTR("; %luK@%08lx lfs%lu.%lu %lux%luK"), (unsigned long)(a.bytes >> 10),
                        (unsigned long)at, (unsigned long)(a.sb.version >> 16), (unsigned long)(a.sb.version & 0xffff),
                        (unsigned long)a.sb.block_count, (unsigned long)(a.sb.block_size >> 10));
    else
      k += RMP_SNPRINTF(buf + k, n - k, PSTR("; %luK@%08lx %s"), (unsigned long)(a.bytes >> 10), (unsigned long)at,
                        a.valid ? PSTR("commit, no superblock") : PSTR("none"));
  }
}
RMP_COLD bool rmp_store_wipe() {
  rd_drop();
  rmp_store_iter_end();
  if (inst) inst.close();
  inst_total = inst_done = 0;
  mounted = false;
  if (!fs.configure(RMP_STORE_BYTES, RMP_STORE_META_MAX)) return false;
  // Ours formatted first, then a larger known store's superblock pair erased: it lies below our base, outside our
  // region, and every later boot would find it again (a smaller store's pair lies inside ours: our format and files
  // overwrite or ignore it). In this order a wipe cut short (power, reset) still finds the larger store and refuses
  // until wiped again (extras/store_probe_test.cpp); erasing first, a cut format would leave neither store.
  mounted = fs.format_mount() && finish_mount();
  if (!mounted) return false;
  uint32_t room = flash_room();
  for (uint32_t k : known_sizes) {
    if (k <= RMP_STORE_BYTES || k > room) continue;
    for (uint32_t b = 0; b < 2; ++b) {
      uint8_t *p = (uint8_t *)(RMP_FLASH_TOP - k + b * RMP_SECTOR);
      bool erased = true;
      for (uint32_t i = 0; i < RMP_SECTOR / 4 && erased; ++i) erased = ((const uint32_t *)p)[i] == 0xFFFFFFFFu;
      if (erased) continue;
      rmp_guard_feed();
      rmp_store_on_erase();
      eepromemu_flash_erase_64K_block(p);
    }
  }
  probe.layout = RMP_LAYOUT_OURS;
  probe.why = RMP_WHY_NONE;
  return true;
}
RMP_COLD bool rmp_store_pending() { return mounted && fs.exists(TMP); }
RMP_COLD bool rmp_store_install_begin(uint32_t total) {
  rd_drop();
  if (!mounted || total < RMP_HEADER_BYTES || total > RMP_STORE_MAX_PKG) return false;
  if (inst) inst.close();
  fs.remove(TMP);
  inst = fs.open(TMP, FILE_WRITE);
  inst_total = total; inst_done = 0;
  return (bool)inst;
}
RMP_COLD bool rmp_store_install_write(const uint8_t *p, uint32_t n) {
  if (!inst || n > inst_total - inst_done) return false;   // overflow-safe; a refused chunk leaves the install open
  if (inst.write(p, n) != n) { rmp_store_install_abort(); return false; }   // a failed write aborts the install
  inst_done += n; return true;
}
RMP_COLD void rmp_store_install_abort() { rd_drop(); if (inst) inst.close(); fs.remove(TMP); inst_total = inst_done = 0; }
RMP_COLD rmp_status rmp_store_install_commit(char *id_out) {
  if (!inst || inst_done != inst_total) { rmp_store_install_abort(); return RMP_ERR_SIZE; }
  inst.close();
  // streaming verify from flash: the header, then the body CRC'd in 512-byte chunks (no malloc)
  File f = fs.open(TMP, FILE_READ);
  if (!f) { rmp_store_install_abort(); return RMP_ERR_SIZE; }
  uint32_t n = f.size();
  uint8_t chunk[512];
  rmp_info info; rmp_status st = RMP_ERR_SIZE;
  if (n == inst_total && f.read(chunk, RMP_HEADER_BYTES) == (int)RMP_HEADER_BYTES) {
    st = rmp_verify_header(chunk, n, &info);
    if (st == RMP_OK && !id_ok_pkg(info.id)) st = RMP_ERR_FORMAT;
    if (st == RMP_OK) {
      uint32_t crc = 0xFFFFFFFFu;
      for (uint32_t left = n - RMP_HEADER_BYTES; left;) {
        uint32_t k = left < sizeof chunk ? left : sizeof chunk;
        if ((size_t)f.read(chunk, k) != (size_t)k) { st = RMP_ERR_SIZE; break; }
        crc = rmp_crc32_update(crc, chunk, k); left -= k;
      }
      if (st == RMP_OK && ~crc != info.crc32) st = RMP_ERR_CRC;
    }
  }
  f.close();
  if (st != RMP_OK) { rmp_store_install_abort(); return st; }
  char path[64]; RMP_SNPRINTF(path, sizeof path, "/machines/%s.rmp", info.id);
  rd_drop();   // the old file is replaced
  // no remove first: lfs_rename replaces an existing file atomically in one commit
  if (!fs.rename(TMP, path)) { rmp_store_install_abort(); return RMP_ERR_SIZE; }
  inst_total = inst_done = 0;
  memcpy(id_out, info.id, 32);
  return RMP_OK;
}
RMP_COLD bool rmp_store_remove(const char *id) {
  rd_drop();
  if (!mounted || !id_ok(id)) return false;
  char path[64]; RMP_SNPRINTF(path, sizeof path, "/machines/%s.rmp", id);
  return fs.remove(path);
}
RMP_COLD bool rmp_store_read(const char *id, uint8_t *buf, uint32_t *len) {
  if (!mounted || !id_ok(id)) return false;
  char path[64]; RMP_SNPRINTF(path, sizeof path, "/machines/%s.rmp", id);
  File f = fs.open(path, FILE_READ);
  if (!f) return false;
  uint32_t n = f.size();
  if (n > *len) { *len = n; f.close(); return false; }   // too small: *len = the size needed
  *len = (uint32_t)f.read(buf, n); f.close();
  return *len == n;
}
// One-entry handle cache for the streaming access: opening a file in the program-flash
// LittleFS costs 0.1-0.2 ms with the metadata log bounded (2.2-5.4 ms before; a read of 512 B
// ~30 us; measured on the Teensy 4.1), and a load used to open the package once per chunk. The handle is kept for the last id used and
// dropped on anything that changes the store (install begin/abort/commit, remove,
// begin) and by rmp_store_release().
RMP_COLD bool rmp_store_space(uint32_t *total, uint32_t *used) {
  if (!mounted) return false;
  *total = (uint32_t)fs.totalSize(); *used = (uint32_t)fs.usedSize();   // usedSize walks the file system: a few ms
  return true;
}
RMP_COLD void rmp_store_release() { rd_drop(); }
// lfs_file_open straight (LittleFS::open stats the path first: two lookups instead of one).
static bool rd_get(const char *id) {
  if (!mounted || !id_ok(id)) return false;
  if (rd && !strncmp(rd_id, id, 32)) return true;
  rd_drop();
  char path[64]; RMP_SNPRINTF(path, sizeof path, "/machines/%s.rmp", id);
  uint32_t t0 = micros();
  rd = (lfs_file_t *)malloc(sizeof *rd);
  if (rd && lfs_file_open(fs.raw(), rd, path, LFS_O_RDONLY) < 0) { free(rd); rd = nullptr; }
  stats.us_open += micros() - t0; ++stats.opens;
  if (!rd) return false;
  strncpy(rd_id, id, 31); rd_id[31] = 0;
  return true;
}
RMP_COLD bool rmp_store_size(const char *id, uint32_t *len) {
  if (!rd_get(id)) return false;
  lfs_soff_t n = lfs_file_size(fs.raw(), rd);   // negative: an LFS error code
  if (n < 0) return false;
  *len = (uint32_t)n;
  return true;
}
RMP_COLD bool rmp_store_read_at(const char *id, uint32_t off, uint8_t *buf, uint32_t n) {
  if (!rd_get(id)) return false;
  uint32_t t1 = micros();
  lfs_t *L = fs.raw();
  lfs_soff_t size = lfs_file_size(L, rd);   // negative: an LFS error code
  bool ok = size >= 0 && (uint64_t)off + n <= (uint64_t)size && lfs_file_seek(L, rd, (lfs_soff_t)off, LFS_SEEK_SET) >= 0 &&
            lfs_file_read(L, rd, buf, n) == (lfs_ssize_t)n;
  stats.us_read += micros() - t1; ++stats.reads; stats.bytes += n;
  return ok;
}
// The same listing one entry a call, for callers that must not stall audio: lfs_dir_read gives the name,
// type and size (File::openNextFile opened every entry as well: 3.5 ms an entry before the log bound).
static lfs_dir_t *lit;   // malloc'd, as the File wrapper's was
RMP_COLD void rmp_store_iter_end() { if (lit) { lfs_dir_close(fs.raw(), lit); free(lit); lit = nullptr; } }
RMP_COLD bool rmp_store_iter_begin() {
  rmp_store_iter_end();
  if (!mounted) return false;
  lit = (lfs_dir_t *)malloc(sizeof *lit);
  if (lit && lfs_dir_open(fs.raw(), lit, "/machines") < 0) { free(lit); lit = nullptr; }
  return lit != nullptr;
}
// A package entry: a file <id>.rmp, the id not starting with '.' (the install file).
static bool entry_id(const struct lfs_info &e, char *id) {
  size_t l = strlen(e.name);
  if (e.type != LFS_TYPE_REG || e.name[0] == '.' || l <= 4 || strcmp(e.name + l - 4, ".rmp")) return false;
  memset(id, 0, 32); memcpy(id, e.name, l - 4 < 31 ? l - 4 : 31);
  return true;
}
RMP_COLD bool rmp_store_iter_next(char *id, uint32_t *size) {
  if (!lit) return false;
  struct lfs_info e;
  while (lfs_dir_read(fs.raw(), lit, &e) > 0)
    if (entry_id(e, id)) { *size = e.size; return true; }
  rmp_store_iter_end();
  return false;
}
RMP_COLD void rmp_store_list(void (*cb)(const char *, uint32_t, void *), void *ctx) {
  if (!mounted) return;
  lfs_dir_t d;
  if (lfs_dir_open(fs.raw(), &d, "/machines") < 0) return;
  struct lfs_info e;
  char id[32];
  while (lfs_dir_read(fs.raw(), &d, &e) > 0)
    if (entry_id(e, id)) cb(id, e.size, ctx);
  lfs_dir_close(fs.raw(), &d);
}

// teensy/platform/platform_teensy.cpp - the Teensy's platform functions (core/platform.h) and the card's
// fingerprint: every raw 32-byte entry below the folder, as card_index.cpp hashed it before the core (FAT32:
// short-name entries; exFAT: whole entry sets), so a card's stored index stays valid. Cold: program flash.
#include <stdarg.h>

#include "platform.h"

extern "C" uint8_t external_psram_size;   // MB, found by the core at boot (startup.c)

namespace core {

static void (*g_sink)(const char*, int) = nullptr;
void set_log_sink(void (*sink)(const char*, int)) { g_sink = sink; }

uint32_t cycles_per_second() { return F_CPU_ACTUAL; }
FLASHMEM uint32_t random_seed() { return ARM_DWT_CYCCNT ^ micros(); }

FLASHMEM void log(const char* fmt, ...) {   // cold: boot, reports and error paths only (none per sample)
  char b[200];
  va_list ap;
  va_start(ap, fmt);
  int n = vsniprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  if (n > 199) n = 199;
  if (g_sink) {
    g_sink(b, n);
  } else {
    Serial.write((const uint8_t*)b, n);
    Serial.write('\n');
  }
}

FLASHMEM uint32_t sample_ram_total() { return (uint32_t)external_psram_size << 20; }
// extmem_malloc falls back to the heap (RAM2) when PSRAM cannot take it: only PSRAM (0x7000_0000..) will do.
FLASHMEM void* sample_ram_alloc(uint32_t bytes) {
  void* m = extmem_malloc(bytes);
  if (m && ((uint32_t)m >> 28) != 7) {
    extmem_free(m);
    return nullptr;
  }
  return m;
}
FLASHMEM void sample_ram_free(void* p) { extmem_free(p); }

FLASHMEM bool Fs::begin() { return SdFs::begin(SdioConfig(FIFO_SDIO)); }

namespace {
FLASHMEM void fnv(uint64_t* h, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; ++i) {
    *h ^= b[i];
    *h *= 0x100000001b3ull;
  }
}
FLASHMEM uint16_t le16(const uint8_t* p) { return p[0] | p[1] << 8; }

// FAT32: every raw 32-byte entry (short name, attributes, times, first cluster, size), long-name parts and
// deleted entries skipped. Only folders are opened.
FLASHMEM void walk_raw(FsFile& dir, int depth, uint64_t* h) {
  DirFat_t d;
  dir.rewind();
  for (;;) {
    uint32_t pos = dir.curPosition();
    if (dir.read(&d, sizeof d) != (int)sizeof d) break;
    if (d.name[0] == 0) break;
    if (d.name[0] == 0xe5) continue;
    if ((d.attributes & 0x0f) == 0x0f) continue;
    if (d.name[0] == '.') continue;
    fnv(h, &d, sizeof d);
    if ((d.attributes & 0x10) && depth < 12) {
      FsFile sub;
      if (sub.open(&dir, pos / 32, O_RDONLY)) {
        walk_raw(sub, depth + 1, h);
        sub.close();
      }
      dir.seekSet(pos + 32);
    }
  }
}

// exFAT: the raw entries a sector at a time; each in-use entry set (File 0x85, Stream 0xC0, Name 0xC1 parts)
// hashed whole unless its name starts with '.'; folders entered by their File entry's index.
FLASHMEM void walk_exfat(FsFile& dir, int depth, uint64_t* h, uint32_t* entries, uint32_t* hidden) {
  uint8_t sec[512], set[19 * 32];
  int need = 0, have = 0;
  uint32_t set_index = 0, pos = 0;
  dir.rewind();
  for (;;) {
    int got = dir.read(sec, sizeof sec);
    if (got < 32) return;
    for (int k = 0; k + 32 <= got; k += 32, pos += 32) {
      const uint8_t* e = sec + k;
      if (e[0] == 0x00) return;
      if (!(e[0] & 0x80)) {
        need = 0;
        continue;
      }
      if (e[0] == 0x85) {
        need = 1 + e[1];
        have = 0;
        set_index = pos / 32;
        if (need > 19) need = 0;
      }
      if (!need) continue;
      memcpy(set + 32 * have++, e, 32);
      if (have < need) continue;
      bool hide = need >= 3 && set[64] == 0xc1 && set[66] == '.' && set[67] == 0;
      bool is_dir = le16(set + 4) & 0x10;
      need = 0;
      if (hide) {
        ++*hidden;
        continue;
      }
      fnv(h, set, 32 * have);
      ++*entries;
      if (is_dir && depth < 12) {
        uint32_t next = pos + 32;
        FsFile sub;
        if (sub.open(&dir, set_index, O_RDONLY)) {
          walk_exfat(sub, depth + 1, h, entries, hidden);
          sub.close();
        }
        if (!dir.seekSet(next)) return;
        got = dir.read(sec, sizeof sec);
        if (got < 32) return;
        k = -32;
        pos = next - 32;
      }
    }
  }
}
}  // namespace

FLASHMEM uint64_t Fs::tree_hash(const char* path, uint32_t* entries, uint32_t* hidden) {
  *entries = *hidden = 0;
  FsFile root;
  uint64_t h = 0xcbf29ce484222325ull;
  if (!root.open(path, O_RDONLY)) return 0;
  if (fatType() == FAT_TYPE_EXFAT) walk_exfat(root, 0, &h, entries, hidden);
  else walk_raw(root, 0, &h);
  root.close();
  return h;
}

}  // namespace core

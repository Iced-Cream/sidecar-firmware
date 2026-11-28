// core/bundle/pack.cpp - see pack.h.
#include "pack.h"

#include <string.h>

#include "../platform.h"

namespace core {
namespace bundle {

namespace {
bool say(char* err, int cap, const char* what, const char* name = nullptr) {
  if (name) sniprintf(err, cap, "bundle: %s (%s)", what, name);
  else sniprintf(err, cap, "bundle: %s", what);
  return false;
}
uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
}  // namespace

bool store_entry_name(const char* f, char* out, int cap) {
  if (!f || strchr(f, '/')) return false;
  int n = sniprintf(out, cap, "store/%s", f);
  return n > 0 && n < cap && name_ok(out);
}
bool sample_entry_name(const char* p, char* out, int cap) {
  static const char kSamples[] = "/Samples/";
  const size_t k = sizeof kSamples - 1;
  if (!p || strncmp(p, kSamples, k)) return false;
  int n = sniprintf(out, cap, "samples/%s", p + k);
  return n > 0 && n < cap && name_ok(out);
}

bool plan(const Manifest& m, uint32_t mlen, uint32_t mcrc, uint32_t epoch, ZEntry* z, int cap, Layout* L) {
  if (m.ne > kEntriesMax || m.ne + 1 > cap) return false;
  memset(&z[0], 0, sizeof z[0]);
  strcpy(z[0].name, "manifest.json");
  z[0].size = mlen, z[0].crc = mcrc;
  for (int i = 0; i < m.ne; ++i) {
    ZEntry& e = z[i + 1];
    memset(&e, 0, sizeof e);
    sniprintf(e.name, sizeof e.name, "%s", m.e[i].name);
    e.size = m.e[i].size, e.crc = m.e[i].crc;
  }
  uint16_t d, t;
  dos_time(epoch, &d, &t);
  return layout(z, m.ne + 1, d, t, L);
}

bool sniff_zip(const uint8_t* h, uint32_t n, uint64_t* off, uint32_t* size) {
  static const char kName[] = "manifest.json";
  const uint32_t nl0 = sizeof kName - 1;
  if (n < 30 || get32(h) != 0x04034b50u) return false;
  if (get16(h + 6) & 0x0009 || get16(h + 8) != 0) return false;   // encrypted, a data descriptor, compressed
  const uint32_t nl = get16(h + 26), xl = get16(h + 28);
  if (nl != nl0 || 30 + nl > n || memcmp(h + 30, kName, nl0)) return false;
  const uint32_t sz = get32(h + 18);
  if (get32(h + 22) != sz || sz > kManifestMax) return false;
  *off = 30 + nl + xl, *size = sz;
  return true;
}

bool check(Manifest* m, const ZEntry* z, int nz, char* err, int cap) {
  if (nz < 1 || strcmp(z[0].name, "manifest.json")) return say(err, cap, "the first entry is not manifest.json");
  if (nz != m->ne + 1) return say(err, cap, nz > m->ne + 1 ? "an entry the manifest does not list" : "an entry the manifest lists is missing");
  for (int i = 0; i < m->ne; ++i) {
    MEntry& e = m->e[i];
    const ZEntry& x = z[i + 1];
    if (strcmp(e.name, x.name)) return say(err, cap, "entries differ from the manifest (name or order)", x.name);
    if (!in_kind_dir(e.name, e.kind)) return say(err, cap, "an entry outside its kind's folder", e.name);
    if (e.size != x.size) return say(err, cap, "an entry's size differs from the manifest", e.name);
    e.crc = x.crc;
  }
  return true;
}

bool EntryHash::match(const MEntry& e, char* err, int cap) {
  uint8_t d[32];
  s.final(d);
  if (n != e.size) return say(err, cap, "an entry's size differs", e.name);
  if (crc != e.crc) return say(err, cap, "an entry's CRC differs", e.name);
  if (memcmp(d, e.sha, 32)) return say(err, cap, "an entry's sha256 differs", e.name);
  return true;
}

}  // namespace bundle
}  // namespace core

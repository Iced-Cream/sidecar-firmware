// rmp_manifest.cpp - see rmp_manifest.h.
#include "rmp_manifest.h"
#include <string.h>

// n bytes of a NUL-padded field, printable ASCII ('_' otherwise); everything after the first NUL is NUL;
// dst[n] = 0 (dst holds n + 1 bytes).
RMP_COLD static void clean(char *dst, const uint8_t *src, int n) {
  dst[n] = 0;
  bool end = false;
  for (int i = 0; i < n; ++i) {
    uint8_t c = src[i];
    if (!c) end = true;
    dst[i] = end ? 0 : (c >= 0x20 && c <= 0x7e) ? (char)c : '_';
  }
}
RMP_COLD static uint8_t sat127(uint8_t v) { return v > 127 ? 127 : v; }
RMP_COLD static bool alnum(uint8_t c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

RMP_COLD int rmp_desc_find(const rmp_desc *d, int p) {
  for (int j = 0; j < d->nknobs; ++j)
    if (d->knob[j].p == p) return j;
  return -1;
}

RMP_COLD bool rmp_manifest_read(const rmp_info *info, rmp_read_fn rd, void *ctx, rmp_desc *d) {
  memset(d, 0, sizeof *d);
  d->pre = -1;
  int fmt = info->format;
  if (fmt < 1 || fmt > 3) return false;
  uint32_t hb = fmt >= 3 ? 84u : 64u, kb = fmt == 1 ? 20u : fmt == 2 ? 22u : 26u;
  uint64_t at64 = (uint64_t)RMP_HEADER_BYTES + info->image_size + 8ull * info->reloc_count;
  if (info->manifest_size < hb || at64 + info->manifest_size > 0xFFFFFFFFull) return false;
  uint32_t at = (uint32_t)at64;
  uint8_t h[84];
  if (!rd(ctx, at, h, hb)) return false;
  uint32_t n = h[63];
  if (n > 32 || hb + n * kb > info->manifest_size) return false;
  clean(d->name, h, 10);
  clean(d->short_name, h + 10, 4);
  d->page2 = h[62] ? 1 : 0;
  d->format = (uint8_t)fmt;
  if (fmt >= 3) {
    clean(d->p2_title, h + 64, 13);
    int e = 0;
    while (e < 7 && alnum(h[77 + e])) ++e;
    bool ok = e > 0 && (e == 7 || h[77 + e] == 0);   // letters and digits up to the NUL padding
    if (ok) memcpy(d->preset_ext, h + 77, (size_t)e);
  }
  if (fmt >= 2) {
    d->names_at = at + hb + n * kb;
    d->names_end = at + info->manifest_size;
  }
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t k[26];
    if (!rd(ctx, at + hb + i * kb, k, kb)) return false;
    rmp_desc_knob q;
    memset(&q, 0, sizeof q);
    q.p = k[0];
    q.look = k[1] <= RMP_LOOK_LIST ? k[1] : RMP_LOOK_DIAL;
    q.kind = k[2] <= RMP_TEXT_POOL ? k[2] : RMP_TEXT_NUMBER;
    q.min = 0; q.max = 127; q.def = 64;
    const uint8_t *s;
    if (fmt == 1) {
      s = k + 4;
    } else if (fmt == 2) {
      q.enum_first = k[3]; q.enum_count = k[4];
      s = k + 6;
    } else {
      q.enum_first = k[3]; q.enum_count = k[4];
      q.role = k[5] <= RMP_ROLE_OFFSET ? k[5] : RMP_ROLE_FREE;
      uint8_t lo = sat127(k[6]), hi = sat127(k[7]), df = sat127(k[8]);
      if (lo <= hi) { q.min = lo; q.max = hi; q.def = df < lo ? lo : df > hi ? hi : df; }
      s = k + 10;
    }
    if (q.enum_count > RMP_ENUM_MAX) q.enum_count = RMP_ENUM_MAX;
    clean(q.long_name, s, 12);
    clean(q.short_name, s + 12, 4);
    if (q.p < 1 || q.p > 16 || rmp_desc_find(d, q.p) >= 0 || d->nknobs >= RMP_DESC_KNOBS) continue;
    int j = d->nknobs++;
    while (j > 0 && d->knob[j - 1].p > q.p) { d->knob[j] = d->knob[j - 1]; --j; }   // keep p order
    d->knob[j] = q;
  }
  for (int j = 0; j < d->nknobs; ++j)
    if (d->knob[j].kind == RMP_TEXT_PRESET) { d->pre = (int8_t)j; break; }
  return true;
}

RMP_COLD int rmp_manifest_enum(const rmp_desc *d, int k, rmp_read_fn rd, void *ctx, char names[16][8]) {
  memset(names, 0, 16 * 8);
  if (k < 0 || k >= d->nknobs || !d->names_at) return 0;
  const rmp_desc_knob &q = d->knob[k];
  if (!q.enum_count) return 0;
  uint64_t from = (uint64_t)d->names_at + 8ull * q.enum_first, to = from + 8ull * q.enum_count;
  if (to > d->names_end) return -1;
  uint8_t b[16 * 8];
  if (!rd(ctx, (uint32_t)from, b, 8u * q.enum_count)) return -1;
  for (int i = 0; i < q.enum_count; ++i) {
    clean(names[i], b + 8 * i, 7);   // the 8th byte is the terminator
  }
  return q.enum_count;
}

RMP_COLD void rmp_desc_from_id(const char *id, rmp_desc *d) {
  memset(d, 0, sizeof *d);
  d->pre = -1;
  const char *last = strrchr(id, '.');
  const char *p = last && last[1] ? last + 1 : id;
  int n = 0;
  for (; p[n] && n < 10; ++n) {
    char c = p[n];
    d->name[n] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (c >= 0x20 && c <= 0x7e) ? c : '_';
  }
  memcpy(d->short_name, d->name, n < 4 ? (size_t)n : 4u);
}

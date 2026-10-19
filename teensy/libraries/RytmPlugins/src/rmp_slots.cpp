// rmp_slots.cpp - see rmp_slots.h.
#include "rmp_slots.h"
#include <stdio.h>
#include <string.h>
#include "rmp_api.h"

static bool is_ws(char c) { return c == ' ' || c == '\t'; }
// 1..max_digits decimal digits at *p, no leading zero (except "0" itself); advances *p.
RMP_COLD static bool digits(const char **p, const char *end, uint32_t max_digits, uint32_t *v) {
  const char *s = *p; uint32_t n = 0, x = 0;
  while (*p < end && **p >= '0' && **p <= '9') { if (++n > max_digits) return false; x = x * 10 + (uint32_t)(**p - '0'); ++*p; }
  *v = x; return n > 0 && !(n > 1 && *s == '0');
}
// One line [p, end) without its '\n'. Returns -1 bad, 0 blank, 1 parsed into *e / *slot.
RMP_COLD static int parse_line(const char *p, const char *end, rmp_slot_entry *e, int *slot) {
  if (end > p && end[-1] == '\r') --end;
  while (p < end && is_ws(*p)) ++p;
  while (end > p && is_ws(end[-1])) --end;
  if (p == end) return 0;
  uint32_t n, a, b, c;
  if (*p++ != 'U' || !digits(&p, end, 1, &n) || n < 1 || n > RMP_SLOTS) return -1;
  if (p == end || !is_ws(*p)) return -1;
  while (p < end && is_ws(*p)) ++p;
  const char *id = p;
  while (p < end && !is_ws(*p)) ++p;
  size_t idl = (size_t)(p - id);
  if (idl == 0 || idl > 31) return -1;
  if (memchr(id, 0, idl)) return -1;   // a NUL inside the token (rmp_id_ok would only see its head)
  char idbuf[32] = {0}; memcpy(idbuf, id, idl);
  if (!rmp_id_ok(idbuf)) return -1;
  if (p == end) return -1;
  while (p < end && is_ws(*p)) ++p;
  if (!digits(&p, end, 3, &a) || p == end || *p++ != '.' || !digits(&p, end, 3, &b) || p == end || *p++ != '.' ||
      !digits(&p, end, 3, &c) || p != end || a > 255 || b > 255 || c > 255) return -1;
  memcpy(e->id, idbuf, 32); e->version = a << 16 | b << 8 | c;
  *slot = (int)n - 1;
  return 1;
}
RMP_COLD bool rmp_slots_parse(const char *txt, uint32_t len, rmp_slot_map *m) {
  memset(m, 0, sizeof *m);
  const char *p = txt, *end = txt + len;
  while (p < end) {
    const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
    const char *le = nl ? nl : end;
    rmp_slot_entry e; int s = -1;
    int r = parse_line(p, le, &e, &s);
    if (r < 0 || (r > 0 && m->slot[s].id[0])) { memset(m, 0, sizeof *m); return false; }   // bad line or duplicate slot
    if (r > 0) m->slot[s] = e;
    p = nl ? nl + 1 : end;
  }
  return true;
}
RMP_COLD uint32_t rmp_slots_format(const rmp_slot_map *m, char *out, uint32_t cap) {
  if (!cap) return 0;
  uint32_t n = 0; out[0] = 0;
  for (int s = 0; s < RMP_SLOTS; ++s) {
    if (!m->slot[s].id[0]) continue;
    uint32_t v = m->slot[s].version;
    int w = RMP_SNPRINTF(out + n, cap - n, "U%d %.31s %u.%u.%u\n", s + 1, m->slot[s].id,
                     (unsigned)(v >> 16), (unsigned)((v >> 8) & 0xFF), (unsigned)(v & 0xFF));
    if (w < 0 || n + (uint32_t)w >= cap) { out[0] = 0; return 0; }
    n += (uint32_t)w;
  }
  return n;
}

struct rd_ctx { const rmp_platform *pf; const char *id; };
RMP_COLD static bool rd_at(void *ctx, uint32_t off, uint8_t *buf, uint32_t n) {
  rd_ctx *c = (rd_ctx *)ctx; return c->pf->read_at(c->id, off, buf, n);
}
RMP_COLD static void say(const rmp_platform *pf, int s, const char *id, const char *what) {
  if (!pf->log) return;
  char b[96]; RMP_SNPRINTF(b, sizeof b, "U%d %s: %s", s + 1, id, what); pf->log(b);
}

// One package's load as a state machine (header, CRC in chunks, place), so the Teensy can
// spread it over loop passes. A step does at most max_bytes of CRC reading, or the header,
// or the whole placement (one big read of the image plus the relocations).
RMP_COLD static void job_begin(rmp_load_job *j, const rmp_platform *pf, int slot, const char *id, uint32_t want) {
  memset(j, 0, sizeof *j);
  j->pf = pf; j->slot = slot; j->want = want; j->phase = 0;
  memcpy(j->id, id, 32);
  j->state = RMP_SLOT_LOADING;
}
RMP_COLD static bool job_step(rmp_load_job *j, uint32_t max_bytes) {
  const rmp_platform *pf = j->pf;
  uint8_t chunk[512];
  if (j->phase == 0) {
    rmp_info &info = j->info;
    if (!pf->size_of(j->id, &j->len)) { j->state = RMP_SLOT_MISSING; say(pf, j->slot, j->id, "not installed"); return true; }
    if (j->len < RMP_HEADER_BYTES || !pf->read_at(j->id, 0, chunk, RMP_HEADER_BYTES) ||
        rmp_verify_header(chunk, j->len, &info) != RMP_OK || strncmp(info.id, j->id, 32)) {
      j->state = RMP_SLOT_BAD; j->pkg_fail = true; say(pf, j->slot, j->id, "bad header"); return true;
    }
    // the saved version must be within [compatible_from, version] of the installed one
    if (j->want > info.version || j->want < info.compatible_from) { j->state = RMP_SLOT_MISSING; say(pf, j->slot, j->id, "version out of range"); return true; }
    j->off = RMP_HEADER_BYTES; j->crc = 0xFFFFFFFFu; j->phase = 1;
    return false;
  }
  if (j->phase == 1) {
    bool ok = true;
    uint32_t done = 0;
    while (ok && j->off < j->len && done < max_bytes) {
      uint32_t k = j->len - j->off < sizeof chunk ? j->len - j->off : (uint32_t)sizeof chunk;
      ok = pf->read_at(j->id, j->off, chunk, k);
      if (ok) { j->crc = rmp_crc32_update(j->crc, chunk, k); j->off += k; done += k; }
    }
    if (!ok || (j->off >= j->len && ~j->crc != j->info.crc32)) {
      j->state = RMP_SLOT_BAD; j->pkg_fail = true; say(pf, j->slot, j->id, "bad CRC"); return true;
    }
    if (j->off >= j->len) j->phase = 2;
    return false;
  }
  rmp_info &info = j->info;
  uint32_t bytes = info.image_size + info.bss_size;   // each <= 1 MB (rmp_verify_header)
  uint8_t *img = pf->arena_alloc(bytes);
  if (!img) {
    j->state = RMP_SLOT_NO_ROOM; j->pkg_fail = true; j->need = bytes;
    char b[40]; RMP_SNPRINTF(b, sizeof b, "no room (needs %lu B)", (unsigned long)bytes); say(pf, j->slot, j->id, b);
    return true;
  }
  rd_ctx c{pf, j->id};
  if (rmp_place_stream(&info, rd_at, &c, img, (uintptr_t)img) != RMP_OK) { j->state = RMP_SLOT_BAD; j->pkg_fail = true; say(pf, j->slot, j->id, "bad relocation"); return true; }
  pf->arena_sync(img, bytes);
  const rmp_plugin *p = (const rmp_plugin *)(img + info.entry_offset);
  // the entry table's api must be the header's (rmp_verify_header took it as RMP_API_MIN..RMP_API): the host
  // picks the plugin's host table by p->api, the device report and the generations by the header's
  if (p->magic != RMP_PLUGIN_MAGIC || p->api < RMP_API_MIN || p->api > RMP_API || p->api != info.api) { j->state = RMP_SLOT_BAD; j->pkg_fail = true; say(pf, j->slot, j->id, "bad entry"); return true; }
  j->state = RMP_SLOT_READY; j->plugin = p;
  // init() runs on the Teensy only, through the guard: the caller calls it
  return true;
}
RMP_COLD static rmp_loaded job_result(const rmp_load_job *j) {
  if (j->state == RMP_SLOT_READY) return rmp_loaded{RMP_SLOT_READY, j->plugin, j->info.version, j->info.compatible_from, 0};
  return rmp_loaded{j->state, nullptr, 0, 0, j->need};
}

RMP_COLD void rmp_slots_load(const rmp_platform *pf, const rmp_slot_map *m, rmp_loaded out[RMP_SLOTS]) {
  pf->arena_reset();
  bool pkg_fail[RMP_SLOTS] = {false};   // the slot's package itself is BAD or got NO_ROOM
  for (int s = 0; s < RMP_SLOTS; ++s) out[s] = rmp_loaded{RMP_SLOT_EMPTY, nullptr, 0, 0, 0};
  for (int s = 0; s < RMP_SLOTS; ++s) {
    const rmp_slot_entry &e = m->slot[s];
    if (!e.id[0]) continue;
    const uint32_t want = e.version;
    // the same id in an earlier slot already placed: share its image, check this slot's version
    // an earlier slot whose package failed as a whole (BAD / NO_ROOM): same state, no reload
    // (it would waste arena space again); a version-range MISSING is per slot and retries
    int t = 0;
    for (; t < s; ++t) if (pkg_fail[t] && !strncmp(m->slot[t].id, e.id, 32)) break;
    if (t < s) { out[s].state = out[t].state; out[s].need = out[t].need; pkg_fail[s] = true; continue; }
    for (t = 0; t < s; ++t) if (out[t].plugin && !strncmp(m->slot[t].id, e.id, 32)) break;
    if (t < s) {
      if (want > out[t].version || want < out[t].from) { out[s].state = RMP_SLOT_MISSING; say(pf, s, e.id, "version out of range"); }
      else out[s] = out[t];
      continue;
    }
    if (!memchr(e.id, 0, 32) || !rmp_id_ok(e.id)) { out[s].state = RMP_SLOT_MISSING; say(pf, s, "?", "unsafe id"); continue; }
    rmp_load_job j;
    job_begin(&j, pf, s, e.id, want);
    while (!job_step(&j, 0xFFFFFFFFu)) {}
    out[s] = job_result(&j);
    pkg_fail[s] = j.pkg_fail;
  }
}

RMP_COLD bool rmp_slots_one_begin(rmp_load_job *j, const rmp_platform *pf, const rmp_slot_map *m, int slot, rmp_loaded out[RMP_SLOTS]) {
  const rmp_slot_entry &e = m->slot[slot];
  out[slot] = rmp_loaded{RMP_SLOT_EMPTY, nullptr, 0, 0, 0};
  j->phase = -1; j->slot = slot;
  if (!e.id[0]) return true;
  if (!memchr(e.id, 0, 32) || !rmp_id_ok(e.id)) { out[slot].state = RMP_SLOT_MISSING; say(pf, slot, "?", "unsafe id"); return true; }
  for (int t = 0; t < RMP_SLOTS; ++t) {   // another slot already plays this id: share its image
    if (t == slot || out[t].state != RMP_SLOT_READY || strncmp(m->slot[t].id, e.id, 32)) continue;
    if (e.version > out[t].version || e.version < out[t].from) { out[slot].state = RMP_SLOT_MISSING; say(pf, slot, e.id, "version out of range"); }
    else out[slot] = out[t];
    return true;
  }
  job_begin(j, pf, slot, e.id, e.version);
  out[slot].state = RMP_SLOT_LOADING;
  return false;
}
RMP_COLD bool rmp_slots_one_step(rmp_load_job *j, rmp_loaded out[RMP_SLOTS], uint32_t max_bytes) {
  if (j->phase < 0) return true;   // already done (begin returned true, or a step did): nothing to run
  if (!job_step(j, max_bytes)) return false;
  out[j->slot] = job_result(j);
  j->phase = -2;
  return true;
}
RMP_COLD void rmp_slots_load_one(const rmp_platform *pf, const rmp_slot_map *m, int slot, rmp_loaded out[RMP_SLOTS]) {
  rmp_load_job j;
  if (rmp_slots_one_begin(&j, pf, m, slot, out)) return;
  while (!rmp_slots_one_step(&j, out, 0xFFFFFFFFu)) {}
}

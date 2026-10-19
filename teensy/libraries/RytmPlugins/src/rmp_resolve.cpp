// rmp_resolve.cpp - see rmp_resolve.h.
#include "rmp_resolve.h"
#include <stdio.h>
#include <string.h>
#include "rmp_loader.h"
#include "rmp_proto.h"

static const char *const k_src[] = {"none", "saved", "sounds", "inherited", "empty", "imported"};
RMP_COLD const char *rmp_src_name(int src) { return (unsigned)src < 6 ? k_src[src] : "none"; }

RMP_COLD static bool ws(char c) { return c == ' ' || c == '\t'; }
// the next space-separated token of [*p, end): its start and length (0 at the end)
RMP_COLD static uint32_t token(const char **p, const char *end, const char **t) {
  while (*p < end && ws(**p)) ++*p;
  *t = *p;
  while (*p < end && !ws(**p)) ++*p;
  return (uint32_t)(*p - *t);
}
RMP_COLD static bool number(const char *t, uint32_t n, uint32_t max, uint32_t *v) {
  if (!n || n > 5) return false;
  uint32_t x = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (t[i] < '0' || t[i] > '9') return false;
    x = x * 10 + (uint32_t)(t[i] - '0');
  }
  if (x > max) return false;
  *v = x;
  return true;
}
RMP_COLD static bool version(const char *t, uint32_t n, uint32_t *v) {
  uint32_t part[3] = {0, 0, 0}, k = 0, digits = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (t[i] == '.') { if (!digits || ++k > 2) return false; digits = 0; continue; }
    if (t[i] < '0' || t[i] > '9' || ++digits > 3) return false;
    part[k] = part[k] * 10 + (uint32_t)(t[i] - '0');
  }
  if (k != 2 || !digits || part[0] > 255 || part[1] > 255 || part[2] > 255) return false;
  *v = part[0] << 16 | part[1] << 8 | part[2];
  return true;
}
RMP_COLD static void copy_name(char *d, const char *s, uint32_t n) {   // printable ASCII, at most 10
  uint32_t k = 0;
  for (uint32_t i = 0; i < n && k < 10; ++i) d[k++] = s[i] >= 0x20 && s[i] <= 0x7e ? s[i] : '_';
  while (k && d[k - 1] == ' ') --k;
  d[k] = 0;
}

RMP_COLD bool rmp_notes_parse(const char *txt, uint32_t len, rmp_slot_notes *out) {
  memset(out, 0, sizeof *out);
  const char *p = txt, *end = txt + len;
  bool any = false;
  while (p < end) {
    const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
    const char *le = nl ? nl : end;
    if (le > p && le[-1] == '\r') --le;
    const char *q = p, *t;
    uint32_t n, s, src, flags, fp, ver = 0;
    rmp_slot_note e;
    memset(&e, 0, sizeof e);
    bool ok = (n = token(&q, le, &t)) == 2 && t[0] == 'U' && t[1] >= '1' && t[1] <= '0' + RMP_SLOTS;
    s = ok ? (uint32_t)(t[1] - '1') : 0;
    if (ok) {
      n = token(&q, le, &t);
      src = 99;
      for (uint32_t k = 0; k < 6; ++k)
        if (n == strlen(k_src[k]) && !strncmp(t, k_src[k], n)) src = k;
      ok = src < 6;
      e.src = (uint8_t)src;
    }
    if (ok) { n = token(&q, le, &t); ok = number(t, n, 255, &flags); e.flags = (uint8_t)flags; }
    if (ok) { n = token(&q, le, &t); ok = number(t, n, 16383, &fp); e.fp = (uint16_t)fp; }
    if (ok) {
      n = token(&q, le, &t);
      if (n == 1 && t[0] == '-') e.id[0] = 0;
      else if (n && n < 32) { memcpy(e.id, t, n); e.id[n] = 0; ok = rmp_id_ok(e.id); }
      else ok = false;
    }
    if (ok) {
      n = token(&q, le, &t);
      if (n == 1 && t[0] == '-') e.version = 0;
      else if ((ok = version(t, n, &ver))) e.version = ver;
    }
    if (ok) {
      while (q < le && ws(*q)) ++q;
      copy_name(e.name, q, (uint32_t)(le - q));
      out->slot[s] = e;
      any = true;
    }
    p = nl ? nl + 1 : end;
  }
  return any;
}

RMP_COLD uint32_t rmp_notes_format(const rmp_slot_notes *in, char *out, uint32_t cap) {
  if (!cap) return 0;
  uint32_t n = 0;
  out[0] = 0;
  for (int s = 0; s < RMP_SLOTS; ++s) {
    const rmp_slot_note &e = in->slot[s];
    if (e.src == RMP_SRC_NONE && !e.flags && !e.fp && !e.id[0]) continue;
    char v[12] = "-";
    if (e.id[0]) RMP_SNPRINTF(v, sizeof v, "%u.%u.%u", (unsigned)(e.version >> 16), (unsigned)(e.version >> 8 & 255), (unsigned)(e.version & 255));
    int w = RMP_SNPRINTF(out + n, cap - n, "U%d %s %u %u %s %s%s%.10s\n", s + 1, rmp_src_name(e.src), (unsigned)e.flags,
                         (unsigned)e.fp, e.id[0] ? e.id : "-", v, e.name[0] ? " " : "", e.name);
    if (w < 0 || n + (uint32_t)w >= cap) { out[0] = 0; return 0; }
    n += (uint32_t)w;
  }
  return n;
}

RMP_COLD bool rmp_parse_prints(const uint8_t *m, int len, rmp_prints *p) {
  memset(p, 0, sizeof *p);
  if (len < 10 || m[0] != 0xF0 || m[1] != 0x7D || m[2] != 0x52 || m[3] != 0x54 || m[4] != RMP_CMD_SLOT_PRINTS ||
      m[5] != 1 || m[len - 1] != 0xF7)
    return false;
  int n = m[8];
  if (n > 16 || len != 10 + 4 * n) return false;
  p->seq = m[6], p->flags = m[7], p->n = (uint8_t)n;
  for (int k = 0; k < n; ++k) {
    const uint8_t *e = m + 9 + 4 * k;
    if (e[0] < 1 || e[0] > RMP_SLOTS || (e[0] | e[1] | e[2] | e[3]) & 0x80 || !e[3]) return false;   // count 1..127
    uint16_t fp = (uint16_t)(e[1] << 7 | e[2]);
    if (!fp) return false;
    p->e[k] = rmp_print{(uint8_t)(e[0] - 1), fp, e[3]};
  }
  return true;
}

RMP_COLD uint32_t rmp_prints_hash(const rmp_prints *p) {
  uint32_t h = 2166136261u;
  h = (h ^ p->n) * 16777619u;
  h = (h ^ (p->flags & 1u)) * 16777619u;
  for (int k = 0; k < p->n; ++k) {
    h = (h ^ p->e[k].slot) * 16777619u;
    h = (h ^ p->e[k].fp) * 16777619u;
    h = (h ^ p->e[k].count) * 16777619u;
  }
  return h;
}

RMP_COLD bool rmp_resolve_open(rmp_slot_map *m, rmp_slot_notes *n, bool have_map, bool have_notes) {
  if (!have_notes) memset(n, 0, sizeof *n);
  for (int s = 0; s < RMP_SLOTS; ++s) {
    rmp_slot_note &e = n->slot[s];
    bool used = m->slot[s].id[0] != 0;
    if (e.src == RMP_SRC_EMPTY && !used) {   // explicit empty (and the map agrees)
    } else if (used) {
      // a map from an older build, a note gone or stale (an "empty" note beside a used map: the map wins), or an
      // inherited slot of an earlier load: it was saved, so it is the project's own now
      if (e.src == RMP_SRC_NONE || e.src == RMP_SRC_EMPTY || (have_notes && e.src == RMP_SRC_INHERITED)) e.src = RMP_SRC_SAVED;
    } else {
      e.src = RMP_SRC_NONE;
    }
    e.flags &= (uint8_t)~RMP_RF_UPDATED;   // shown until the next load
  }
  return !have_map && !have_notes;
}

RMP_COLD uint32_t rmp_resolve_inherit(rmp_slot_map *m, rmp_slot_notes *n, const rmp_slot_map *prev) {
  uint32_t ch = 0;
  for (int s = 0; s < RMP_SLOTS; ++s) {
    rmp_slot_note &e = n->slot[s];
    if (m->slot[s].id[0] || e.src == RMP_SRC_EMPTY || e.fp || !prev->slot[s].id[0]) continue;
    m->slot[s] = prev->slot[s];
    e.src = RMP_SRC_INHERITED;
    ch |= 1u << s;
  }
  return ch;
}

// The installed package for fp: exactly one, or the one whose id the note wants. -1 none, -2 a collision.
RMP_COLD static int pick(const rmp_pkg_ref *pk, int npk, uint16_t fp, const char *want) {
  int found = -1, count = 0;
  for (int i = 0; i < npk; ++i) {
    if (pk[i].fp != fp) continue;
    if (want && want[0] && !strncmp(pk[i].id, want, 32)) return i;
    found = i;
    ++count;
  }
  return count > 1 ? -2 : found;
}
RMP_COLD static void note_pkg(rmp_slot_note &e, const rmp_pkg_ref &p) {
  memset(e.id, 0, sizeof e.id);
  strncpy(e.id, p.id, 31);
  e.version = p.version;
  memset(e.name, 0, sizeof e.name);
  if (p.name) copy_name(e.name, p.name, (uint32_t)strnlen(p.name, 10));
}
// Slot s against its sounds' fingerprints: top (count ntop) the most used, has whether fp(the slot's id) is
// among them. Returns true when the assignment changed.
RMP_COLD static bool resolve_slot(rmp_slot_map *m, rmp_slot_notes *n, int s, uint16_t top, uint16_t ntop, bool has, bool mixed,
                         const rmp_pkg_ref *pk, int npk) {
  rmp_slot_note &e = n->slot[s];
  rmp_slot_entry &cur = m->slot[s];
  e.flags &= (uint8_t)~(RMP_RF_COLLISION | RMP_RF_UNKNOWN | RMP_RF_MIXED);
  if (mixed) e.flags |= RMP_RF_MIXED;
  if (has) {   // the slot's machine is one the sounds use: it stands
    e.fp = rmp_plugin_fp(cur.id), e.nfp = ntop;
    if (e.src == RMP_SRC_INHERITED) e.src = RMP_SRC_SOUNDS;
    return false;
  }
  e.fp = top, e.nfp = ntop;
  if (e.id[0] && rmp_plugin_fp(e.id) != top) e.id[0] = 0, e.name[0] = 0, e.version = 0;   // a stale wish
  if (e.src == RMP_SRC_EMPTY) return false;   // explicit empty stays
  int k = pick(pk, npk, top, e.id);
  if (k < 0) {
    e.flags |= k == -2 ? RMP_RF_COLLISION : RMP_RF_UNKNOWN;
    if (e.src == RMP_SRC_INHERITED && cur.id[0]) {   // inherited, and the sounds want another machine: not this one
      memset(&cur, 0, sizeof cur);
      e.src = RMP_SRC_NONE;
      return true;
    }
    return false;
  }
  if (cur.id[0] && (e.src == RMP_SRC_SAVED || e.src == RMP_SRC_IMPORTED || e.src == RMP_SRC_INHERITED))
    e.flags |= RMP_RF_UPDATED;
  memset(&cur, 0, sizeof cur);
  strncpy(cur.id, pk[k].id, 31);
  cur.version = pk[k].version;
  e.src = RMP_SRC_SOUNDS;
  note_pkg(e, pk[k]);
  return true;
}

RMP_COLD uint32_t rmp_resolve_open_slots(const rmp_slot_notes *n) {
  uint32_t r = 0;
  for (int s = 0; s < RMP_SLOTS; ++s)
    if (n->slot[s].src != RMP_SRC_EMPTY && (n->slot[s].flags & (RMP_RF_UNKNOWN | RMP_RF_COLLISION))) r |= 1u << s;
  return r;
}

RMP_COLD uint32_t rmp_resolve_prints(rmp_slot_map *m, rmp_slot_notes *n, const rmp_prints *p, const rmp_pkg_ref *pk, int npk,
                                     uint32_t mask) {
  uint32_t ch = 0;
  for (int s = 0; s < RMP_SLOTS; ++s) {
    if (!(mask >> s & 1)) continue;
    uint16_t top = 0, ntop = 0, fcur = m->slot[s].id[0] ? rmp_plugin_fp(m->slot[s].id) : 0;
    bool has = false;
    int distinct = 0;
    for (int k = 0; k < p->n; ++k) {
      const rmp_print &q = p->e[k];
      if (q.slot != s) continue;
      ++distinct;
      if (fcur && q.fp == fcur) has = true;
      if (q.count > ntop || (q.count == ntop && q.fp < top)) top = q.fp, ntop = q.count;
    }
    if (!distinct) {   // no fingerprint information: the map (saved, imported, inherited) stands
      rmp_slot_note &e = n->slot[s];
      e.fp = 0, e.nfp = 0;
      e.flags &= (uint8_t)~(RMP_RF_COLLISION | RMP_RF_UNKNOWN | RMP_RF_MIXED);
      continue;
    }
    if (has) for (int k = 0; k < p->n; ++k) if (p->e[k].slot == s && p->e[k].fp == fcur) ntop = p->e[k].count;
    if (resolve_slot(m, n, s, top, ntop, has, distinct > 1, pk, npk)) ch |= 1u << s;
  }
  return ch;
}

RMP_COLD bool rmp_resolve_tag(rmp_slot_map *m, rmp_slot_notes *n, int s, uint16_t fp, const rmp_pkg_ref *pk, int npk) {
  if (s < 0 || s >= RMP_SLOTS || !fp) return false;
  rmp_slot_note &e = n->slot[s];
  const rmp_slot_entry &cur = m->slot[s];
  bool has = cur.id[0] && rmp_plugin_fp(cur.id) == fp;
  if (has || (cur.id[0] && e.src != RMP_SRC_INHERITED)) return false;   // matches, or the project's own: stands
  (void)e;
  return resolve_slot(m, n, s, fp, 0, false, false, pk, npk);
}

RMP_COLD void rmp_resolve_set(rmp_slot_map *m, rmp_slot_notes *n, int s, const char *id, uint32_t version, int src) {
  if (s < 0 || s >= RMP_SLOTS) return;
  rmp_slot_note &e = n->slot[s];
  memset(&m->slot[s], 0, sizeof m->slot[s]);
  uint16_t fp = e.fp, nfp = e.nfp;
  memset(&e, 0, sizeof e);
  e.fp = fp, e.nfp = nfp;   // what the sounds use stays noted
  if (!id || !id[0]) { e.src = RMP_SRC_EMPTY; return; }
  strncpy(m->slot[s].id, id, 31);
  m->slot[s].version = version;
  e.src = (uint8_t)src;
  strncpy(e.id, id, 31);
  e.version = version;
}

RMP_COLD void rmp_resolve_import(rmp_slot_map *m, rmp_slot_notes *n, int s, const char *id, uint32_t version, uint16_t fp,
                                 const char *name) {
  if (s < 0 || s >= RMP_SLOTS) return;
  rmp_resolve_set(m, n, s, id, version, id && id[0] ? RMP_SRC_IMPORTED : RMP_SRC_EMPTY);
  rmp_slot_note &e = n->slot[s];
  if (id && id[0]) e.fp = fp ? fp : rmp_plugin_fp(id);
  if (name) copy_name(e.name, name, (uint32_t)strnlen(name, 10));
}

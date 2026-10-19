// rmp_presets.cpp - see rmp_presets.h.
#include "rmp_presets.h"
#include "rmp_format.h"
#include "rmp_manifest.h"
#include <string.h>
#include <strings.h>
// Everything here runs from loop() (jobs and report(), never an ISR or the audio path): flash.
#if defined(__IMXRT1062__)
#define RMP_PRE_COLD __attribute__((section(".flashmem")))
#else
#define RMP_PRE_COLD
#endif

static inline bool ws(char c) { return c == ' ' || c == '\t'; }
// A report's float as 0..127 (NaN and below 0: 0); never an out-of-range float-to-int conversion.
static inline int q7(float f) { return !(f > 0.0f) ? 0 : f >= 127.0f ? 127 : (int)f; }
static inline uint8_t next_gen(uint8_t g) { return (uint8_t)(g == 255 ? 1 : g + 1); }

// 1..max digits at *p (advances): false if none or too many.
RMP_PRE_COLD static bool num(const char **p, const char *end, int max, int *v) {
  int n = 0, x = 0;
  while (*p < end && **p >= '0' && **p <= '9') { if (++n > max) return false; x = x * 10 + (**p - '0'); ++*p; }
  *v = x;
  return n > 0;
}

RMP_PRE_COLD void rmp_preset_parse(const char *txt, uint32_t len, int16_t values[16], bool truncated) {
  for (int k = 0; k < 16; ++k) values[k] = -1;
  if (!txt) return;
  const char *p = txt, *end = txt + len;
  if (len >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;   // UTF-8 BOM
  while (p < end) {
    const char *le = p;
    while (le < end && *le != '\n' && *le != '\r') ++le;   // LF, CRLF (an empty line between) or a lone CR
    if (le == end && truncated) break;                      // the cut last line
    const char *q = p;
    p = le < end ? le + 1 : end;
    while (q < le && ws(*q)) ++q;
    int n, v;
    if (q >= le || (*q != 'P' && *q != 'p')) continue;
    ++q;
    if (!num(&q, le, 2, &n) || q >= le || !ws(*q)) continue;
    while (q < le && ws(*q)) ++q;
    if (!num(&q, le, 3, &v)) continue;
    while (q < le && ws(*q)) ++q;
    if (q != le || n < 1 || n > 16 || v > 127) continue;
    values[n - 1] = (int16_t)v;
  }
}

RMP_PRE_COLD void rmp_preset_names_init(rmp_preset_names *nm) {
  nm->slot = -1;
  nm->gen = 0;
  nm->n = 0;
}

RMP_PRE_COLD void rmp_preset_index_init(rmp_preset_index *ix) {
  ix->n = 0;
  ix->gen = 1;
  ix->ok = 0;
  ix->print = 0;
}

RMP_PRE_COLD void rmp_preset_invalidate(rmp_preset_index *ix) {
  ix->ok = 0;
  ix->gen = next_gen(ix->gen);
}

RMP_PRE_COLD void rmp_preset_scan_begin(rmp_preset_names *nm, rmp_preset_index *ix, int slot) {
  nm->slot = (int8_t)slot;
  nm->n = 0;
  ix->n = 0;
  ix->ok = 0;
}

RMP_PRE_COLD bool rmp_preset_add(rmp_preset_names *nm, rmp_preset_index *ix, const char *file, uint16_t dir, const char *ext) {
  if (!file || file[0] == '.') return false;   // no name, or AppleDouble (._x) / dot files
  const char *dot = strrchr(file, '.');
  if (!dot) return false;
  uint8_t kind;
  if (!strcasecmp(dot + 1, "txt")) kind = RMP_PRESET_GENERIC;
  else if (ext && *ext && !strcasecmp(dot + 1, ext)) kind = RMP_PRESET_NATIVE;
  else return false;
  char nm1[RMP_PRESET_NAME] = {0};
  int len = (int)(dot - file) < RMP_PRESET_NAME - 1 ? (int)(dot - file) : RMP_PRESET_NAME - 1;
  bool blank = true;
  for (int i = 0; i < len; ++i) {
    nm1[i] = (file[i] >= 0x20 && file[i] <= 0x7e) ? file[i] : '_';
    if (nm1[i] != ' ') blank = false;
  }
  if (blank) return false;
  int n = ix->n, at = 0;
  while (at < n && strcmp(nm->name[at], nm1) < 0) ++at;
  if (at < n && !strcmp(nm->name[at], nm1)) {
    if (kind == RMP_PRESET_GENERIC || ix->kind[at] == RMP_PRESET_NATIVE) return false;   // the first stays / native wins
    ix->kind[at] = RMP_PRESET_NATIVE;   // a native one replaces the generic one of its name
    ix->dir[at] = dir;
    return true;
  }
  if (at >= RMP_PRESETS_MAX) return false;                    // after the first 127 by name
  int keep = n < RMP_PRESETS_MAX ? n : RMP_PRESETS_MAX - 1;   // a full list drops its last
  for (int i = keep; i > at; --i) {
    memcpy(nm->name[i], nm->name[i - 1], RMP_PRESET_NAME);
    ix->kind[i] = ix->kind[i - 1];
    ix->dir[i] = ix->dir[i - 1];
  }
  memcpy(nm->name[at], nm1, RMP_PRESET_NAME);
  ix->kind[at] = kind;
  ix->dir[at] = dir;
  if (n < RMP_PRESETS_MAX) ++n;
  ix->n = (uint8_t)n;
  nm->n = (uint8_t)n;
  return true;
}

RMP_PRE_COLD static uint32_t fnv(uint32_t h, const void *p, uint32_t n) {
  const uint8_t *b = (const uint8_t *)p;
  for (uint32_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
  return h;
}

RMP_PRE_COLD void rmp_preset_scan_end(rmp_preset_names *nm, rmp_preset_index *ix) {
  uint32_t h = fnv(2166136261u, &ix->n, 1);
  h = fnv(h, nm->name, (uint32_t)ix->n * RMP_PRESET_NAME);
  h = fnv(h, ix->kind, ix->n);
  h = fnv(h, ix->dir, (uint32_t)ix->n * sizeof ix->dir[0]);
  if (h != ix->print) ix->gen = next_gen(ix->gen);   // the list came out different
  ix->print = h;
  ix->ok = 1;
  nm->n = ix->n;
  nm->gen = ix->gen;
}

RMP_PRE_COLD bool rmp_preset_names_ok(const rmp_preset_names *nm, const rmp_preset_index *ix, int slot) {
  return ix->ok && nm->slot >= 0 && nm->slot == slot && nm->gen == ix->gen;
}

RMP_PRE_COLD bool rmp_preset_index_ok(const rmp_preset_index *ix) { return ix->ok != 0; }

RMP_PRE_COLD void rmp_knobs_from_roles(rmp_knobs *r, const uint8_t roles[16]) {
  for (int k = 0; k < 16; ++k) {
    r->role[k] = roles ? roles[k] : (uint8_t)RMP_ROLE_FREE;
    r->min[k] = 0;
    r->max[k] = 127;
  }
}

RMP_PRE_COLD void rmp_knobs_from_desc(rmp_knobs *r, const rmp_desc *d) {
  rmp_knobs_from_roles(r, nullptr);
  if (!d) return;
  int nk = d->nknobs < RMP_DESC_KNOBS ? d->nknobs : RMP_DESC_KNOBS;
  for (int i = 0; i < nk; ++i) {
    int k = d->knob[i].p - 1;
    if (k < 0 || k > 15) continue;
    r->role[k] = d->knob[i].role;
    r->min[k] = d->knob[i].min;
    r->max[k] = d->knob[i].max;
  }
}

RMP_PRE_COLD void rmp_pv_invalidate(rmp_pv *v) {
  v->synced = v->playing = v->failed = -1;
  v->plock = false;
  v->pmask = v->mask = v->hold = v->omask = 0;
}

RMP_PRE_COLD void rmp_pv_reset(rmp_pv *v) {
  memset(v, 0, sizeof *v);
  rmp_pv_invalidate(v);
}

RMP_PRE_COLD void rmp_pv_check(rmp_pv *v, uint8_t gen) {
  if (v->gen == gen) return;
  rmp_pv_invalidate(v);
  v->gen = gen;
}

RMP_PRE_COLD int rmp_pv_decide(rmp_pv *v, int pre, int total, bool is_trig, bool first, const float *p, bool *sync) {
  *sync = false;
  if (!is_trig) {   // the sound's values (a trig carries its step's p-locks)
    for (int k = 0; k < 16; ++k) v->snd[k] = (uint8_t)q7(p[k]);
    v->snd_ok = true;
  }
  if (pre < 0 || pre > 15 || total <= 0) return -1;
  int e = q7(p[pre]);
  if (e > total - 1) e = total - 1;
  int r = -1;
  bool s = false;
  if (!is_trig) {
    if (first || e != v->synced) { r = e; s = true; }   // the sound's own preset: written back
  } else if (first) {
    r = e;                                               // maybe p-locked: played, not written back
  } else if (e != v->synced) {
    r = (v->plock && v->playing == e) ? -1 : e;          // a p-locked PRE: played, not written
  } else {
    r = v->plock ? e : -1;                               // back from a p-lock to the sound's own preset
  }
  if (r >= 0 && r == v->failed) return -1;               // failed before: not asked again
  *sync = s;
  return r;
}

RMP_PRE_COLD void rmp_pv_failed(rmp_pv *v, int entry) { v->failed = (int16_t)entry; }

RMP_PRE_COLD int rmp_pv_load(rmp_pv *v, int entry, bool sync, const int16_t values[16], const rmp_knobs *r, uint32_t now_ms,
                             const float *p, uint8_t *out_k, uint8_t *out_v) {
  v->failed = -1;
  v->plock = !sync && entry != v->synced;
  v->playing = (int16_t)entry;
  if (sync) v->synced = (int16_t)entry;
  uint16_t m = 0;
  v->omask = 0;
  for (int k = 0; k < 16; ++k) {
    if (values[k] < 0 || values[k] > 127) continue;
    if (r->role[k] == RMP_ROLE_PRESET) {
      int lo = r->min[k], hi = r->max[k] > 127 ? 127 : r->max[k];
      if (lo > hi) { lo = 0; hi = 127; }
      int x = values[k] < lo ? lo : values[k] > hi ? hi : values[k];
      v->val[k] = (uint8_t)x;
      m |= (uint16_t)(1u << k);
    } else if (r->role[k] == RMP_ROLE_OFFSET) {
      v->oval[k] = (uint8_t)values[k];
      v->omask |= (uint16_t)(1u << k);
    }
  }
  v->pmask = v->mask = 0;
  int n = 0;
  if (sync) {
    v->hold = m;
    v->hold_t = now_ms;
    for (int k = 0; k < 16; ++k)
      if (m >> k & 1) { v->hval[k] = v->val[k]; out_k[n] = (uint8_t)k; out_v[n] = v->val[k]; ++n; }
  } else if (v->plock) {
    v->pmask = v->mask = m;
    for (int k = 0; k < 16; ++k) v->base[k] = (uint8_t)q7(p[k]);
  }   // else the sound's own preset: its values are on the knobs already
  return n;
}

RMP_PRE_COLD int rmp_pv_loaded(rmp_pv *v, int entry, bool sync, const int16_t values[16], const uint8_t roles[16],
                               uint32_t now_ms, const float *p, uint8_t *out_k, uint8_t *out_v) {
  rmp_knobs r;
  rmp_knobs_from_roles(&r, roles);
  return rmp_pv_load(v, entry, sync, values, &r, now_ms, p, out_k, out_v);
}

RMP_PRE_COLD void rmp_pv_apply(rmp_pv *v, float *p, bool is_trig, uint32_t now_ms) {
  uint16_t locked = 0;   // knobs p-locked on this trig: their own value wins
  if (!is_trig) {
    for (int k = 0; k < 16; ++k) v->snd[k] = (uint8_t)q7(p[k]);
    v->snd_ok = true;
  } else if (v->snd_ok) {
    for (int k = 0; k < 16; ++k)
      if (q7(p[k]) != v->snd[k]) locked |= (uint16_t)(1u << k);
  }
  if (v->hold) {
    bool all = true;
    for (int k = 0; k < 16; ++k)
      if ((v->hold >> k & 1) && q7(p[k]) != v->hval[k]) all = false;
    if (all || now_ms - v->hold_t >= RMP_PV_HOLD_MS) {
      v->hold = 0;
    } else {
      for (int k = 0; k < 16; ++k)
        if ((v->hold >> k & 1) && !(locked >> k & 1)) p[k] = v->hval[k];
    }
  }
  if (v->plock && v->pmask) {
    if (is_trig) {   // a trig (re)starts the override: all the entry's knobs but those locked on this step
      for (int k = 0; k < 16; ++k) v->base[k] = (uint8_t)q7(p[k]);
      v->mask = (uint16_t)(v->pmask & ~locked);
    }
    for (int k = 0; k < 16; ++k) {
      if (!(v->mask >> k & 1)) continue;
      if (!is_trig && q7(p[k]) != v->base[k]) v->mask &= (uint16_t)~(1u << k);   // turned since: the turn wins
      else p[k] = v->val[k];
    }
  }
}

RMP_PRE_COLD void rmp_pv_effective(const rmp_pv *v, const rmp_knobs *r, float *p) {
  if (!v->omask || v->playing < 0) return;
  for (int k = 0; k < 16; ++k) {
    if (!(v->omask >> k & 1) || r->role[k] != RMP_ROLE_OFFSET) continue;
    float lo = r->min[k], hi = r->max[k] > 127 ? 127 : r->max[k];
    if (lo > hi) { lo = 0; hi = 127; }
    float f = (float)v->oval[k] + (p[k] - 64.0f);
    p[k] = !(f >= lo) ? lo : f > hi ? hi : f;
  }
}

// core/msamp/elmulti.cpp - see elmulti.h. Control core only (an instrument's load), cold.
#include "../platform.h"

#if 1   // upstream: CORE_MSAMP (Pi and Mac); the generic engine is Pi only, always built
#include "elmulti.h"

#include <string.h>

#include "sfz.h"
#include "text.h"

namespace core {
namespace msamp {

namespace {

constexpr float kDefaultVelocity = 0.49411765f;   // the format's default layer threshold (about MIDI 63)

// What a region needs after its file is read: where it sits in the file's zone / layer tree.
struct Tmp {
  uint16_t zone, layer;   // running counts (a zone's regions are contiguous, a layer's too)
  uint8_t pitch;          // the zone's `pitch`
  uint8_t vel;            // the layer's threshold as MIDI velocity 1..127
};

struct Reader {
  Desc* d;
  const char* dir;
  Tmp* tmp;
  int zone = -1, layer = -1;
  int zone_pitch = -1;
  float zone_center = -1;
  bool zone_has_center = false;
  uint8_t layer_vel = 64;
  enum { kTop, kZone, kLayer, kSlot, kOther } at = kTop;
  Region cur;
  char sample[kPath];
  bool open = false, loop_forward = false, loop_off = false, keep = false;

  void ignored(const char* name) {
    if (!d->ignored++ && name) {
      strncpy(d->first_ignored, name, sizeof d->first_ignored - 1);
      d->first_ignored[sizeof d->first_ignored - 1] = 0;
    }
  }
  int sample_index(const char* path) {
    for (int i = 0; i < d->nsamples; ++i)
      if (strcasecmp(d->samples[i], path) == 0) return i;
    if (d->nsamples >= d->max_samples) return -1;
    strcpy(d->samples[d->nsamples], path);
    return d->nsamples++;
  }
  // The open slot into the table (a zone without a pitch, a slot without a sample, a full table: dropped, counted).
  void commit() {
    if (!open) return;
    open = false;
    char full[kPath];
    int i = -1;
    if (zone_pitch >= 0 && zone_pitch <= 127 && sample[0] && d->nregions < d->max_regions &&
        join_path(dir, sample, full, sizeof full))
      i = sample_index(full);
    if (i < 0) {
      ++d->skipped;
      return;
    }
    Region r = cur;
    r.sample = (int16_t)i;
    float kc = zone_has_center ? zone_center : (float)zone_pitch;
    int k0 = (int)(kc + (kc >= 0 ? 0.5f : -0.5f));   // the nearest key; the rest is `tune`
    r.keycenter = (uint8_t)(k0 < 0 ? 0 : k0 > 127 ? 127 : k0);
    r.tune = (kc - (float)r.keycenter) * 100.0f;   // (a root at 40.5 under key 41: 50 cents down)
    // loop-mode: Forward loops (and keeps looping after the release with keep-looping-on-release), Off does not, and a
    // slot without one is a one-shot [Appendix C of the converter's spec: SFZ loop_continuous / loop_sustain /
    // no_loop; "omitting it yields one-shot playback"]
    r.loop_mode = loop_forward ? (keep ? kLoopContinuous : kLoopSustain) : loop_off ? kNoLoop : kOneShot;
    Tmp& t = tmp[d->nregions];
    t.zone = (uint16_t)zone, t.layer = (uint16_t)layer, t.pitch = (uint8_t)zone_pitch, t.vel = layer_vel;
    d->regions[d->nregions++] = r;
  }
  void header(const char* h) {
    commit();
    if (!strcmp(h, "key-zones")) {
      ++zone, layer = -1, zone_pitch = -1, zone_has_center = false, at = kZone;
      layer_vel = (uint8_t)(kDefaultVelocity * 127.0f + 0.5f);   // (a layer without `velocity` takes the default, not the last zone's)
    } else if (!strcmp(h, "key-zones.velocity-layers")) {
      ++layer, layer_vel = (uint8_t)(kDefaultVelocity * 127.0f + 0.5f), at = kLayer;
    } else if (!strcmp(h, "key-zones.velocity-layers.sample-slots")) {
      cur = default_region_sfz();
      sample[0] = 0, open = true, loop_forward = loop_off = keep = false, at = kSlot;
    } else {
      at = kOther;
      ++d->malformed;   // a table this reader does not know
    }
  }
  void value(const char* name, const char* v, bool quoted) {
    double x = 0;
    bool isnum = false;
    if (!quoted) {
      const char* e;
      x = to_double(v, &e);
      // a number that fits every field below (frames < 2e9: never the sfz sentinels; no inf / huge before a cast)
      isnum = e != v && !*e && x > -2e9 && x < 2e9;
    }
    switch (at) {
      case kTop:
        if (strcmp(name, "version") && strcmp(name, "name")) ignored(name);
        return;
      case kZone:
        if (!strcmp(name, "pitch") && isnum) zone_pitch = (int)x;
        else if (!strcmp(name, "key-center") && isnum && x > -1000 && x < 1000) zone_center = (float)x, zone_has_center = true;
        else ignored(name);
        return;
      case kLayer:
        if (!strcmp(name, "velocity") && isnum) {
          int m = (int)(x * 127.0 + 0.5);
          layer_vel = (uint8_t)(m < 1 ? 1 : m > 127 ? 127 : m);
        } else if (strcmp(name, "strategy")) {   // (Forward: the only round-robin strategy known)
          ignored(name);
        }
        return;
      case kSlot:
        if (!strcmp(name, "sample") && quoted) {
          strncpy(sample, v, sizeof sample - 1);
          sample[sizeof sample - 1] = 0;
        } else if (!strcmp(name, "loop-mode") && quoted) {
          loop_forward = !strcasecmp(v, "forward"), loop_off = !strcasecmp(v, "off");
        } else if (!strcmp(name, "keep-looping-on-release")) {
          keep = !quoted && !strcmp(v, "true");
        } else if (!strcmp(name, "loop-start") && isnum && x >= 0) {
          cur.loop_start = (uint32_t)x;
        } else if (!strcmp(name, "loop-end") && isnum && x >= 0) {
          cur.loop_end = (uint32_t)x;
        } else if (!strcmp(name, "trim-start") && isnum && x >= 0) {
          cur.offset = (uint32_t)x;
        } else if (!strcmp(name, "trim-end") && isnum && x >= 1) {
          cur.end = (uint32_t)x - 1;   // (an end position: the region's last frame is the one before it)
        } else {
          ignored(name);   // loop-crossfade: no model for it
        }
        return;
      default:
        return;
    }
  }
};

// Ranges and round robin from the zone / layer tree, once the whole file is read. A zone's regions are contiguous.
CORE_COLD void finish(Desc* d, const Tmp* tmp) {
  bool has[128] = {false};
  for (int i = 0; i < d->nregions; ++i) has[tmp[i].pitch] = true;
  for (int a = 0; a < d->nregions;) {
    int b = a;
    while (b < d->nregions && tmp[b].zone == tmp[a].zone) ++b;
    int p = tmp[a].pitch;
    // keys: each pitch is the root of the zone, the keys between two roots go to the nearer one (the middle one of an
    // odd gap: the lower), the first zone down to 0 and the last up to 127 [the spec: "root key selection uses the
    // nearest root note"; the key range itself is not stored]
    int prev = -1, next = -1;
    for (int k = p - 1; k >= 0 && prev < 0; --k)
      if (has[k]) prev = k;
    for (int k = p + 1; k <= 127 && next < 0; ++k)
      if (has[k]) next = k;
    int lo = prev < 0 ? 0 : (prev + p) / 2 + 1, hi = next < 0 ? 127 : (p + next) / 2;
    // velocity: a layer's threshold is its lower bound, the next higher layer's the end of it; the lowest layer takes
    // everything below (hard switching, no crossfade)
    // (linear in the zone: the thresholds present, then each one's next higher by one pass over 1..127)
    bool present[129] = {false};
    uint8_t nextv[129];
    int minv = 128;
    for (int i = a; i < b; ++i) present[tmp[i].vel] = true, minv = tmp[i].vel < minv ? tmp[i].vel : minv;
    for (int v = 127, nx = 128; v >= 0; --v) {
      nextv[v] = (uint8_t)nx;
      if (present[v]) nx = v;
    }
    for (int i = a; i < b; ++i) {
      int up = nextv[tmp[i].vel];
      Region& r = d->regions[i];
      r.lokey = (uint8_t)lo, r.hikey = (uint8_t)hi;
      r.lovel = tmp[i].vel == minv ? 1 : tmp[i].vel, r.hivel = (uint8_t)(up - 1);
    }
    // round robin: the slots of one layer rotate
    for (int i = a; i < b;) {
      int j = i;
      while (j < b && tmp[j].layer == tmp[i].layer) ++j;
      if (j - i > 1)
        for (int k = i; k < j; ++k) d->regions[k].seq_length = (uint16_t)(j - i), d->regions[k].seq_position = (uint16_t)(k - i + 1);
      i = j;
    }
    a = b;
  }
}

}  // namespace

CORE_COLD bool is_elmulti(const char* t, int len) {
  static const char kKey[] = "[[key-zones";
  for (int i = 0; i + (int)sizeof kKey - 1 <= len; ++i) {
    if (t[i] == 0) return false;
    if (t[i] == '[' && !strncmp(t + i, kKey, sizeof kKey - 1) && (i == 0 || t[i - 1] == '\n' || t[i - 1] == '\r')) return true;
  }
  return false;
}

CORE_COLD bool parse_elmulti(const char* t, int len, const char* dir, Desc* d) {
  d->nregions = d->nsamples = d->ignored = d->skipped = d->malformed = d->load = 0;
  d->first_ignored[0] = d->err[0] = 0;
  Reader* R = new Reader;   // off the control core's stack
  R->tmp = new Tmp[d->max_regions > 0 ? d->max_regions : 1];
  R->d = d, R->dir = dir;
  for (int i = 0; i < len;) {
    int e = i;
    while (e < len && t[e] != '\n' && t[e] != '\r') ++e;
    char ln[2 * kPath];
    int l = e - i < (int)sizeof ln - 1 ? e - i : (int)sizeof ln - 1;
    memcpy(ln, t + i, l);
    ln[l] = 0;
    i = e + 1;
    if (memchr(ln, 0, l) != nullptr) break;
    char* s = ln;
    while (is_space(*s)) ++s;
    if (!*s || *s == '#') continue;
    if (s[0] == '[' && s[1] == '[') {   // [[key-zones...]]
      char* c = strstr(s, "]]");
      if (!c) {
        ++d->malformed;
        continue;
      }
      *c = 0;
      char* h = s + 2;
      while (is_space(*h)) ++h;
      for (char* q = c; q > h && is_space(q[-1]); --q) q[-1] = 0;
      R->header(h);
      continue;
    }
    if (s[0] == '[') {   // a plain table: nothing of ours
      R->commit();
      R->at = Reader::kOther;
      ++d->malformed;
      continue;
    }
    char* eq = strchr(s, '=');
    if (!eq) {
      ++d->malformed;
      continue;
    }
    char* nm = s;
    char* ne = eq;
    while (ne > nm && is_space(ne[-1])) --ne;
    *ne = 0;
    char* v = eq + 1;
    while (is_space(*v)) ++v;
    bool quoted = *v == '\'' || *v == '"';
    if (quoted) {
      char q = *v++;
      char* ve = strchr(v, q);
      if (!ve) {
        ++d->malformed;
        continue;
      }
      *ve = 0;
    } else {
      char* ve = v;
      while (*ve && !is_space(*ve) && *ve != '#') ++ve;
      *ve = 0;
    }
    R->value(nm, v, quoted);
  }
  R->commit();
  if (d->nregions) finish(d, R->tmp);
  delete[] R->tmp;
  delete R;
  if (!d->nregions) {
    sniprintf(d->err, sizeof d->err, "no playable region (%d skipped, %d malformed)", d->skipped, d->malformed);
    return false;
  }
  return true;
}

}  // namespace msamp
}  // namespace core
#endif

// core/bundle/manifest.cpp - see manifest.h. (The writer's and reader's shape from 3a8bf7d's core/bundle/manifest.cpp,
// the old line's Rytm fields - prints, slots, Send FX route, sqp/song formats - replaced by the box project's.)
#include "manifest.h"

#include <string.h>
#include <strings.h>

#include "../fmt/fmt.h"
#include "../lib/sha256.h"
#include "../platform.h"

namespace core {
namespace bundle {

namespace {
const char* const kKindNames[kKinds] = {"box", "store", "sample"};
const char* const kNeedNames[kNeedKinds] = {"code", "www"};
const char* const kBoxTypes[] = {"rytm", "a4", "dt"};
void cpy(char* d, const char* s, int cap) { sniprintf(d, cap, "%s", s ? s : ""); }
bool path_ok(const char* p) {   // a card path in a manifest: absolute, a bundle-safe name after the '/'
  return p[0] == '/' && strlen(p) < (size_t)kCardPath && name_ok(p + 1);
}
bool box_type_ok(const char* t) {
  for (const char* b : kBoxTypes)
    if (!strcmp(t, b)) return true;
  return false;
}
bool need_name_ok(const char* n) {   // a blob's / web bundle's name: 1..31 of [A-Za-z0-9._-]
  size_t l = strlen(n);
  if (!l || l >= sizeof(Need::name)) return false;
  for (size_t i = 0; i < l; ++i) {
    char c = n[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return false;
  }
  return true;
}
const fmt::Format& reg() {   // the registry's row: always there (formats.cpp)
  return *fmt::find("bundle");
}
// An entry's own format against its registry row: false when this build cannot read that version
bool entry_fmt_ok(const MEntry& x) {
  if (!x.fmt[0]) return true;
  const fmt::Format* f = fmt::find(x.fmt);
  return !f || (x.fmt_ver >= 0 && x.fmt_ver <= f->current && (x.fmt_ver == 0 || x.fmt_ver >= f->oldest));
}
}  // namespace

bool hex_id(const char* s) {
  for (int i = 0; i < kHexId; ++i)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
  return s[kHexId] == 0;
}
const char* kind_name(int k) { return k >= 0 && k < kKinds ? kKindNames[k] : "?"; }
int kind_of(const char* s) {
  for (int k = 0; k < kKinds; ++k)
    if (!strcmp(s, kKindNames[k])) return k;
  return -1;
}
const char* need_name(int k) { return k >= 0 && k < kNeedKinds ? kNeedNames[k] : "?"; }
int need_of(const char* s) {
  for (int k = 0; k < kNeedKinds; ++k)
    if (!strcmp(s, kNeedNames[k])) return k;
  return -1;
}

void Manifest::alloc(int entries, int paths, int miss) {
  if (!e) e = new MEntry[entries], cap_e = entries;
  if (!p) p = new MPath[paths], cap_p = paths;
  if (!missing) missing = new char[miss][kCardPath], cap_missing = miss;
  clear();
}
void Manifest::clear() {
  MEntry* e0 = e;
  MPath* p0 = p;
  char(*m0)[kCardPath] = missing;
  int ce = cap_e, cp = cap_p, cm = cap_missing;
  memset((void*)this, 0, sizeof *this);
  e = e0, p = p0, missing = m0, cap_e = ce, cap_p = cp, cap_missing = cm;
  version = min_reader = reg().current;
}
int Manifest::add_entry(const char* name, int kind) {
  if (ne >= cap_e || !name_ok(name) || !strcmp(name, "manifest.json") || find_entry(name) >= 0) return -1;
  if (kind < 0 || kind >= kKinds) return -1;
  MEntry& x = e[ne];
  memset(&x, 0, sizeof x);
  cpy(x.name, name, sizeof x.name);
  x.kind = (uint8_t)kind;
  x.first_path = np;
  return ne++;
}
bool Manifest::add_path(int i, const char* path) {
  if (np >= cap_p || i < 0 || i >= ne || !path_ok(path)) return false;
  if (e[i].npaths == 0) e[i].first_path = np;
  else if (e[i].first_path + e[i].npaths != np) return false;   // an entry's paths are contiguous
  cpy(p[np].path, path, kCardPath);
  p[np].entry = i;
  ++np, ++e[i].npaths;
  return true;
}
bool Manifest::add_need(int kind, const char* name, const uint8_t sha[32]) {
  if (nneeds >= kNeedsMax || kind < 0 || kind >= kNeedKinds || !need_name_ok(name)) return false;
  for (int i = 0; i < nneeds; ++i)
    if (needs[i].kind == kind && !memcmp(needs[i].sha, sha, 32)) return false;
  Need& n = needs[nneeds++];
  n.kind = (uint8_t)kind;
  cpy(n.name, name, sizeof n.name);
  memcpy(n.sha, sha, 32);
  return true;
}
bool Manifest::add_missing(const char* path) {
  if (nmissing >= cap_missing || !path_ok(path)) return false;
  cpy(missing[nmissing++], path, kCardPath);
  return true;
}
int Manifest::find_entry(const char* name) const {
  for (int i = 0; i < ne; ++i)
    if (!strcmp(e[i].name, name)) return i;
  return -1;
}
int Manifest::find_path(const char* path) const {
  for (int i = 0; i < np; ++i)
    if (!strcasecmp(p[i].path, path)) return i;
  return -1;
}
uint64_t Manifest::bytes() const {
  uint64_t b = 0;
  for (int i = 0; i < ne; ++i) b += e[i].size;
  return b;
}
int Manifest::count(int kind) const {
  int c = 0;
  for (int i = 0; i < ne; ++i) c += e[i].kind == kind;
  return c;
}

bool write_manifest(const Manifest& m, JOut& o) {
  if (!box_type_ok(m.box_type) || !hex_id(m.box_uid) || !hex_id(m.project_id)) return false;
  for (int i = 0; i < m.ne; ++i)
    if (!entry_fmt_ok(m.e[i])) return false;
  const fmt::Format& f = reg();
  char h[65];
  o.raw("{\"format\":\"sidecar-bundle\",\"version\":");
  o.num(f.current);
  o.raw(",\"min_reader\":");
  o.num(f.current);
  o.raw(",\"created\":");
  o.str(m.created);
  o.raw(",\"clock\":");
  o.str(m.clock);
  o.raw(",\n\"sidecar\":{\"version\":");
  o.str(m.sc_version);
  o.raw(",\"board\":");
  o.str(m.board);
  o.raw("},\n\"box\":{\"type\":");
  o.str(m.box_type);
  o.raw(",\"uid\":");
  o.str(m.box_uid);
  o.raw(",\"name\":");
  o.str(m.box_name);
  o.raw("},\n\"project\":{\"id\":");
  o.str(m.project_id);
  o.raw(",\"name\":");
  o.str(m.project_name);
  o.raw("},\n\"needs\":[");
  for (int i = 0; i < m.nneeds; ++i) {
    if (i) o.raw(",");
    hex(m.needs[i].sha, 32, h);
    o.raw("{\"kind\":"), o.str(need_name(m.needs[i].kind)), o.raw(",\"name\":"), o.str(m.needs[i].name);
    o.raw(",\"sha256\":"), o.str(h), o.raw("}");
  }
  o.raw("],\n\"entries\":[");
  for (int i = 0; i < m.ne; ++i) {
    const MEntry& x = m.e[i];
    o.raw(i ? ",\n" : "\n");
    o.raw("{\"name\":"), o.str(x.name), o.raw(",\"kind\":"), o.str(kind_name(x.kind)), o.raw(",\"size\":");
    o.num(x.size);
    hex(x.sha, 32, h);
    o.raw(",\"sha256\":"), o.str(h);
    if (x.npaths) {
      o.raw(",\"paths\":[");
      for (int j = 0; j < x.npaths; ++j) {
        if (j) o.raw(",");
        o.str(m.p[x.first_path + j].path);
      }
      o.raw("]");
    }
    if (x.fmt[0]) o.raw(",\"format\":{\"name\":"), o.str(x.fmt), o.raw(",\"version\":"), o.num((uint64_t)x.fmt_ver), o.raw("}");
    o.raw("}");
  }
  o.raw("],\n\"missing\":[");
  for (int i = 0; i < m.nmissing; ++i) {
    if (i) o.raw(",");
    o.str(m.missing[i]);
  }
  o.raw("],\n\"totals\":{\"entries\":");
  o.num((uint64_t)m.ne);
  o.raw(",\"bytes\":");
  o.num(m.bytes());
  o.raw(",\"store\":");
  o.num((uint64_t)m.count(kKindStore));
  o.raw(",\"samples\":");
  o.num((uint64_t)m.count(kKindSample));
  o.raw("}}\n");
  return !o.over;
}

// ---- the reader
namespace {
struct R {
  JIn& j;
  char* err;
  int cap;
  bool bad(const char* what) {
    sniprintf(err, cap, "manifest: %s", what ? what : "a value");
    return false;
  }
  bool str(char* out, int c, const char* what) {
    if (!j.str(out, (uint32_t)c)) return bad(what);
    return true;
  }
  bool num(int64_t* v, int64_t lo, int64_t hi, const char* what) {
    if (!j.num(v) || *v < lo || *v > hi) return bad(what);
    return true;
  }
};
}  // namespace

bool read_manifest(const char* text, uint32_t n, Manifest* m, char* err, int cap) {
  m->clear();
  m->version = 0, m->min_reader = 0;
  if (n > kManifestMax) return sniprintf(err, cap, "manifest: over 1 MB"), false;
  // the registry's verdict first (sniff: "format":"sidecar-bundle" and the versions among the first keys)
  const fmt::Format& f = reg();
  fmt::Info info;
  const fmt::Status st = fmt::load(f, fmt::Bytes{(const uint8_t*)text, n}, nullptr, &info);
  if (st != fmt::kOk && st != fmt::kForeignNewer) {
    fmt::refusal(f, info, "manifest.json", err, cap);
    return false;
  }
  m->read_only = st == fmt::kForeignNewer;

  JIn j;
  j.begin(text, n);
  R r{j, err, cap};
  char k[64], v[kCardPath];
  int64_t x;
  bool format = false, have_entries = false, have_box = false, have_project = false;
  if (!j.obj_begin()) return r.bad(j.err);
  while (j.next_key(k, sizeof k)) {
    if (!strcmp(k, "format")) {
      if (!r.str(v, sizeof v, "format") || strcmp(v, "sidecar-bundle")) return r.bad("not a Sidecar bundle");
      format = true;
    } else if (!strcmp(k, "version")) {
      if (!r.num(&x, 1, 65535, "version")) return false;
      m->version = (int)x;
    } else if (!strcmp(k, "min_reader")) {
      if (!r.num(&x, 1, 65535, "min_reader")) return false;
      m->min_reader = (int)x;
    } else if (!strcmp(k, "created")) {
      if (!r.str(m->created, sizeof m->created, "created")) return false;
    } else if (!strcmp(k, "clock")) {
      if (!r.str(m->clock, sizeof m->clock, "clock")) return false;
    } else if (!strcmp(k, "sidecar")) {
      if (!j.obj_begin()) return r.bad("sidecar");
      while (j.next_key(k, sizeof k)) {
        if (!strcmp(k, "version")) { if (!r.str(m->sc_version, sizeof m->sc_version, "sidecar.version")) return false; }
        else if (!strcmp(k, "board")) { if (!r.str(m->board, sizeof m->board, "sidecar.board")) return false; }
        else if (!j.skip()) return r.bad("sidecar");
      }
      if (j.err) return r.bad(j.err);
    } else if (!strcmp(k, "box")) {
      have_box = true;
      if (!j.obj_begin()) return r.bad("box");
      while (j.next_key(k, sizeof k)) {
        if (!strcmp(k, "type")) { if (!r.str(m->box_type, sizeof m->box_type, "box.type")) return false; }
        else if (!strcmp(k, "uid")) { if (!r.str(m->box_uid, sizeof m->box_uid, "box.uid")) return false; }
        else if (!strcmp(k, "name")) { if (!r.str(m->box_name, sizeof m->box_name, "box.name")) return false; }
        else if (!j.skip()) return r.bad("box");
      }
      if (j.err) return r.bad(j.err);
    } else if (!strcmp(k, "project")) {
      have_project = true;
      if (!j.obj_begin()) return r.bad("project");
      while (j.next_key(k, sizeof k)) {
        if (!strcmp(k, "id")) { if (!r.str(m->project_id, sizeof m->project_id, "project.id")) return false; }
        else if (!strcmp(k, "name")) { if (!r.str(m->project_name, sizeof m->project_name, "project.name")) return false; }
        else if (!j.skip()) return r.bad("project");
      }
      if (j.err) return r.bad(j.err);
    } else if (!strcmp(k, "needs")) {
      if (!j.arr_begin()) return r.bad("needs");
      while (j.arr_more()) {
        char nm[sizeof(Need::name)] = "", sha[80] = "";
        int kind = -1;
        if (!j.obj_begin()) return r.bad("needs");
        while (j.next_key(k, sizeof k)) {
          if (!strcmp(k, "kind")) { if (!r.str(v, 16, "a need's kind")) return false; kind = need_of(v); }
          else if (!strcmp(k, "name")) { if (!r.str(nm, sizeof nm, "a need's name")) return false; }
          else if (!strcmp(k, "sha256")) { if (!r.str(sha, sizeof sha, "a need's sha256")) return false; }
          else if (!j.skip()) return r.bad("needs");
        }
        if (j.err) return r.bad(j.err);
        uint8_t d[32];
        if (kind < 0) return r.bad("a need of an unknown kind");
        if (!unhex(sha, d, 32)) return r.bad("a need's sha256");
        if (!m->add_need(kind, nm, d)) return r.bad("a bad need (name, the same hash twice, too many)");
      }
      if (j.err) return r.bad(j.err);
    } else if (!strcmp(k, "entries")) {
      have_entries = true;
      if (!j.arr_begin()) return r.bad("entries");
      while (j.arr_more()) {
        char name[kNameMax + 1] = "", sha[80] = "";
        int kind = -1;
        int64_t size = -1;
        int first = m->np, npaths = 0;
        char fmtn[16] = "";
        int64_t fver = 0;
        if (!j.obj_begin()) return r.bad("entries");
        while (j.next_key(k, sizeof k)) {
          if (!strcmp(k, "name")) { if (!r.str(name, sizeof name, "an entry name")) return false; }
          else if (!strcmp(k, "kind")) { if (!r.str(v, 16, "an entry kind")) return false; kind = kind_of(v); }
          else if (!strcmp(k, "size")) { if (!r.num(&size, 0, 0xFFFFFFFFll, "an entry size")) return false; }
          else if (!strcmp(k, "sha256")) { if (!r.str(sha, sizeof sha, "an entry sha256")) return false; }
          else if (!strcmp(k, "format")) {
            if (!j.obj_begin()) return r.bad("an entry format");
            while (j.next_key(k, sizeof k)) {
              if (!strcmp(k, "name")) { if (!r.str(fmtn, sizeof fmtn, "an entry format")) return false; }
              else if (!strcmp(k, "version")) { if (!r.num(&fver, 0, 65535, "an entry format version")) return false; }
              else if (!j.skip()) return r.bad("an entry format");
            }
            if (j.err) return r.bad(j.err);
          } else if (!strcmp(k, "paths")) {
            if (!j.arr_begin()) return r.bad("paths");
            while (j.arr_more()) {
              if (m->np >= m->cap_p) return r.bad("too many paths");
              if (!r.str(m->p[m->np].path, kCardPath, "a path")) return false;
              if (!path_ok(m->p[m->np].path)) return r.bad("an unsafe path");
              m->p[m->np].entry = m->ne;
              ++m->np, ++npaths;
            }
            if (j.err) return r.bad(j.err);
          } else if (!j.skip()) return r.bad("entries");
        }
        if (j.err) return r.bad(j.err);
        if (!name_ok(name)) return r.bad("an unsafe entry name");
        if (kind < 0) return r.bad("an entry of an unknown kind");
        if (size < 0) return r.bad("an entry without a size");
        if (m->find_entry(name) >= 0 || !strcmp(name, "manifest.json")) return r.bad("an entry named twice");
        if (m->ne >= m->cap_e) return r.bad("too many entries");
        MEntry& e = m->e[m->ne];
        memset(&e, 0, sizeof e);
        cpy(e.name, name, sizeof e.name);
        e.kind = (uint8_t)kind;
        e.size = (uint32_t)size;
        e.first_path = first, e.npaths = npaths;
        cpy(e.fmt, fmtn, sizeof e.fmt);
        e.fmt_ver = (int)fver;
        if (!unhex(sha, e.sha, 32)) return r.bad("an entry's sha256");
        if (!entry_fmt_ok(e)) {
          sniprintf(err, cap, "manifest: %s is a %s v%d file, newer than this Sidecar reads", e.name, e.fmt, e.fmt_ver);
          return false;
        }
        ++m->ne;
      }
      if (j.err) return r.bad(j.err);
    } else if (!strcmp(k, "missing")) {
      if (!j.arr_begin()) return r.bad("missing");
      while (j.arr_more()) {
        if (!r.str(v, sizeof v, "missing")) return false;
        m->add_missing(v);   // listed for the summary only: past cap_missing (or unsafe) dropped
      }
      if (j.err) return r.bad(j.err);
    } else if (!j.skip()) {
      return r.bad(j.err ? j.err : "a value");
    }
  }
  if (j.err) return r.bad(j.err);
  j.ws();
  if (j.at != j.n) return r.bad("text after the manifest");
  if (!format) return r.bad("not a Sidecar bundle");
  if (!m->version || !m->min_reader) return r.bad("no version");
  if (!have_entries) return r.bad("no entries");
  if (!have_box || !box_type_ok(m->box_type) || !hex_id(m->box_uid)) return r.bad("no box (type, uid)");
  if (!have_project || !hex_id(m->project_id)) return r.bad("no project id");
  return true;
}

}  // namespace bundle
}  // namespace core

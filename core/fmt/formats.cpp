// core/fmt/formats.cpp - the registry's table (fmt.h): the generic line's card formats at the version their files have
// today, each with a sniffer (bytes -> version) and the current version's reader (bytes -> usable or why not). v1 of a
// format that has no header today is "today's file": its sniffer accepts the bytes as they are and reports version 1.
//
// Entries (port step S8m, decided 2026-11-28; the mechanism from 3a8bf7d): STORE records, the per-box folder layout
// with the old-layout move, a box folder's name.txt and a type folder's last. The code cache and index (S8e), the
// sample pool and instruments (S8f) and the mods' web bundles (S8j) add theirs when they land.
#include <string.h>

#include "../kv/kv.h"
#include "fmt.h"

namespace core {
namespace fmt {

// ---- the per-box folder layout's names (fmt.h)
static int hexv(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
bool uid_name(const char* s, int n) {
  if (n != kUidChars) return false;
  for (int i = 0; i < n; ++i)
    if (hexv(s[i]) < 0) return false;
  return true;
}
bool old_layout_entry(const char* name, int n, bool dir) {
  if (!n || (n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.')) return false;
  if (n == 4 && !memcmp(name, "last", 4)) return false;
  return !(dir && uid_name(name, n));
}

namespace {
bool say(char* why, int cap, const char* m) {
  if (why && cap) sniprintf(why, cap, "%s", m);
  return false;
}
bool printable(const uint8_t* l, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    if (l[i] < 0x20 || l[i] == 0x7f) return false;
  return true;
}
// text formats: "#sidecar <name> N" says N, else today's v1
bool sniff_text(const Bytes& in, const char* name, Sniffed* o) {
  if (text_header(in, name, o)) return true;
  static const char kTag[] = "#sidecar ";
  if (in.n >= sizeof kTag - 1 && !memcmp(in.p, kTag, sizeof kTag - 1)) return false;   // another format's header
  o->version = o->min_reader = 1, o->body = 0;
  return true;
}
// a one-line file's line (from body; its trailing \r, \n and spaces dropped, as core/device's read_small): false when
// more than one line has text
bool one_line(const Bytes& in, uint32_t body, const uint8_t** l, uint32_t* n) {
  uint32_t e = in.n;
  while (e > body && (in.p[e - 1] == '\n' || in.p[e - 1] == '\r' || in.p[e - 1] == ' ')) --e;
  for (uint32_t i = body; i < e; ++i)
    if (in.p[i] == '\n' || in.p[i] == '\r') return false;
  *l = in.p + body, *n = e - body;
  return true;
}

// ---- STORE records (core/kv, host-link 51): a mod's own bytes, no header. v1 is every record on a card today; a
// later version can only be told apart by a header the Pi adds (named gap: no room to detect a v2 in v1's bytes)
bool sn_store(const Bytes& in, Sniffed* o) {
  o->version = o->min_reader = 1, o->body = 0;
  return true;
}
bool rd_store(const Bytes& in, const Sniffed&, char* why, int cap) {
  return in.n <= kv::kMaxRecord ? true : say(why, cap, "longer than a record");
}

// ---- the per-box folder layout (fmt.h): a listing, one name a line ('/' at the end: a folder; "<uid>/<name>": an
// entry of a UID folder). v1 while any top-level entry is the old layout's; v2 (current) when none is - an empty or
// absent type folder too.
struct Line {
  const uint8_t* p;
  uint32_t n;      // without the trailing '/'
  bool dir;
  int slash;       // where "<uid>/" ends inside (-1: a top-level entry)
};
// the next line from *at: false at the end; ok false for a line no listing has
bool next(const Bytes& in, uint32_t* at, Line* l, bool* ok) {
  *ok = true;
  while (*at < in.n && (in.p[*at] == '\n' || in.p[*at] == '\r')) ++*at;   // (blank lines: none)
  if (*at >= in.n) return false;
  uint32_t a = *at, b = a;
  while (b < in.n && in.p[b] != '\n') ++b;
  *at = b;
  uint32_t e = b;
  if (e > a && in.p[e - 1] == '\r') --e;
  l->p = in.p + a, l->dir = e > a && in.p[e - 1] == '/';
  l->n = e - a - (l->dir ? 1 : 0);
  l->slash = -1;
  if (!l->n || !printable(l->p, l->n)) return *ok = false, true;
  for (uint32_t i = 0; i < l->n; ++i)
    if (l->p[i] == '/') {
      if (l->slash >= 0 || i != kUidChars || !uid_name((const char*)l->p, kUidChars) || i + 1 == l->n) return *ok = false, true;
      l->slash = (int)i;
    }
  return true;
}
// the listing's version (0: not a listing); *olds the old-layout entries
uint16_t layout_version(const Bytes& in, int* olds) {
  if (memchr(in.p, 0, in.n)) return 0;
  int k = 0;
  Line l;
  bool ok;
  for (uint32_t at = 0; next(in, &at, &l, &ok);) {
    if (!ok) return 0;
    if (l.slash < 0 && old_layout_entry((const char*)l.p, (int)l.n, l.dir)) ++k;
  }
  if (olds) *olds = k;
  return k ? 1 : 2;
}
bool sn_layout(const Bytes& in, Sniffed* o) {
  const uint16_t v = layout_version(in, nullptr);
  if (!v) return false;
  o->version = o->min_reader = v, o->body = 0;
  return true;
}
bool rd_layout(const Bytes& in, const Sniffed&, char* why, int cap) {
  int olds = 0;
  if (!layout_version(in, &olds)) return say(why, cap, "not a folder listing");
  return olds ? say(why, cap, "old-layout entries left") : true;
}
// v1 -> v2, the move as a listing: every old top-level entry becomes "<uid>/<entry>", "<uid>/" listed once (after the
// entries that stay, before what moved into it); the rest as it was. c->key: the box's UID (8 hex; lower-cased here)
bool up_layout_1(const Bytes& in, Buf* out, Ctx* c) {
  if (!c || !c->key || strlen(c->key) != (size_t)kUidChars || !uid_name(c->key, kUidChars)) return false;
  char u[kUidChars + 1];
  for (int i = 0; i < kUidChars; ++i) u[i] = (char)(c->key[i] >= 'A' && c->key[i] <= 'F' ? c->key[i] + 32 : c->key[i]);
  u[kUidChars] = 0;
  uint32_t o = 0;
  auto put = [&](const void* d, uint32_t n) {
    if (o + n > out->cap) return false;
    memcpy(out->p + o, d, n);
    o += n;
    return true;
  };
  Line l;
  bool ok, have = false;
  for (int pass = 0; pass < 2; ++pass) {   // 0: what stays (and whether <uid>/ is there); 1: what moves
    for (uint32_t at = 0; next(in, &at, &l, &ok);) {
      if (!ok) return false;
      const bool old = l.slash < 0 && old_layout_entry((const char*)l.p, (int)l.n, l.dir);
      if (pass == 0 && !old) {
        have = have || (l.slash < 0 && l.dir && l.n == (uint32_t)kUidChars && !memcmp(l.p, u, kUidChars));
        if (!put(l.p, l.n) || (l.dir && !put("/", 1)) || !put("\n", 1)) return false;
      } else if (pass == 1 && old) {
        if (!put(u, kUidChars) || !put("/", 1) || !put(l.p, l.n) || (l.dir && !put("/", 1)) || !put("\n", 1)) return false;
      }
    }
    if (pass == 0 && !have && (!put(u, kUidChars) || !put("/\n", 2))) return false;
  }
  out->n = o;
  return true;
}
const Upgrade kLayoutChain[] = {{1, up_layout_1}};

// ---- a box folder's name.txt: the box's name for the user, one line (core/device's Link::label: 23 characters)
bool sn_boxname(const Bytes& in, Sniffed* o) { return sniff_text(in, "boxname", o); }
bool rd_boxname(const Bytes& in, const Sniffed& s, char* why, int cap) {
  const uint8_t* l;
  uint32_t n;
  if (!one_line(in, s.body, &l, &n)) return say(why, cap, "more than one line");
  if (!n) return say(why, cap, "empty");
  if (n > 23) return say(why, cap, "a name too long");
  return printable(l, n) ? true : say(why, cap, "a control character");
}
// ---- a type folder's last: the UID attached last, 8 hex digits
bool sn_boxlast(const Bytes& in, Sniffed* o) { return sniff_text(in, "boxlast", o); }
bool rd_boxlast(const Bytes& in, const Sniffed& s, char* why, int cap) {
  const uint8_t* l;
  uint32_t n;
  if (!one_line(in, s.body, &l, &n) || !uid_name((const char*)l, (int)n)) return say(why, cap, "not a UID (8 hex digits)");
  return true;
}

const Format kFormats[] = {
    // name, class, shape, current, oldest, native, sniff, read, chain, nchain, min_reader_of, where, max bytes
    {"store", kProject, kBinary, 1, 1, 1, sn_store, rd_store, nullptr, 0, nullptr,
     "/.rytm/store/<project, 8 hex>/<scope>-<handle, 4 hex>.bin (per box from S8a: <box folder>/store/...)", kv::kMaxRecord},
    {"boxlayout", kProject, kText, 2, 1, 2, sn_layout, rd_layout, kLayoutChain, 1, nullptr,
     "<type folder> (/.rytm, /.a4, /.dt): its listing; v2 a box's state in <type folder>/<uid>/", 64 * 1024},
    {"boxname", kSetting, kText, 1, 1, 1, sn_boxname, rd_boxname, nullptr, 0, nullptr, "<type folder>/<uid>/name.txt", 64},
    {"boxlast", kSetting, kText, 1, 1, 1, sn_boxlast, rd_boxlast, nullptr, 0, nullptr, "<type folder>/last", 32},
};
}  // namespace

int count() { return (int)(sizeof kFormats / sizeof kFormats[0]); }
const Format* at(int i) { return i >= 0 && i < count() ? &kFormats[i] : nullptr; }

}  // namespace fmt
}  // namespace core

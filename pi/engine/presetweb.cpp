// pi/engine/presetweb.cpp - see presetweb.h.
#include "presetweb.h"

#include <stdint.h>
#include <string.h>

#include "platform.h"
#include "presets/preset_index.h"

namespace presetweb {

namespace {

namespace P = core::presets;
constexpr int kMaxRows = 200;

// the value of name in "a=1&b=2" (no decoding: the page sends plain tokens), "" when absent
void param(const char* q, const char* name, char* out, int cap) {
  out[0] = 0;
  int nl = (int)strlen(name);
  for (const char* p = q; p && *p;) {
    if (!strncmp(p, name, nl) && p[nl] == '=') {
      p += nl + 1;
      int n = 0;
      while (*p && *p != '&' && n < cap - 1) out[n++] = *p++;
      out[n] = 0;
      return;
    }
    p = strchr(p, '&');
    if (p) ++p;
  }
}
int number(const char* s, int dflt, int lo, int hi) {
  bool neg = *s == '-';
  if (neg) ++s;
  if (*s < '0' || *s > '9') return dflt;
  int v = 0;
  while (*s >= '0' && *s <= '9' && v < 100000) v = v * 10 + (*s++ - '0');
  v = neg ? -v : v;
  return v < lo ? lo : v > hi ? hi : v;
}

struct Out {
  char* b;
  int cap, n;
  bool full;
  void raw(const char* s, int len = -1) {
    if (len < 0) len = (int)strlen(s);
    if (n + len >= cap) { full = true; return; }
    memcpy(b + n, s, len);
    n += len;
    b[n] = 0;
  }
  void num(int v) {
    char t[16];
    sniprintf(t, sizeof t, "%d", v);
    raw(t);
  }
  void str(const char* s) {   // a JSON string: quotes, backslashes and control characters escaped
    raw("\"");
    for (; *s; ++s) {
      unsigned char c = (unsigned char)*s;
      if (c == '"' || c == '\\') { char e[3] = {'\\', (char)c, 0}; raw(e); }
      else if (c < 0x20) { char e[8]; sniprintf(e, sizeof e, "\\u%04x", c); raw(e); }
      else raw(s, 1);
    }
    raw("\"");
  }
};

}  // namespace

int json(const char* params, char* out, int cap) {
  if (cap < 512) return 0;
  const char* q = params ? params : "";
  char kt[8], ft[12], ot[12], nt[8];
  param(q, "k", kt, sizeof kt), param(q, "f", ft, sizeof ft), param(q, "o", ot, sizeof ot), param(q, "n", nt, sizeof nt);
  int kind = !strcmp(kt, "fm") ? P::kIxFm : P::kIxJuno;
  int folder = number(ft, -1, -1, P::kMaxFolders);
  int first = number(ot, 0, 0, 100000), max = number(nt, 50, 1, kMaxRows);
  static char names[kMaxRows][P::kViewName];
  static uint8_t flags[kMaxRows];
  static int16_t kids[kMaxRows];   // (core 0's web tasks take turns: Circle's scheduler is cooperative)
  P::View v;
  int n = P::view(folder, kind, first, max, names, flags, kids, &v);
  Out o{out, cap - 1, 0, false};
  out[0] = 0;
  o.raw("{\"ready\":"), o.raw(n >= 0 ? "true" : "false");
  o.raw(",\"gen\":"), o.num((int)(n >= 0 ? v.gen : P::gen()));
  o.raw(",\"k\":"), o.str(kind == P::kIxFm ? "fm" : "juno");
  o.raw(",\"f\":"), o.num(folder);
  o.raw(",\"o\":"), o.num(first);
  char mt[48];
  P::Result r = P::result();
  P::result_text(mt, sizeof mt);
  if (n >= 0) {
    o.raw(",\"total\":"), o.num(v.total);
    o.raw(",\"up\":"), o.num(v.up);
    o.raw(",\"title\":"), o.str(v.title);
    o.raw(",\"builtin\":"), o.raw(v.built_in ? "true" : "false");
    o.raw(",\"full\":"), o.raw(v.full ? "true" : "false");
    o.raw(",\"rows\":[");
    for (int i = 0; i < n; ++i) {
      if (i) o.raw(",");
      o.raw("["), o.str(names[i]), o.raw(","), o.num(flags[i]), o.raw("]");
    }
    o.raw("],\"kids\":[");
    for (int i = 0; i < n; ++i) {
      if (i) o.raw(",");
      o.num(kids[i]);
    }
    o.raw("]");
  }
  o.raw(",\"counts\":["), o.num(P::rows(P::kIxJuno)), o.raw(","), o.num(P::rows(P::kIxFm));
  o.raw("],\"msg\":{\"seq\":"), o.num((int)r.seq), o.raw(",\"text\":"), o.str(mt), o.raw("}}");
  return o.full ? 0 : o.n;
}

}  // namespace presetweb

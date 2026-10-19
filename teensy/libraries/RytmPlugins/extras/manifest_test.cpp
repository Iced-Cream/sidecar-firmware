// extras/manifest_test.cpp - manifests (formats 1..3) as slot descriptions, on the Mac.
// build: c++ -std=c++17 -O1 -Wall -I../src manifest_test.cpp ../src/rmp_manifest.cpp ../src/rmp_loader.cpp -o /tmp/manifest_test && /tmp/manifest_test
// (run from extras/; argv[1] overrides the path of sine_test.rmp)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "rmp_manifest.h"
#include "rmp_format.h"

static std::vector<uint8_t> pkg;   // a fake package: 96 header bytes, an 8-byte image, the manifest
static int reads;
static bool rd(void *, uint32_t off, uint8_t *buf, uint32_t n) {
  if ((uint64_t)off + n > pkg.size()) return false;
  ++reads; memcpy(buf, pkg.data() + off, n); return true;
}
static void put(std::vector<uint8_t> &v, const char *s, size_t n) {
  for (size_t i = 0; i < n; ++i) v.push_back(i < strlen(s) ? (uint8_t)s[i] : 0);
}
struct K { int p, look, text, first, count, role, lo, hi, def; const char *lng, *sht; };
// A manifest in format fmt with these knobs and names (8 bytes each).
static rmp_info make(int fmt, const char *name, const char *shrt, int page2, const char *title, const char *ext,
                     const std::vector<K> &ks, const std::vector<std::string> &names) {
  std::vector<uint8_t> m;
  put(m, name, 10); put(m, shrt, 4); put(m, "MIT", 48); m.push_back((uint8_t)page2); m.push_back((uint8_t)ks.size());
  if (fmt >= 3) { put(m, title, 13); put(m, ext, 7); }
  for (const K &k : ks) {
    m.push_back((uint8_t)k.p); m.push_back((uint8_t)k.look); m.push_back((uint8_t)k.text);
    if (fmt == 1) m.push_back(0);
    else { m.push_back((uint8_t)k.first); m.push_back((uint8_t)k.count); }
    if (fmt == 2) m.push_back(0);
    if (fmt >= 3) { m.push_back((uint8_t)k.role); m.push_back((uint8_t)k.lo); m.push_back((uint8_t)k.hi); m.push_back((uint8_t)k.def); m.push_back(0); }
    put(m, k.lng, 12); put(m, k.sht, 4);
  }
  if (fmt >= 2) for (const std::string &n : names) put(m, n.c_str(), 8);
  pkg.assign(96 + 8, 0);
  pkg.insert(pkg.end(), m.begin(), m.end());
  rmp_info info; memset(&info, 0, sizeof info);
  info.format = (uint16_t)fmt; info.image_size = 8; info.reloc_count = 0; info.manifest_size = (uint32_t)m.size();
  return info;
}
static bool str_is(const char *f, size_t n, const char *want) {   // NUL-padded field == want
  char b[32] = {0}; memcpy(b, f, n); return !strcmp(b, want);
}

int main(int argc, char **argv) {
  rmp_desc d;
  {   // format 3: everything, knobs out of p order, a preset knob, ranges, roles, names
    rmp_info i = make(3, "NP CLUST1", "NPC1", 1, "NP PAGE 2", "np",
                      {{9, 0, 0, 0, 0, 0, 0, 127, 64, "Gain", "GAN"},
                       {1, 3, 2, 0, 2, 1, 0, 127, 0, "Algorithm", "ALG"},
                       {2, 0, 6, 0, 0, 0, 0, 127, 0, "Preset", "PRE"},
                       {3, 2, 0, 0, 0, 2, 10, 120, 100, "Level", "LEV"},
                       {5, 0, 7, 0, 0, 0, 0, 127, 0, "Sample", "SMP"}},
                      {"PrimeCl", "PrimeCn"});
    CHECK(rmp_manifest_read(&i, rd, nullptr, &d));
    CHECK(str_is(d.name, 10, "NP CLUST1") && str_is(d.short_name, 4, "NPC1") && str_is(d.p2_title, 13, "NP PAGE 2"));
    CHECK(!strcmp(d.preset_ext, "np") && d.page2 == 1 && d.nknobs == 5 && d.format == 3);
    CHECK(d.knob[0].p == 1 && d.knob[1].p == 2 && d.knob[2].p == 3 && d.knob[3].p == 5 && d.knob[4].p == 9);   // p order
    CHECK(d.pre == 1 && d.knob[1].kind == RMP_TEXT_PRESET);
    CHECK(d.knob[0].kind == RMP_TEXT_ENUM && d.knob[0].look == RMP_LOOK_LIST && d.knob[0].role == RMP_ROLE_PRESET);
    CHECK(d.knob[2].min == 10 && d.knob[2].max == 120 && d.knob[2].def == 100 && d.knob[2].role == RMP_ROLE_OFFSET);
    CHECK(d.knob[3].kind == RMP_TEXT_POOL && str_is(d.knob[3].long_name, 12, "Sample") && str_is(d.knob[3].short_name, 4, "SMP"));
    CHECK(rmp_desc_find(&d, 5) == 3 && rmp_desc_find(&d, 4) == -1);
    char nm[16][8];
    CHECK(rmp_manifest_enum(&d, 0, rd, nullptr, nm) == 2 && !strcmp(nm[0], "PrimeCl") && !strcmp(nm[1], "PrimeCn"));
    CHECK(rmp_manifest_enum(&d, 2, rd, nullptr, nm) == 0);   // no names
  }
  {   // format 2: format 3's defaults, the name table
    rmp_info i = make(2, "SINE TEST", "SINE", 0, "", "", {{1, 3, 2, 0, 3, 0, 0, 0, 0, "Mode", "MOD"}}, {"A", "B", "C"});
    CHECK(rmp_manifest_read(&i, rd, nullptr, &d));
    CHECK(d.nknobs == 1 && d.knob[0].min == 0 && d.knob[0].max == 127 && d.knob[0].def == 64 && d.knob[0].role == 0);
    CHECK(d.p2_title[0] == 0 && d.preset_ext[0] == 0 && d.pre == -1);
    char nm[16][8];
    CHECK(rmp_manifest_enum(&d, 0, rd, nullptr, nm) == 3 && !strcmp(nm[2], "C"));
  }
  {   // format 1: no names
    rmp_info i = make(1, "OLD", "OLD", 0, "", "", {{2, 1, 1, 0, 0, 0, 0, 0, 0, "Tune", "TUN"}}, {});
    CHECK(rmp_manifest_read(&i, rd, nullptr, &d));
    CHECK(d.nknobs == 1 && d.knob[0].p == 2 && d.knob[0].look == RMP_LOOK_CENTRED && d.knob[0].kind == RMP_TEXT_BIPOLAR);
    CHECK(d.names_at == 0);
    char nm[16][8];
    CHECK(rmp_manifest_enum(&d, 0, rd, nullptr, nm) == 0);
  }
  {   // skipped knobs: p 0, p 17, a P seen before; unprintable bytes made '_'; a bad range repaired
    rmp_info i = make(3, "BAD\x01NAME", "B\x7f", 0, "T", "n.p",
                      {{0, 0, 0, 0, 0, 0, 0, 127, 64, "Zero", "Z"}, {17, 0, 0, 0, 0, 0, 0, 127, 64, "Big", "B"},
                       {4, 0, 0, 0, 0, 0, 90, 20, 5, "Four", "F"}, {4, 0, 0, 0, 0, 0, 0, 127, 64, "Again", "A"}}, {});
    CHECK(rmp_manifest_read(&i, rd, nullptr, &d));
    CHECK(str_is(d.name, 10, "BAD_NAME") && str_is(d.short_name, 4, "B_"));
    CHECK(d.preset_ext[0] == 0);                                   // "n.p": not letters and digits
    CHECK(d.nknobs == 1 && d.knob[0].p == 4 && str_is(d.knob[0].long_name, 12, "Four"));
    CHECK(d.knob[0].min == 0 && d.knob[0].max == 127 && d.knob[0].def == 64);   // min > max: the full range
  }
  {   // full-width strings: every field NUL-terminated (one spare byte each), even with no padding left
    rmp_info i = make(3, "ABCDEFGHIJ", "WXYZ", 1, "THIRTEENCHARS", "abcdefg",
                      {{1, 0, 2, 0, 1, 0, 0, 127, 64, "TwelveLetter", "FOUR"}}, {"EIGHTCHR"});
    CHECK(rmp_manifest_read(&i, rd, nullptr, &d));
    CHECK(strlen(d.name) == 10 && !strcmp(d.name, "ABCDEFGHIJ"));
    CHECK(strlen(d.short_name) == 4 && !strcmp(d.short_name, "WXYZ"));
    CHECK(strlen(d.p2_title) == 13 && !strcmp(d.p2_title, "THIRTEENCHARS"));
    CHECK(strlen(d.preset_ext) == 7 && !strcmp(d.preset_ext, "abcdefg"));
    CHECK(strlen(d.knob[0].long_name) == 12 && !strcmp(d.knob[0].long_name, "TwelveLetter"));
    CHECK(strlen(d.knob[0].short_name) == 4 && !strcmp(d.knob[0].short_name, "FOUR"));
    char nm[16][8];
    CHECK(rmp_manifest_enum(&d, 0, rd, nullptr, nm) == 1 && !strcmp(nm[0], "EIGHTCH"));   // at most 7
    rmp_desc_from_id("x.ABCDEFGHIJKLMN", &d);
    CHECK(strlen(d.name) == 10 && !strcmp(d.name, "ABCDEFGHIJ") && strlen(d.short_name) == 4);
  }
  {   // range bytes 128..255 saturate to 127 (not masked)
    rmp_info i = make(3, "SAT", "SAT", 0, "", "",
                      {{1, 0, 0, 0, 0, 0, 100, 200, 255, "Hi", "HI"},      // 100..127, default 127
                       {2, 0, 0, 0, 0, 0, 0, 130, 129, "Top", "TOP"},      // 0..127, default 127 (& 127 would give 0..2, 1)
                       {3, 0, 0, 0, 0, 0, 200, 50, 30, "Bad", "BAD"}},     // 127 > 50: the full range, default 64
                      {});
    CHECK(rmp_manifest_read(&i, rd, nullptr, &d));
    CHECK(d.knob[0].min == 100 && d.knob[0].max == 127 && d.knob[0].def == 127);
    CHECK(d.knob[1].min == 0 && d.knob[1].max == 127 && d.knob[1].def == 127);
    CHECK(d.knob[2].min == 0 && d.knob[2].max == 127 && d.knob[2].def == 64);
  }
  {   // malformed: the knob count does not fit the manifest; another format; a failing read
    rmp_info i = make(3, "X", "X", 0, "", "", {{1, 0, 0, 0, 0, 0, 0, 127, 64, "A", "A"}}, {});
    rmp_info t = i; t.manifest_size -= 1; CHECK(!rmp_manifest_read(&t, rd, nullptr, &d));
    t = i; t.format = 4; CHECK(!rmp_manifest_read(&t, rd, nullptr, &d));
    t = i; t.image_size = 100000; CHECK(!rmp_manifest_read(&t, rd, nullptr, &d));
  }
  {   // a missing machine's description from its id
    rmp_desc_from_id("befaco.np.atari", &d);
    CHECK(str_is(d.name, 10, "ATARI") && str_is(d.short_name, 4, "ATAR") && d.nknobs == 0 && d.pre == -1);
    rmp_desc_from_id("plainidentifier1", &d);
    CHECK(str_is(d.name, 10, "PLAINIDENT") && str_is(d.short_name, 4, "PLAI"));
  }
  {   // the real sine_test package (format 3 since Task 3), read in pieces
    const char *path = argc > 1 ? argv[1] : "../../../../plugins/sine_test/sine_test.rmp";
    FILE *f = fopen(path, "rb"); CHECK(f);
    pkg.clear(); uint8_t b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) pkg.insert(pkg.end(), b, b + n);
    fclose(f);
    rmp_info i; CHECK(rmp_verify_header(pkg.data(), (uint32_t)pkg.size(), &i) == RMP_OK);
    reads = 0;
    CHECK(rmp_manifest_read(&i, rd, nullptr, &d));
    CHECK(str_is(d.name, 10, "SINE TEST") && d.nknobs == 3 && d.knob[0].kind == RMP_TEXT_BIPOLAR && d.format == 3);
    CHECK(reads == 1 + 3);   // the head, then a knob a read: never the package whole
  }
  printf("ok\n");
  return 0;
}

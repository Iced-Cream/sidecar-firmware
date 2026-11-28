// core/tests/test_bundle_zip.cpp - a box project's bundle (port step S8n; the container from 3a8bf7d's bundles phase 1,
// docs/BUNDLES.md "Format"): SHA-256 against FIPS 180-4's examples, CRC-32, the stored zip's layout (4096-aligned data,
// the 0xD935 padding), any byte range of the archive equal to the whole, byte-exact against a fixed reference, read by
// Python's zipfile and `unzip -t` (each skipped, said so, when not installed), the reader's checks (every region's
// damage refused), the 4 GiB refusal; the box project's manifest (the registry's "bundle" v2) written and read back,
// the old line's v1 and a newer bundle refused, its bounded reader against truncations, huge numbers and deep nesting;
// the round trip: entries -> manifest -> zip -> read back, checked against the manifest, every entry's hashes matched.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "bundle/json.h"
#include "bundle/manifest.h"
#include "bundle/pack.h"
#include "fmt/fmt.h"
#include "lib/sha256.h"
#include "bundle/zip.h"
#include "check.h"

using namespace core::bundle;

static std::string hexs(const uint8_t* d, int n) {
  char b[129];
  hex(d, n, b);
  return b;
}
static std::string sha(const std::string& s) {
  uint8_t d[32];
  sha256(s.data(), s.size(), d);
  return hexs(d, 32);
}

struct Src {
  std::vector<std::string>* data;
};
static bool data_fn(void* ctx, int i, uint32_t off, uint8_t* buf, uint32_t n) {
  auto& d = *((Src*)ctx)->data;
  if (off + n > d[i].size()) return false;
  memcpy(buf, d[i].data() + off, n);
  return true;
}
static std::string whole(std::vector<ZEntry>& e, std::vector<std::string>& d, Layout* L, uint32_t epoch) {
  uint16_t dd, dt;
  dos_time(epoch, &dd, &dt);
  for (size_t i = 0; i < e.size(); ++i) e[i].size = (uint32_t)d[i].size(), e[i].crc = crc32(0, d[i].data(), d[i].size());
  CHECK(layout(e.data(), (int)e.size(), dd, dt, L));
  std::string z(L->total, '\0');
  Src s{&d};
  CHECK(render(e.data(), *L, 0, (uint8_t*)&z[0], (uint32_t)z.size(), data_fn, &s));
  return z;
}
static ZEntry ze(const char* n) {
  ZEntry e;
  memset(&e, 0, sizeof e);
  snprintf(e.name, sizeof e.name, "%s", n);
  return e;
}
static bool read_all(const std::string& z, std::vector<ZEntry>& out, std::string* err) {
  char e[160] = "";
  ZRead r;
  uint32_t t = z.size() < kEocdTail ? (uint32_t)z.size() : kEocdTail;
  if (!eocd((const uint8_t*)z.data() + z.size() - t, t, z.size(), &r, e, sizeof e)) return *err = e, false;
  out.assign(r.n, ZEntry());
  if (!central((const uint8_t*)z.data() + r.cd_off, r, out.data(), r.n, e, sizeof e)) return *err = e, false;
  for (int i = 0; i < r.n; ++i) {
    uint64_t next = i + 1 < r.n ? out[i + 1].local : r.cd_off;
    uint64_t avail = z.size() - out[i].local;
    if (!local((const uint8_t*)z.data() + out[i].local, (uint32_t)(avail < kLocalMax ? avail : kLocalMax), &out[i], next, e, sizeof e))
      return *err = e, false;
  }
  return true;
}

static bool have_python = false;
// a box project's manifest: the A4's project 0000beef, its box stream, two STORE records, one pool sample
static void fill_manifest(Manifest* m) {
  m->clear();
  strcpy(m->created, "2026-11-28T21:30:00Z");
  strcpy(m->clock, "browser");
  strcpy(m->sc_version, "0.14.0");
  strcpy(m->board, "pi4");
  strcpy(m->box_type, "a4");
  strcpy(m->box_uid, "3fa2c001");
  strcpy(m->box_name, "A4 STUDIO");
  strcpy(m->project_id, "0000beef");
  strcpy(m->project_name, "PERC \"LIVE\"");
  uint8_t h[32];
  sha256("tgain", 5, h);
  CHECK(m->add_need(kNeedCode, "tgain", h));
  sha256("www", 3, h);
  CHECK(m->add_need(kNeedWww, "lfo-page", h));
  CHECK(m->add_entry("box/project.bin", kKindBox) == 0);
  int s1 = m->add_entry("store/1-0001.bin", kKindStore), s2 = m->add_entry("store/0-0002.bin", kKindStore);
  CHECK(s1 == 1 && s2 == 2);
  strcpy(m->e[s1].fmt, "store"), m->e[s1].fmt_ver = 1;
  strcpy(m->e[s2].fmt, "store"), m->e[s2].fmt_ver = 1;
  int k = m->add_entry("samples/Drums/kick.wav", kKindSample);
  CHECK(k == 3 && m->add_path(k, "/Samples/Drums/kick.wav") && m->add_path(k, "/Samples/Old/kick copy.wav"));
  m->e[k].size = 5000;
  CHECK(m->add_missing("/Samples/gone.wav"));
}

int main() {
  have_python = system("python3 -c 1 >/dev/null 2>&1") == 0;
  if (!have_python) printf("test_bundle_zip: python3 not found: its zipfile / json checks skipped\n");
  // ---- SHA-256 (FIPS 180-4 examples), any split of the input the same
  CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  {
    std::string m(1000000, 'a');
    CHECK(sha(m) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    Sha256 s;
    s.init();
    for (size_t at = 0, k = 1; at < m.size(); at += k, k = k * 7 % 997 + 1) s.update(m.data() + at, std::min(k, m.size() - at));
    uint8_t d[32];
    s.final(d);
    CHECK(hexs(d, 32) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  }
  uint8_t u[32];
  CHECK(unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", u, 32) && hexs(u, 32)[0] == 'b');
  CHECK(!unhex("zz", u, 1) && !unhex("abc", u, 1));
  // ---- CRC-32 (the check value), in pieces
  CHECK(crc32(0, "123456789", 9) == 0xCBF43926u);
  CHECK(crc32(crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);

  // ---- names
  for (const char* ok : {"manifest.json", "files/Samples/Drums/kick.wav", "files/Samples/caf\xc3\xa9.wav", "rytm/AR PERC.arprj"})
    CHECK(name_ok(ok));
  for (const char* bad : {"", "/abs", "a/../b", "..", "a/./b", "a//b", "a/", "a\\b", "c:x", "a\nb", "a\x7f"})
    CHECK(!name_ok(bad));
  CHECK(!name_ok(std::string(201, 'a').c_str()) && name_ok(std::string(200, 'a').c_str()));

  // ---- a stored zip: aligned, any range equal to the whole, byte-exact, read back
  std::vector<ZEntry> e = {ze("manifest.json"), ze("project/pool.txt"), ze("files/Samples/a b.wav"), ze("empty.txt")};
  std::vector<std::string> d = {"{\"format\":\"sidecar-bundle\"}\n", "kick.wav\n\nsnare.wav\n", std::string(10000, 'x'), ""};
  for (int i = 0; i < 10000; ++i) d[2][i] = (char)(i * 31 + 7);
  Layout L;
  std::string z = whole(e, d, &L, 1792677780u);   // 2026-10-22 14:03:00 UTC
  for (auto& x : e) CHECK(x.data % kAlign == 0 && z.compare(x.data, x.size, d[&x - &e[0]]) == 0);
  CHECK(z.size() == L.total && L.cd_off + L.cd_size + 22 == L.total);
  printf("test_bundle_zip: 4 entries, %llu bytes, sha256 %s\n", (unsigned long long)z.size(), sha(z).c_str());
  CHECK(sha(z) == "2699c2fc16f18f099b14ec34667d04057b01fd7f712893af95cec329af06a6a4");   // the fixed reference (checked by Python's zipfile and unzip -t below)
  {
    Src s{&d};
    std::vector<uint8_t> b(z.size());
    for (uint32_t step : {1u, 7u, 4096u, 4093u, 65536u})
      for (uint64_t off = 0; off < z.size(); off += step) {
        uint32_t n = (uint32_t)std::min<uint64_t>(step, z.size() - off);
        CHECK(render(e.data(), L, off, b.data(), n, data_fn, &s));
        CHECK(memcmp(b.data(), z.data() + off, n) == 0);
      }
    CHECK(!render(e.data(), L, z.size() - 1, b.data(), 2, data_fn, &s));
  }
  {
    std::vector<ZEntry> r;
    std::string err;
    CHECK(read_all(z, r, &err));
    CHECK(r.size() == 4);
    for (size_t i = 0; i < 4; ++i) CHECK(!strcmp(r[i].name, e[i].name) && r[i].data == e[i].data && r[i].size == e[i].size && r[i].crc == e[i].crc);
  }
  // Python's zipfile and unzip read it: every entry, its CRC, the alignment
  char tmpl[] = "/tmp/bzip-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string dir = tmpl, path = dir + "/t.sidecar.zip";
  FILE* f = fopen(path.c_str(), "wb");
  fwrite(z.data(), 1, z.size(), f);
  fclose(f);
  std::string py = "python3 -I -c \"import zipfile,sys\nz=zipfile.ZipFile(sys.argv[1])\nassert z.testzip() is None\n"
                   "n=[i.filename for i in z.infolist()]\nassert n==['manifest.json','project/pool.txt','files/Samples/a b.wav','empty.txt'],n\n"
                   "assert all(i.compress_type==0 for i in z.infolist())\nassert z.read('project/pool.txt')==b'kick.wav\\n\\nsnare.wav\\n'\n"
                   "import struct\nb=open(sys.argv[1],'rb').read()\n"
                   "for i in z.infolist():\n h=b[i.header_offset:i.header_offset+30]\n nl,xl=struct.unpack('<HH',h[26:30])\n"
                   " assert (i.header_offset+30+nl+xl)%4096==0\n x=b[i.header_offset+30+nl:i.header_offset+30+nl+4]\n"
                   " assert struct.unpack('<H',x[:2])[0]==0xD935\n assert i.flag_bits&0x800 and not i.flag_bits&8\" '" + path + "'";
  if (have_python) CHECK(system(py.c_str()) == 0);
  if (system("command -v unzip >/dev/null 2>&1") == 0) CHECK(system(("unzip -tq '" + path + "' >/dev/null").c_str()) == 0);
  else printf("test_bundle_zip: unzip not found: unzip -t skipped\n");

  // ---- damage in each region refused: the end record, the central directory, a local header; data damage is the
  // CRC / sha256's (the import's hash pass)
  {
    std::vector<ZEntry> r;
    std::string err;
    std::vector<uint64_t> spots = {z.size() - 22, z.size() - 10, L.cd_off, L.cd_off + 10, L.cd_off + 20, L.cd_off + 42,
                                   e[1].local, e[1].local + 14, e[1].local + 18, e[1].local + 26, e[2].local + 31};
    for (uint64_t at : spots) {
      std::string bad = z;
      bad[at] ^= 0x41;
      CHECK(!read_all(bad, r, &err));
    }
    CHECK(!read_all(z.substr(0, z.size() - 1), r, &err));
    CHECK(!read_all(z + "x", r, &err));
    CHECK(!read_all("PK", r, &err));
  }
  // a zip by another tool: deflated, data descriptors -> refused with the reason
  if (have_python) {
    std::string p2 = dir + "/other.zip";
    CHECK(system(("python3 -I -c \"import zipfile\nz=zipfile.ZipFile('" + p2 + "','w',zipfile.ZIP_DEFLATED)\nz.writestr('a.txt','hello '*100)\nz.close()\"").c_str()) == 0);
    FILE* g = fopen(p2.c_str(), "rb");
    std::string o;
    char b[4096];
    for (size_t n; (n = fread(b, 1, sizeof b, g)) > 0;) o.append(b, n);
    fclose(g);
    std::vector<ZEntry> r;
    std::string err;
    CHECK(!read_all(o, r, &err) && err.find("compressed") != std::string::npos);
  }
  // ---- the 4 GiB limit: refused by the layout (sizes only, nothing written)
  {
    std::vector<ZEntry> big = {ze("manifest.json"), ze("files/a.wav"), ze("files/b.wav")};
    big[0].size = 1000, big[1].size = 0xFFFF0000u, big[2].size = 0x10000;
    Layout B;
    CHECK(!layout(big.data(), 3, 0, 0, &B));
    big[2].size = 0x1000;
    CHECK(!layout(big.data(), 3, 0, 0, &B));   // 4 GiB - 64 KiB at most, headers counted
    big[1].size = 0xFFFE0000u - 3 * kAlign;
    CHECK(layout(big.data(), 3, 0, 0, &B) && B.total <= kZipMax);
  }

  // ---- the box project's manifest (S8n): written, read back, written again: the same text
  static char t1[1 << 16], t2[1 << 16];
  char err[200];
  Manifest m{}, m2{};
  m.alloc(64, 64, 8);
  m2.alloc(64, 64, 8);
  fill_manifest(&m);
  JOut o;
  o.begin(t1, sizeof t1);
  CHECK(write_manifest(m, o));
  CHECK(strstr(t1, "\"format\":\"sidecar-bundle\",\"version\":2,\"min_reader\":2"));   // the registry's row
  CHECK(read_manifest(t1, o.n, &m2, err, sizeof err));
  CHECK(!m2.read_only && m2.version == 2 && m2.min_reader == 2);
  JOut o2;
  o2.begin(t2, sizeof t2);
  CHECK(write_manifest(m2, o2));
  CHECK(o.n == o2.n && !memcmp(t1, t2, o.n));
  CHECK(m2.ne == 4 && m2.e[3].npaths == 2 && !strcmp(m2.p[m2.e[3].first_path + 1].path, "/Samples/Old/kick copy.wav"));
  CHECK(!strcmp(m2.box_type, "a4") && !strcmp(m2.box_uid, "3fa2c001") && !strcmp(m2.project_id, "0000beef"));
  CHECK(!strcmp(m2.project_name, "PERC \"LIVE\"") && !strcmp(m2.e[1].fmt, "store") && m2.e[1].fmt_ver == 1);
  CHECK(m2.nneeds == 2 && m2.needs[1].kind == kNeedWww && !memcmp(m2.needs[0].sha, m.needs[0].sha, 32));
  CHECK(m2.nmissing == 1 && !strcmp(m2.missing[0], "/Samples/gone.wav"));
  // the writer refuses what the reader would
  {
    Manifest b{};
    b.alloc(8, 8, 2);
    fill_manifest(&b);
    strcpy(b.box_uid, "3FA2C001");
    JOut ob;
    static char tb[1 << 16];
    ob.begin(tb, sizeof tb);
    CHECK(!write_manifest(b, ob));
    fill_manifest(&b);
    b.e[1].fmt_ver = 2;   // a STORE record newer than the registry's row
    ob.begin(tb, sizeof tb);
    CHECK(!write_manifest(b, ob));
    fill_manifest(&b);
    CHECK(b.add_entry("../x", kKindBox) < 0 && b.add_entry("box/project.bin", kKindBox) < 0);   // unsafe, twice
    CHECK(b.add_entry("manifest.json", kKindBox) < 0 && !b.add_path(0, "rel/path") && !b.add_need(kNeedCode, "a b", b.needs[0].sha));
    CHECK(!b.add_need(kNeedCode, "again", b.needs[0].sha));   // the same blob twice
    CHECK(b.add_path(0, "/Samples/y.wav") && !b.add_path(3, "/Samples/z.wav"));   // an entry's paths stay together
  }
  // Python's json reads it the same
  {
    std::string p3 = dir + "/manifest.json";
    FILE* g = fopen(p3.c_str(), "wb");
    fwrite(t1, 1, o.n, g);
    fclose(g);
    if (have_python)
      CHECK(system(("python3 -I -c \"import json,sys\nm=json.load(open(sys.argv[1]))\nassert m['format']=='sidecar-bundle' and m['version']==2 and "
                    "m['box']['uid']=='3fa2c001' and m['entries'][3]['paths'][1]=='/Samples/Old/kick copy.wav' and m['totals']['entries']==4 "
                    "and m['needs'][0]['kind']=='code'\" '" + p3 + "'").c_str()) == 0);
  }
  // every truncation refused (never a crash, never a read past the end)
  for (uint32_t n = 0; n < o.n - 2; ++n) CHECK(!read_manifest(t1, n, &m2, err, sizeof err));
  // the registry's verdicts: the old line's v1 bundle, a newer one this build may not read, a newer one it may
  {
    const std::string box = "\"box\":{\"type\":\"rytm\",\"uid\":\"3fa2c001\"},\"project\":{\"id\":\"0000beef\"}";
    const std::string v1 = "{\"format\":\"sidecar-bundle\",\"version\":1,\"min_reader\":1,\"project\":{\"rytm_name\":\"AR\",\"key\":\"K\"},"
                           "\"slots\":[],\"entries\":[]}";
    CHECK(!read_manifest(v1.data(), (uint32_t)v1.size(), &m2, err, sizeof err) && strstr(err, "not a bundle file this Sidecar can read"));
    const std::string v3 = "{\"format\":\"sidecar-bundle\",\"version\":3,\"min_reader\":3," + box + ",\"entries\":[]}";
    CHECK(!read_manifest(v3.data(), (uint32_t)v3.size(), &m2, err, sizeof err) && strstr(err, "newer Sidecar") && strstr(err, "bundle v3"));
    const std::string v3r = "{\"format\":\"sidecar-bundle\",\"version\":3,\"min_reader\":2,\"future\":{\"a\":[1,2,{\"b\":null}]}," + box +
                            ",\"entries\":[]}";
    CHECK(read_manifest(v3r.data(), (uint32_t)v3r.size(), &m2, err, sizeof err) && m2.read_only && m2.version == 3);
    const std::string v2 = "{\"format\":\"sidecar-bundle\",\"version\":2,\"min_reader\":2," + box + ",\"entries\":[]}";
    CHECK(read_manifest(v2.data(), (uint32_t)v2.size(), &m2, err, sizeof err) && !m2.read_only && m2.ne == 0);
  }
  // huge numbers, deep nesting, bad escapes, a zip slip in a name or a path, a newer STORE record, no box, no project
  {
    const std::string h = "{\"format\":\"sidecar-bundle\",\"version\":2,\"min_reader\":2,";
    const std::string box = "\"box\":{\"type\":\"rytm\",\"uid\":\"3fa2c001\"},\"project\":{\"id\":\"0000beef\"},";
    const std::string z64 = std::string(64, '0');
    const std::string bad[] = {
        "{\"format\":\"sidecar-bundle\",\"version\":2,\"min_reader\":99999999999999999999," + box + "\"entries\":[]}",
        h + box + "\"x\":[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]],\"entries\":[]}",
        h + box + "\"entries\":[{\"name\":\"../x\",\"kind\":\"box\",\"size\":1,\"sha256\":\"" + z64 + "\"}]}",
        h + box + "\"entries\":[{\"name\":\"samples/a\",\"kind\":\"sample\",\"size\":1,\"sha256\":\"" + z64 + "\",\"paths\":[\"/a/../../b\"]}]}",
        h + box + "\"entries\":[{\"name\":\"samples/a\",\"kind\":\"sample\",\"size\":-1,\"sha256\":\"" + z64 + "\"}]}",
        h + box + "\"entries\":[{\"name\":\"samples/a\",\"kind\":\"sample\",\"size\":1.5,\"sha256\":\"" + z64 + "\"}]}",
        h + box + "\"entries\":[{\"name\":\"a\\u0000b\",\"kind\":\"sample\",\"size\":1,\"sha256\":\"" + z64 + "\"}]}",
        h + box + "\"entries\":[{\"name\":\"store/1-0001.bin\",\"kind\":\"store\",\"size\":1,\"sha256\":\"" + z64 +
            "\",\"format\":{\"name\":\"store\",\"version\":7}}]}",
        h + box + "\"entries\":[{\"name\":\"box/a\",\"kind\":\"rytm\",\"size\":1,\"sha256\":\"" + z64 + "\"}]}",
        h + box + "\"entries\":[{\"name\":\"store/a\",\"kind\":\"box\",\"size\":1,\"sha256\":\"" + z64 + "\"}]}",
        h + "\"box\":{\"type\":\"dt\",\"uid\":\"3fa2c001\"},\"project\":{\"id\":\"0000beef\"},\"entries\":[]}",
        h + box + "\"needs\":[{\"kind\":\"code\",\"name\":\"t\",\"sha256\":\"" + z64 + "\"},{\"kind\":\"code\",\"name\":\"u\",\"sha256\":\"" + z64 + "\"}],\"entries\":[]}",
        h + box + "\"entries\":[]} x",
        h + "\"project\":{\"id\":\"0000beef\"},\"entries\":[]}",
        h + "\"box\":{\"type\":\"rytm\",\"uid\":\"3fa2c001\"},\"entries\":[]}",
        h + "\"box\":{\"type\":\"mono\",\"uid\":\"3fa2c001\"},\"project\":{\"id\":\"0000beef\"},\"entries\":[]}",
        h + "\"box\":{\"type\":\"rytm\",\"uid\":\"3fa2c0\"},\"project\":{\"id\":\"0000beef\"},\"entries\":[]}",
        h + box + "\"nothing\":1}",
        "{\"format\":\"zip\",\"version\":2,\"min_reader\":2," + box + "\"entries\":[]}",
    };
    for (const std::string& b : bad) {
      if (read_manifest(b.data(), (uint32_t)b.size(), &m2, err, sizeof err)) printf("read: %s\n", b.c_str());
      CHECK(!read_manifest(b.data(), (uint32_t)b.size(), &m2, err, sizeof err));
    }
    CHECK(!read_manifest(bad[7].data(), (uint32_t)bad[7].size(), &m2, err, sizeof err) && strstr(err, "store v7"));
  }
  // random bytes: refused, never a crash
  {
    uint32_t x = 1;
    std::string r(4096, ' ');
    for (int t = 0; t < 2000; ++t) {
      for (auto& c : r) x = x * 1103515245 + 12345, c = "{}[]\",:0123456789aeflnrstu\\ "[(x >> 16) % 29];
      read_manifest(r.data(), (uint32_t)r.size(), &m2, err, sizeof err);
    }
  }

  // ---- the round trip: a box project's entries -> manifest -> zip bytes -> eocd / central / local -> the manifest
  // read back from the first entry -> check() against the zip -> every entry's bytes hashed and matched
  {
    Manifest pm{};
    pm.alloc(64, 64, 8);
    fill_manifest(&pm);
    std::vector<std::string> body(pm.ne);
    body[0] = std::string(70000, '\0');                       // the box's stream (kits, patterns, sounds)
    for (size_t i = 0; i < body[0].size(); ++i) body[0][i] = (char)(i * 13 + (i >> 8));
    body[1] = std::string("SREC\x01\x00", 6) + "rec-one";    // two STORE records
    body[2] = "rec-two";
    body[3] = std::string(5000, 'k');                         // a pool sample
    for (int i = 0; i < pm.ne; ++i) {
      pm.e[i].size = (uint32_t)body[i].size();
      sha256(body[i].data(), body[i].size(), pm.e[i].sha);
      pm.e[i].crc = crc32(0, body[i].data(), body[i].size());
    }
    JOut mo;
    static char mt[1 << 16];
    mo.begin(mt, sizeof mt);
    CHECK(write_manifest(pm, mo));
    std::vector<std::string> all = {std::string(mt, mo.n)};
    all.insert(all.end(), body.begin(), body.end());
    std::vector<ZEntry> pz(pm.ne + 1);
    Layout PL;
    CHECK(plan(pm, mo.n, crc32(0, mt, mo.n), 1795000000u, pz.data(), (int)pz.size(), &PL));
    std::string bz(PL.total, '\0');
    Src s{&all};
    CHECK(render(pz.data(), PL, 0, (uint8_t*)&bz[0], (uint32_t)bz.size(), data_fn, &s));
    printf("test_bundle_zip: round trip, %d entries, %llu bytes, sha256 %s\n", pm.ne + 1, (unsigned long long)bz.size(), sha(bz).c_str());
    CHECK(sha(bz) == "5bd5399103fdabb23a09e61ad4a7978cfc17deb4ca244d8ad0f98478f3705ebe");   // the v2 bundle pinned: a change to the manifest's text or the layout shows here

    // read it back as the import does: the manifest first, from the first bytes alone
    auto import = [&](const std::string& zb, Manifest* rm, std::string* why) -> bool {
      uint64_t moff;
      uint32_t msz;
      if (!sniff_zip((const uint8_t*)zb.data(), (uint32_t)std::min<size_t>(zb.size(), 8192 + kManifestMax), &moff, &msz) ||
          moff + msz > zb.size())
        return *why = "sniff", false;
      if (!read_manifest(zb.data() + moff, msz, rm, err, sizeof err)) return *why = err, false;
      std::vector<ZEntry> rz;
      if (!read_all(zb, rz, why)) return false;
      if (rz[0].crc != crc32(0, zb.data() + moff, msz) || rz[0].data != moff) return *why = "manifest crc", false;
      if (!check(rm, rz.data(), (int)rz.size(), err, sizeof err)) return *why = err, false;
      for (int i = 0; i < rm->ne; ++i) {
        EntryHash eh;
        eh.begin();
        const ZEntry& x = rz[i + 1];
        for (uint32_t at = 0; at < x.size; at += 777) eh.update(zb.data() + x.data + at, std::min<uint32_t>(777, x.size - at));
        if (!eh.match(rm->e[i], err, sizeof err)) return *why = err, false;
      }
      return true;
    };
    Manifest rm{};
    rm.alloc(64, 64, 8);
    std::string why;
    CHECK(import(bz, &rm, &why));
    CHECK(rm.ne == pm.ne);
    for (int i = 0; i < pm.ne; ++i)
      CHECK(!strcmp(rm.e[i].name, pm.e[i].name) && rm.e[i].kind == pm.e[i].kind && rm.e[i].size == pm.e[i].size &&
            rm.e[i].crc == pm.e[i].crc && !memcmp(rm.e[i].sha, pm.e[i].sha, 32));
    {   // the registry sniffs the zip's manifest the same way
      uint64_t moff;
      uint32_t msz;
      CHECK(sniff_zip((const uint8_t*)bz.data(), 4096 + mo.n, &moff, &msz) && msz == mo.n);
      core::fmt::Info fi;
      CHECK(core::fmt::load(*core::fmt::find("bundle"), core::fmt::Bytes{(const uint8_t*)bz.data() + moff, msz}, nullptr, &fi) == core::fmt::kOk && fi.version == 2);
      CHECK(!sniff_zip((const uint8_t*)z.data(), (uint32_t)z.size(), &moff, &msz) || msz == d[0].size());   // the plain test zip
      CHECK(!sniff_zip((const uint8_t*)"PK", 2, &moff, &msz));
    }
    if (have_python) {   // any Mac or PC opens it
      std::string p4 = dir + "/box.sidecar.zip";
      FILE* g = fopen(p4.c_str(), "wb");
      fwrite(bz.data(), 1, bz.size(), g);
      fclose(g);
      CHECK(system(("python3 -I -c \"import zipfile,sys,json\nz=zipfile.ZipFile(sys.argv[1])\nassert z.testzip() is None\n"
                    "n=[i.filename for i in z.infolist()]\nassert n==['manifest.json','box/project.bin','store/1-0001.bin','store/0-0002.bin',"
                    "'samples/Drums/kick.wav'],n\nassert json.loads(z.read('manifest.json'))['project']['id']=='0000beef'\" '" + p4 + "'").c_str()) == 0);
    }
    // one wrong byte anywhere refuses it: in the data (sha / CRC), the manifest (CRC, or its reader), the headers
    for (uint64_t at : {pz[1].data + 5, pz[2].data, pz[4].data + 4999, pz[0].data + 3, pz[3].local + 31, PL.cd_off + 16}) {
      std::string b = bz;
      b[at] ^= 0x20;
      Manifest bm{};
      bm.alloc(64, 64, 8);
      CHECK(!import(b, &bm, &why));
    }
    // a zip whose entries are not the manifest's: one dropped, one added, two swapped, a size changed, a wrong folder
    auto rebuilt = [&](Manifest& mm, std::vector<std::string>& bodies, std::vector<std::string> names) {
      // the zip from these names and bodies (manifest.json first), the manifest text as given
      JOut x;
      static char xt[1 << 16];
      x.begin(xt, sizeof xt);
      CHECK(write_manifest(mm, x));
      std::vector<ZEntry> ez;
      std::vector<std::string> eb = {std::string(xt, x.n)};
      ez.push_back(ze("manifest.json"));
      for (size_t i = 0; i < names.size(); ++i) ez.push_back(ze(names[i].c_str())), eb.push_back(bodies[i]);
      Layout XL;
      return whole(ez, eb, &XL, 1795000000u);
    };
    std::vector<std::string> names;
    for (int i = 0; i < pm.ne; ++i) names.push_back(pm.e[i].name);
    {
      Manifest bm{};
      bm.alloc(64, 64, 8);
      std::vector<std::string> b2(body.begin(), body.end() - 1), n2(names.begin(), names.end() - 1);
      CHECK(!import(rebuilt(pm, b2, n2), &bm, &why) && why.find("missing") != std::string::npos);
      b2 = body, n2 = names;
      b2.push_back("x"), n2.push_back("box/extra.bin");
      CHECK(!import(rebuilt(pm, b2, n2), &bm, &why) && why.find("does not list") != std::string::npos);
      b2 = body, n2 = names;
      std::swap(b2[1], b2[2]), std::swap(n2[1], n2[2]);
      CHECK(!import(rebuilt(pm, b2, n2), &bm, &why) && why.find("name or order") != std::string::npos);
      b2 = body;
      b2[3] += "!";
      CHECK(!import(rebuilt(pm, b2, names), &bm, &why) && why.find("size differs") != std::string::npos);
      Manifest wm{};   // a box entry under store/: the writer refuses it (the reader too: bad[] above; check() again)
      wm.alloc(64, 64, 8);
      fill_manifest(&wm);
      strcpy(wm.e[0].name, "store/project.bin");
      JOut x;
      static char xt[1 << 16];
      x.begin(xt, sizeof xt);
      CHECK(!write_manifest(wm, x));
      CHECK(wm.add_entry("samples/x.wav", kKindStore) < 0 && wm.add_entry("box/", kKindBox) < 0);
      std::vector<ZEntry> rz;
      CHECK(read_all(bz, rz, &why));
      Manifest cm{};
      cm.alloc(64, 64, 8);
      uint64_t moff;
      uint32_t msz;
      CHECK(sniff_zip((const uint8_t*)bz.data(), 4096 + mo.n, &moff, &msz) && read_manifest(bz.data() + moff, msz, &cm, err, sizeof err));
      cm.e[0].kind = kKindStore;
      CHECK(!check(&cm, rz.data(), (int)rz.size(), err, sizeof err) && strstr(err, "kind's folder"));
    }
    // the names a packer makes
    char nb[kNameMax + 1];
    CHECK(store_entry_name("1-0001.bin", nb, sizeof nb) && !strcmp(nb, "store/1-0001.bin"));
    CHECK(!store_entry_name("a/b.bin", nb, sizeof nb) && !store_entry_name("..", nb, sizeof nb));
    CHECK(sample_entry_name("/Samples/Drums/kick.wav", nb, sizeof nb) && !strcmp(nb, "samples/Drums/kick.wav"));
    CHECK(!sample_entry_name("/CHOP/a.wav", nb, sizeof nb) && !sample_entry_name("/Samples/../x", nb, sizeof nb) &&
          !sample_entry_name("/Samples/", nb, sizeof nb));
  }

  if (system(("rm -rf '" + dir + "'").c_str())) {}
  printf("test_bundle_zip: PASS\n");
  return 0;
}

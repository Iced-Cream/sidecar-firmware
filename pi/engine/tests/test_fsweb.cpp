// pi/engine/tests/test_fsweb.cpp - the web file browser's logic (fsweb.cpp) on a folder standing in for the card:
// paths and the read-only boot files, a 20 MB upload in the page's 48 KB chunks (each chunk's time measured: the
// longest is what one request holds core 0's web task on the Pi, before its card's own speed), resume, a wrong
// CRC, a full card, packages checked before they go in place (good RMPA, a Teensy RMP1, a damaged one, junk),
// delete, mkdir, rename; the kept folders (/.rytm, /Samples, /Plugins) and config.txt.bak; a dropped folder
// (mkdirs, then each file). FSWEB_PKG: a good RMPA package (default plugins/sine_test/sine_test.a64.rmp; its test
// skips without it).
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <vector>

#include "check.h"
#include "fsweb.h"

static std::string g_root;
static std::string mac(const char* p) { return g_root + p; }
static uint64_t g_free = 1ull << 40;
static int g_yields = 0;

static bool b_stat(const char* p, uint64_t* size, bool* dir) {
  struct stat st;
  if (::stat(mac(p).c_str(), &st)) return false;
  *size = (uint64_t)st.st_size;
  *dir = S_ISDIR(st.st_mode);
  return true;
}
static int b_list(const char* p, fsweb::Entry* out, int max, bool* more) {
  DIR* d = opendir(mac(p).c_str());
  if (!d) return -1;
  int n = 0;
  *more = false;
  while (dirent* e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if (n == max) { *more = true; break; }
    snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
    std::string full = std::string(p) + (strcmp(p, "/") ? "/" : "") + e->d_name;
    b_stat(full.c_str(), &out[n].size, &out[n].dir);
    ++n;
  }
  closedir(d);
  return n;
}
static bool b_read(const char* p, uint64_t off, void* buf, uint32_t n, uint32_t* got) {
  FILE* f = fopen(mac(p).c_str(), "rb");
  if (!f) return false;
  fseeko(f, (off_t)off, SEEK_SET);
  *got = (uint32_t)fread(buf, 1, n, f);
  fclose(f);
  return true;
}
static bool b_write(const char* p, uint64_t off, const void* buf, uint32_t n, bool create) {
  FILE* f = fopen(mac(p).c_str(), create ? "wb" : "r+b");
  if (!f) return false;
  fseeko(f, (off_t)off, SEEK_SET);
  bool ok = fwrite(buf, 1, n, f) == n;
  fclose(f);
  return ok;
}
static bool b_remove(const char* p) { return ::remove(mac(p).c_str()) == 0; }
static bool b_rename(const char* a, const char* b) { return ::rename(mac(a).c_str(), mac(b).c_str()) == 0; }
static bool b_mkdir(const char* p) { return ::mkdir(mac(p).c_str(), 0755) == 0; }
static bool b_space(uint64_t* f, uint64_t* t) { *f = g_free; *t = 1ull << 41; return true; }
static void b_yield() { ++g_yields; }
static std::vector<std::string> g_changed;
static void b_changed(const char* p) { g_changed.push_back(p); }
static const fsweb::Backend B = {b_stat, b_list, b_read, b_write, b_remove, b_rename, b_mkdir, b_space, b_yield, b_changed};

static std::vector<uint8_t> slurp(const std::string& p) {
  std::vector<uint8_t> v;
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return v;
  uint8_t b[65536];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
  fclose(f);
  return v;
}

// The whole file through put() in the page's chunks; returns the last result (and the longest chunk in us).
static fsweb::Result upload(const char* path, const std::vector<uint8_t>& data, uint32_t chunk = 48 * 1024,
                            double* max_us = nullptr, uint32_t crc_fix = 0) {
  uint32_t crc = fsweb::crc32(0, data.data(), (uint32_t)data.size()) ^ crc_fix;
  fsweb::Result r{0, ""};
  double worst = 0;
  for (uint64_t off = 0; off < data.size() || (data.empty() && off == 0);) {
    uint32_t n = (uint32_t)std::min<uint64_t>(chunk, data.size() - off);
    auto t0 = std::chrono::steady_clock::now();
    fsweb::put(B, path, off, data.size(), crc, data.data() + off, n, &r);
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    if (us > worst) worst = us;
    if (r.status != 200) break;
    off += n;
    if (data.empty()) break;
  }
  if (max_us) *max_us = worst;
  return r;
}

int main() {
  char tmpl[] = "/tmp/fsweb-XXXXXX";
  g_root = mkdtemp(tmpl);
  mkdir(mac("/Plugins").c_str(), 0755);
  mkdir(mac("/Samples").c_str(), 0755);
  mkdir(mac("/overlays").c_str(), 0755);
  b_write("/config.txt", 0, "arm_64bit=1\n", 12, true);

  // paths and the boot files
  CHECK(fsweb::path_ok("/Samples/a b.wav") && !fsweb::path_ok("Samples") && !fsweb::path_ok("/a/../b") &&
        !fsweb::path_ok("/a//b") && !fsweb::path_ok("/a\\b") && !fsweb::path_ok("/x:y"));
  for (const char* p : {"/config.txt", "/CMDLINE.TXT", "/sdboot.img", "/engine.img", "/engine.img.prev", "/kernel8-rpi4.img",
                        "/start4.elf", "/fixup4.dat", "/bcm2711-rpi-4-b.dtb", "/overlays", "/overlays/x.dtbo", "/",
                        "/tryboot.txt", "/armstub8.bin"})
    CHECK(fsweb::read_only(p));
  for (const char* p : {"/Samples/config.txt", "/Plugins/a.rmp", "/notes.txt", "/.rytm/slots/X.txt"}) CHECK(!fsweb::read_only(p));
  fsweb::Result r;
  fsweb::put(B, "/config.txt", 0, 3, fsweb::crc32(0, (const uint8_t*)"abc", 3), (const uint8_t*)"abc", 3, &r);
  CHECK(r.status == 403);
  fsweb::remove(B, "/overlays", &r);
  CHECK(r.status == 403);
  fsweb::rename(B, "/config.txt", "/c.txt", &r);
  CHECK(r.status == 403);
  CHECK(slurp(mac("/config.txt")).size() == 12);

  // 20 MB in 48 KB chunks
  std::vector<uint8_t> big(20u << 20);
  uint32_t x = 1;
  for (auto& c : big) { x = x * 1103515245u + 12345u; c = (uint8_t)(x >> 16); }
  double worst = 0;
  auto t0 = std::chrono::steady_clock::now();
  r = upload("/Samples/big.bin", big, 48 * 1024, &worst);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(r.status == 200 && !strncmp(r.msg, "done", 4));
  CHECK(slurp(mac("/Samples/big.bin")) == big);
  CHECK(access(mac("/Samples/big.bin.part").c_str(), 0) != 0);
  printf("20 MB upload: %u chunks of 48 KB, %.2f s on the Mac, longest chunk %.0f us, %d yields (4 KB pieces)\n",
         (unsigned)((big.size() + 49151) / 49152), secs, worst, g_yields);
  CHECK(g_yields >= (int)(big.size() / 4096));

  // resume: a chunk out of place gets "expected <offset>"
  std::vector<uint8_t> mid(200000, 7);
  uint32_t crc = fsweb::crc32(0, mid.data(), (uint32_t)mid.size());
  fsweb::put(B, "/Samples/mid.bin", 0, mid.size(), crc, mid.data(), 49152, &r);
  CHECK(r.status == 200 && !strcmp(r.msg, "ok 49152"));
  fsweb::put(B, "/Samples/mid.bin", 98304, mid.size(), crc, mid.data() + 98304, 49152, &r);
  CHECK(r.status == 409 && !strcmp(r.msg, "expected 49152"));
  for (uint64_t off = 49152; off < mid.size(); off += 49152) {
    uint32_t n = (uint32_t)std::min<uint64_t>(49152, mid.size() - off);
    fsweb::put(B, "/Samples/mid.bin", off, mid.size(), crc, mid.data() + off, n, &r);
    CHECK(r.status == 200);
  }
  CHECK(slurp(mac("/Samples/mid.bin")) == mid);

  // abandoned uploads (a page that stopped mid-file): when the slots run out, the reused slot's .part is removed
  for (int i = 0; i < 5; i++) {
    char p[32];
    snprintf(p, sizeof p, "/Samples/ab%d.bin", i);
    fsweb::put(B, p, 0, mid.size(), crc, mid.data(), 49152, &r);
    CHECK(r.status == 200);
  }
  CHECK(access(mac("/Samples/ab0.bin.part").c_str(), 0) != 0);
  CHECK(access(mac("/Samples/ab4.bin.part").c_str(), 0) == 0);
  for (int i = 1; i < 5; i++) {
    char p[40];
    snprintf(p, sizeof p, "/Samples/ab%d.bin.part", i);
    fsweb::remove(B, p, &r);   // the page deletes a failed file's .part
    CHECK(r.status == 200);
  }

  // a wrong CRC: nothing replaced
  std::vector<uint8_t> v2(5000, 9);
  r = upload("/Samples/mid.bin", v2, 48 * 1024, nullptr, 1);
  CHECK(r.status == 400 && strstr(r.msg, "CRC"));
  CHECK(slurp(mac("/Samples/mid.bin")) == mid);
  // a full card
  g_free = 1000;
  r = upload("/Samples/nope.bin", v2);
  CHECK(r.status == 507);
  g_free = 1ull << 40;
  // no such folder
  r = upload("/NoFolder/x.bin", v2);
  CHECK(r.status == 404);

  // packages: junk, a damaged one, a Teensy one, a good one
  r = upload("/Plugins/junk.rmp", v2);
  CHECK(r.status == 422 && strstr(r.msg, "not a machine package"));
  CHECK(access(mac("/Plugins/junk.rmp").c_str(), 0) != 0);
  const char* pkg = getenv("FSWEB_PKG") ? getenv("FSWEB_PKG") : "../../../plugins/sine_test/sine_test.a64.rmp";
  std::vector<uint8_t> good = slurp(pkg);
  if (good.empty()) {
    printf("SKIP the package checks: no %s (make -C plugins/sine_test a64)\n", pkg);
  } else {
    std::vector<uint8_t> bad = good;
    bad[bad.size() / 2] ^= 0x40;
    r = upload("/Plugins/bad.rmp", bad);
    CHECK(r.status == 422 && strstr(r.msg, "CRC wrong"));
    std::vector<uint8_t> arm = good;
    memcpy(arm.data(), "RMP1", 4);
    r = upload("/Plugins/arm.rmp", arm);
    CHECK(r.status == 422 && strstr(r.msg, "Teensy"));
    g_changed.clear();
    r = upload("/Plugins/sine.rmp", good);
    CHECK(r.status == 200);
    CHECK(g_changed.size() == 1 && g_changed[0] == "/Plugins/sine.rmp");
    CHECK(fsweb::check_package(B, "/Plugins/sine.rmp", good.size()) == nullptr);
    fsweb::rename(B, "/Samples/mid.bin", "/Plugins/mid.rmp", &r);
    CHECK(r.status == 422);
  }

  // list, mkdir, rename, delete
  static char js[65536];
  int n = fsweb::list_json(B, "/", js, sizeof js, &r);
  CHECK(n > 0 && strstr(js, "{\"n\":\"config.txt\",\"d\":0,\"s\":12,\"ro\":1}") && strstr(js, "\"n\":\"Samples\",\"d\":1"));
  fsweb::mkdir(B, "/Samples/kit", &r);
  CHECK(r.status == 200);
  fsweb::mkdir(B, "/Samples/kit", &r);
  CHECK(r.status == 409);
  fsweb::rename(B, "/Samples/mid.bin", "/Samples/kit/mid.bin", &r);
  CHECK(r.status == 200 && slurp(mac("/Samples/kit/mid.bin")) == mid);
  fsweb::remove(B, "/Samples/kit", &r);
  CHECK(r.status == 409);   // not empty
  fsweb::remove(B, "/Samples/kit/mid.bin", &r);
  CHECK(r.status == 200);
  fsweb::remove(B, "/Samples/kit", &r);
  CHECK(r.status == 200);
  // the Sidecar's own folders: kept (no delete, no rename), their contents free; config.txt.bak locked, .prev not
  mkdir(mac("/.rytm").c_str(), 0755);
  b_write("/config.txt.bak", 0, "x", 1, true);
  b_write("/config.txt.prev", 0, "y", 1, true);
  CHECK(fsweb::read_only("/config.txt.bak") && fsweb::read_only("/CONFIG.TXT.BAK") && !fsweb::read_only("/config.txt.prev"));
  for (const char* k : {"/.rytm", "/Samples", "/Plugins", "/plugins"}) {
    CHECK(fsweb::kept_dir(k));
    fsweb::remove(B, k, &r);
    CHECK(r.status == 403);
    fsweb::rename(B, k, "/Moved", &r);
    CHECK(r.status == 403);
  }
  CHECK(!fsweb::kept_dir("/Samples/kit") && !fsweb::kept_dir("/Samples2") && !fsweb::kept_dir("/x/Samples"));
  fsweb::remove(B, "/config.txt.bak", &r);
  CHECK(r.status == 403);
  fsweb::rename(B, "/config.txt.bak", "/c.bak", &r);
  CHECK(r.status == 403);
  fsweb::rename(B, "/config.txt.prev", "/config.txt.bak", &r);
  CHECK(r.status == 403);
  fsweb::put(B, "/config.txt.bak", 0, 1, fsweb::crc32(0, (const uint8_t*)"z", 1), (const uint8_t*)"z", 1, &r);
  CHECK(r.status == 403 && slurp(mac("/config.txt.bak"))[0] == 'x');
  fsweb::rename(B, "/config.txt.prev", "/config.txt.old", &r);
  CHECK(r.status == 200);
  fsweb::remove(B, "/config.txt.old", &r);
  CHECK(r.status == 200);
  n = fsweb::list_json(B, "/", js, sizeof js, &r);
  CHECK(strstr(js, "\"n\":\"Samples\",\"d\":1,\"s\":") && strstr(js, "\"n\":\".rytm\",\"d\":1,\"s\":"));
  CHECK(strstr(js, "\"n\":\"config.txt.bak\",\"d\":0,\"s\":1,\"ro\":1}"));
  {
    const char* q = strstr(js, "\"n\":\"Samples\"");
    CHECK(q && std::string(q, strchr(q, '}') - q).find("\"ro\":2") != std::string::npos);
  }
  b_write("/Samples/s.wav", 0, "w", 1, true);
  fsweb::rename(B, "/Samples/s.wav", "/Samples/t.wav", &r);
  CHECK(r.status == 200);
  fsweb::remove(B, "/Samples/t.wav", &r);
  CHECK(r.status == 200);

  // a dropped folder: mkdir -p of every folder, then each file put into it
  fsweb::mkdirs(B, "/Samples/Kit A/sub/deep", &r);
  CHECK(r.status == 200 && !strcmp(r.msg, "made"));
  fsweb::mkdirs(B, "/Samples/Kit A/sub", &r);
  CHECK(r.status == 200 && !strcmp(r.msg, "exists"));
  fsweb::mkdirs(B, "/Samples/Kit A/sub2", &r);
  CHECK(r.status == 200 && !strcmp(r.msg, "made"));
  std::vector<uint8_t> f1(70000, 3), f2(10, 4);
  r = upload("/Samples/Kit A/sub/deep/one.wav", f1);
  CHECK(r.status == 200 && slurp(mac("/Samples/Kit A/sub/deep/one.wav")) == f1);
  r = upload("/Samples/Kit A/two.wav", f2);
  CHECK(r.status == 200 && slurp(mac("/Samples/Kit A/two.wav")) == f2);
  fsweb::mkdirs(B, "/Samples/Kit A/two.wav/x", &r);
  CHECK(r.status == 409);   // a file in the way
  fsweb::mkdirs(B, "/overlays/x", &r);
  CHECK(r.status == 400);   // a boot folder
  fsweb::mkdirs(B, "/a/../b", &r);
  CHECK(r.status == 400);
  fsweb::mkdirs(B, "/NewTop", &r);
  {
    uint64_t sz;
    bool dir = false;
    CHECK(r.status == 200 && b_stat("/NewTop", &sz, &dir) && dir);
  }
  n = fsweb::list_json(B, "/nope", js, sizeof js, &r);
  CHECK(n == 0 && r.status == 404);
  system(("rm -rf " + g_root).c_str());
  printf("test_fsweb: OK\n");
  return 0;
}

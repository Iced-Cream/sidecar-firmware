// core/tests/test_lists_cardq.cpp - the project's JUNO and FM lists with a card core (core/pool/pool.cpp project_lists_step, 7z
// 2026-10-31): read on the card core in bounded requests, into a copy; the old project's lists until the copy is whole, then
// the same lists as load_list gives, the names to the Rytm; no card time on the control core; a list's save posted; a save
// still on its way landing before the read; a switch during the read; an edit refused, LOAD TO PROJ held.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "fmt/fmt.h"
#include "msamp/cardq.h"
#include "platform.h"
#include "pool/pool.h"
#include "user/lists.h"
#include "voices.h"
#include "plaits/resources.h"

using namespace core;
static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void put(const std::string& p, const void* d, size_t n) {
  FILE* f = fopen(host_path(p.c_str()).c_str(), "wb");
  CHECK(f);
  fwrite(d, 1, n, f);
  fclose(f);
}
static std::string slurp(const std::string& p) {
  std::string r;
  FILE* f = fopen(host_path(p.c_str()).c_str(), "rb");
  if (!f) return r;
  char b[512];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) r.append(b, n);
  fclose(f);
  return r;
}
static void to_project(const char* n) { project_switch_named(n, n, false); }
static uint64_t g_card3 = 0;
static bool serve() {
  host_card_core(0);
  bool r = cardq::service() != nullptr;
  host_card_core(3);
  return r;
}
static int run() {   // the passes until the lists are in: the most requests waiting checked
  int n = 0;
  for (; n < 5000 && lists_busy(); ++n) {
    uint64_t c = host_card_us(3);
    project_lists_step();
    g_card3 += host_card_us(3) - c;
    CHECK(cardq::pending() <= 1);
    serve();
    fmt::post_step();
  while (fmt::post_busy()) serve(), fmt::post_step();
  }
  CHECK(!lists_busy());
  return n;
}
struct Snap { std::vector<jlib::Patch> j; std::vector<dx7::Slot> f; std::vector<std::vector<uint8_t>> v; };
static Snap snap() {
  Snap s;
  for (int i = 0; i < junolib.listed(); ++i) s.j.push_back(junolib.slot(i));
  for (int i = 0; i < dxlib.listed(); ++i) {
    s.f.push_back(dxlib.slot(i));
    uint8_t b[128];
    std::vector<uint8_t> v;
    if (dxlib.voice(i, b)) v.assign(b, b + 128);
    s.v.push_back(v);
  }
  return s;
}
static bool same(const Snap& a, const Snap& b) {
  if (a.j.size() != b.j.size() || a.f.size() != b.f.size() || a.v != b.v) return false;
  for (size_t i = 0; i < a.j.size(); ++i)
    if (a.j[i].used != b.j[i].used || memcmp(a.j[i].b, b.j[i].b, 18) || strcmp(a.j[i].name, b.j[i].name)) return false;
  for (size_t i = 0; i < a.f.size(); ++i)
    if (a.f[i].used != b.f[i].used || a.f[i].bank != b.f[i].bank || a.f[i].voice != b.f[i].voice || memcmp(a.f[i].md5, b.f[i].md5, 16) ||
        strcmp(a.f[i].name, b.f[i].name))
      return false;
  return true;
}

int main() {
  char tmpl[] = "/tmp/core-lists-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/DX7") && fs.mkdir("/JUNO"));
  // a bank that is not one of Plaits': its voices come from the card
  std::vector<uint8_t> mine(plaits::syx_bank_1, plaits::syx_bank_1 + 4096);
  mine[0] ^= 1;
  put("/DX7/mine.syx", mine.data(), mine.size());
  hooks.to_rytm = out;
  CHECK(engine_begin());
  to_project("AAA");
  while (fmt::post_step()) {}
  int bank = -1;
  for (int k = 0; k < dxlib.nodes(); ++k)
    if (dxlib.node(k).bank) bank = k;
  CHECK(bank >= 0);
  // the lists, by load_list's path (no card core)
  uint8_t pb[18];
  for (int i = 0; i < 18; ++i) pb[i] = (uint8_t)(i * 3);
  CHECK(junolib.put(pb, "Pad") == 0);
  pb[0] = 99;
  CHECK(junolib.put(pb, "Lead") == 1);
  for (int v = 0; v < 12; ++v) CHECK(dxlib.add(bank, v) == v);
  CHECK(dxlib.add_rom(3) == 12);
  fmt::post_step();
  const Snap want = snap();
  CHECK(want.j.size() == 2 && want.f.size() == 13 && !want.v[11].empty());
  to_project("BBB");
  while (fmt::post_step()) {}
  CHECK(junolib.listed() == 0 && dxlib.listed() == 0);
  to_project("AAA");
  while (fmt::post_step()) {}
  CHECK(same(snap(), want));
  to_project("BBB");
  while (fmt::post_step()) {}

  // ---- a card core
  host_card_model(true);
  cardq::set_worker(true);
  g_out.clear();
  to_project("AAA");
  CHECK(lists_busy() && junolib.listed() == 0);   // the old project's until the copy is whole
  for (int i = 0; i < 2; ++i) {   // an edit refused, LOAD TO PROJ held
    char o[120];
    CHECK(lists::command("juno=del:0", o, sizeof o) && strstr(o, "loading"));
  }
  g_out.clear();
  int passes = run();
  CHECK(g_card3 == 0);
  CHECK(same(snap(), want));
  bool j = false, f = false;
  for (auto& m : g_out) j = j || m[4] == 0x16, f = f || m[4] == 0x17;
  CHECK(j && f);
  printf("the lists by requests: %d passes, %.1f ms of card time on the card core\n", passes, (double)host_card_us(0) / 1000.0);
  // LOAD TO PROJ while they load: held, then into the new project's list
  to_project("BBB");
  run();
  to_project("AAA");
  pool_add(cards.folders() + junolib.folders() + bank, 20);
  CHECK(dxlib.listed() == 0 && lists_busy());
  run();
  CHECK(dxlib.listed() == 14);
  fmt::post_step();
  while (fmt::post_busy()) serve(), fmt::post_step();
  CHECK(slurp("/.rytm/fm/AAA.txt").find("\n") != std::string::npos);
  // a list's save posted: no card time here; left and come back to: the save lands first
  uint64_t c3 = host_card_us(3);
  CHECK(junolib.clear(1));
  CHECK(host_card_us(3) == c3);
  to_project("BBB");
  to_project("AAA");
  run();
  CHECK(junolib.listed() == 1 && !strcmp(junolib.slot(0).name, "Pad"));
  // a switch during the read: the newest project's lists
  to_project("BBB");
  project_lists_step();
  CHECK(cardq::pending() == 1);
  to_project("AAA");
  run();
  CHECK(dxlib.listed() == 14 && junolib.listed() == 1);
  // a project renamed during the read: the lists' next writes go to its new key
  to_project("BBB");
  run();
  to_project("AAA");
  project_lists_step();
  junolib.rekey("CCC"), dxlib.rekey("CCC");
  project_lists_rekey("CCC");
  CHECK(fs.rename("/.rytm/juno/AAA.txt", "/.rytm/juno/CCC.txt") && fs.rename("/.rytm/fm/AAA.txt", "/.rytm/fm/CCC.txt"));
  run();
  CHECK(junolib.listed() == 1);
  CHECK(junolib.put(pb, "Moved") >= 0);
  fmt::post_step();
  while (fmt::post_busy()) serve(), fmt::post_step();
  CHECK(slurp("/.rytm/juno/CCC.txt").find("Moved") != std::string::npos && slurp("/.rytm/juno/AAA.txt").empty());
  cardq::set_worker(false);
  printf("test_lists_cardq: PASS\n");
  return 0;
}

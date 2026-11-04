// core/tests/test_presets.cpp - the JUNO / 6-OP FM preset browser (0.4.0, core/presets/preset_index.h): /JUNO and /DX7 in
// RAM, walked on the card core in pieces; the bank list on the project's list, the built-ins when it is empty; adding from
// the index (no card read while scrolling, a voice read on the card core only); walked again after a change; a PAT plays
// what it played; an FM list from before 0.4.0 keeps its PATs and takes its voices into its file; a project's zip plays the
// same on a card without the folders.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "bundle/bundle.h"
#include "check.h"
#include "engine.h"
#include "fmt/fmt.h"
#include "lib/srclib.h"
#include "machines.h"
#include "msamp/cardq.h"
#include "platform.h"
#include "plaits/resources.h"
#include "pool/pool.h"
#include "presets/preset_index.h"
#include "presets/md5.h"
#include "ui/extra.h"
#include "user/lists.h"
#include "voices.h"

namespace core { bool fm_bytes(int pat, uint8_t* b); }
using namespace core;
static uint64_t g_us = 1000000;
static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static std::vector<uint8_t> listing(int f) {   // 0D <folder>: the first 0E back
  g_out.clear();
  const uint8_t m[8] = {0xF0, 0x7D, 0x52, 0x54, 0x0D, (uint8_t)(f >> 7), (uint8_t)(f & 127), 0xF7};
  engine_sysex(m, 8);
  for (auto& r : g_out)
    if (r.size() > 13 && r[4] == 0x0E) return r;
  CHECK(false);
  return {};
}
static int total_of(const std::vector<uint8_t>& r) { return r[11] << 7 | r[12]; }
static bool has_name(const std::vector<uint8_t>& r, const char* n) {
  return std::string(r.begin(), r.end()).find(n) != std::string::npos;
}
static void nolog(const char*, ...) {}
static void current(char* o, int cap) { snprintf(o, cap, "BBB"); }
static void put(const std::string& p, const std::vector<uint8_t>& d) {
  std::string h = host_path(p.c_str());
  CHECK(!system(("mkdir -p \"$(dirname '" + h + "')\"").c_str()));
  FILE* f = fopen(h.c_str(), "wb");
  CHECK(f);
  fwrite(d.data(), 1, d.size(), f);
  fclose(f);
}
static std::string slurp(const std::string& p) {
  std::string r;
  FILE* f = fopen(host_path(p.c_str()).c_str(), "rb");
  if (!f) return r;
  char b[4096];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) r.append(b, n);
  fclose(f);
  return r;
}
static std::vector<uint8_t> dump(int code, uint8_t seed) {   // a Juno-106 patch dump, 24 bytes
  std::vector<uint8_t> m = {0xf0, 0x41, 0x30, 0, (uint8_t)code};
  for (int i = 0; i < 18; ++i) m.push_back((uint8_t)((seed + i) & 0x7f));
  m.push_back(0xf7);
  return m;
}
static std::vector<uint8_t> dx_bank(uint8_t seed) {
  std::vector<uint8_t> b(plaits::syx_bank_1, plaits::syx_bank_1 + 4096);
  b[0] ^= seed;
  return b;
}
static bool serve() {
  host_card_core(0);
  bool r = cardq::service() != nullptr;
  host_card_core(3);
  return r;
}
static void pump() {
  presets::step();
  project_lists_step();
  serve();
  fmt::post_step();
  while (fmt::post_busy()) serve(), fmt::post_step();
}
static int walk() {   // the passes the walk took; never more than one request waiting
  int n = 0;
  for (; n < 20000 && !presets::ready(); ++n) {
    presets::step();
    CHECK(cardq::pending() <= 1);
    serve();
  }
  CHECK(presets::ready());
  return n;
}
static int row_of(int f, const char* name) {
  for (int r = 0; r < presets::count(f); ++r)
    if (!strcmp(presets::name(f, r), name)) return r;
  return -1;
}
// the Rytm's side of the list, on track 0
static void open(int machine, int pat) { ui::src_bank_list(0, 0, machine, pat, 1, 0); }
static void key(int code) { CHECK(ui::src_bank_key(code, true)); }
static void files(int machine) {
  ui::src_bank_list(0, 0, machine, 0, 3, 0);
  CHECK(ui::src_bank_state().browse);
}
enum { kLeft = 0x40, kRight, kUp, kDown, kYes, kNo };
static void go_to(int folder, int row) {   // the cursor by knob A
  ui::src_bank_enc(0, -1000, false);
  ui::src_bank_enc(0, row, false);
  (void)folder;
}
static void add_at(int machine, int f, const char* name) {   // FUNC + YES already: YES on the row
  int r = row_of(f, name);
  CHECK(r >= 0);
  go_to(f, r);
  key(kYes);
  for (int i = 0; i < 20 && ui::src_bank_state().browse; ++i) {
    pump();
    ui::src_bank_page(0, machine);
  }
  CHECK(!ui::src_bank_state().browse);
}
static std::string list_text(const std::vector<uint8_t>& v, int voice) {
  std::string s(v.begin() + 128 * voice, v.begin() + 128 * voice + 128);
  return s;
}
static std::string fm_at(int pat) {
  uint8_t b[128];
  return fm_bytes(pat, b) ? std::string((char*)b, 128) : std::string();
}
static std::string juno_at(int pat) {
  const uint8_t* b;
  const char* n;
  juno_pick(pat, &b, &n);
  return std::string((const char*)b, 18);
}
static std::vector<uint8_t> bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }
static std::string md5_hex(const std::vector<uint8_t>& bank) {
  md5::Md5 m;
  m.update(bank.data(), 4096);
  uint8_t d[16];
  m.final(d);
  char h[33];
  for (int i = 0; i < 16; ++i) snprintf(h + 2 * i, 3, "%02x", d[i]);
  return h;
}
static int dxlib_bank(const char* name) {
  for (int k = 0; k < dxlib.nodes(); ++k)
    if (dxlib.node(k).bank && !strcmp(dxlib.node(k).name, name)) return k;
  CHECK(false);
  return -1;
}

int main() {
  char tmpl[] = "/tmp/core-presets-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples"));
  put("/JUNO/one.syx", dump(5, 11));
  put("/JUNO/not.txt", {1, 2, 3});
  put("/JUNO/sub/two.syx", dump(6, 21));
  std::vector<uint8_t> jb;
  for (int i = 0; i < 3; ++i) {
    auto d = dump(i, (uint8_t)(31 + i));
    jb.insert(jb.end(), d.begin(), d.end());
  }
  put("/JUNO/bank.syx", jb);
  std::vector<uint8_t> dm = dx_bank(1), ds = dx_bank(2);
  put("/DX7/mine.syx", dm);
  std::vector<uint8_t> sx = {0xf0, 0x43, 0, 9, 0x20, 0};
  sx.insert(sx.end(), ds.begin(), ds.end());
  sx.push_back(0), sx.push_back(0xf7);
  put("/DX7/deep/down/sys.syx", sx);
  put("/DX7/bad.syx", {1, 2, 3, 4});
  hooks.to_rytm = out;
  CHECK(engine_begin());
  project_switch_named("AAA", "AAA", false);
  host_card_model(true);
  cardq::set_worker(true);
  while (lists_busy()) pump();

  // ---- the walk, on the card core, in pieces
  uint64_t c3 = host_card_us(3);
  int passes = walk();
  CHECK(host_card_us(3) == c3);
  printf("index: %d passes, %d + %d items, %lu + %lu bytes of text; %lu bytes in all\n", passes, presets::stats(0).items, presets::stats(1).items,
         (unsigned long)presets::stats(0).text, (unsigned long)presets::stats(1).text, (unsigned long)presets::ram_bytes());
  CHECK(!presets::full(0) && !presets::full(1));
  const int J = presets::kIxJuno, F = presets::kIxFm;
  CHECK(!strcmp(presets::name(J, 0), "FACTORY") && !strcmp(presets::name(F, 0), "ROM"));   // the built-ins on top
  int fac = presets::child(J, 0), rom = presets::child(F, 0);
  CHECK(presets::count(fac) == 128 && presets::count(rom) == 96);
  CHECK(presets::is_dir(J, row_of(J, "bank")) && presets::is_dir(J, row_of(J, "sub")) && !presets::is_dir(J, row_of(J, "one")));
  CHECK(row_of(J, "not") < 0 && row_of(J, "not.txt") < 0);
  CHECK(row_of(J, "bank") < row_of(J, "sub") && row_of(J, "sub") < row_of(J, "one"));   // folders (banks too) first, by name
  int jbank = presets::child(J, row_of(J, "bank"));
  CHECK(presets::count(jbank) == 3 && !strcmp(presets::name(jbank, 1), "A12 bank"));
  CHECK(presets::parent(jbank) == J && presets::row_in_parent(jbank) == row_of(J, "bank"));
  CHECK(presets::is_dir(F, row_of(F, "deep")) && row_of(F, "bad") < 0);
  int mine = presets::child(F, row_of(F, "mine"));
  CHECK(presets::count(mine) == 32);
  char nm[11];
  dx7::voice_name(dm.data() + 128 * 7 + 118, nm);
  CHECK(!strcmp(presets::name(mine, 7), nm));
  int deep = presets::child(presets::child(F, row_of(F, "deep")), 0);
  int sys = presets::child(deep, 0);
  CHECK(presets::count(sys) == 32);

  // ---- the SIDECAR folder's host half (rytm-mods 0023's 0D / 0E): JUNO and DX7 in its root, a bank a folder of 32 voices
  {
    int dxb = cards.folders() + junolib.folders(), dxmine = -1;
    for (int k = 0; k < dxlib.nodes(); ++k)
      if (dxlib.node(k).bank && !strcmp(dxlib.node(k).name, "mine")) dxmine = k;
    CHECK(dxmine >= 0);
    std::vector<uint8_t> root = listing(0);
    CHECK(has_name(root, "JUNO") && has_name(root, "DX7"));
    CHECK(total_of(listing(cards.folders())) >= 2 && has_name(listing(cards.folders()), "bank"));
    CHECK(total_of(listing(dxb + dxmine)) == 32);
  }

  // ---- no list: the built-ins, a row a PAT, as they play
  CHECK(presets::rows(J) == 128 && presets::rows(F) == 96 && presets::built_ins(J) && presets::built_ins(F));
  open(kJuno, 9);
  CHECK(ui::src_bank_state().on && ui::src_bank_state().row == 9);
  CHECK(ui::src_bank_page(0, kJuno));
  char b[24];
  CHECK(!strcmp(presets::row_name(J, 9, b, sizeof b), juno::kPatches[9].name));
  open(kFm, 127);
  CHECK(ui::src_bank_state().row == 95);   // PAT 127 plays the last ROM voice
  CHECK(fm_at(127) == std::string((const char*)dx7::rom_bank(2) + 128 * 31, 128));

  // ---- scrolling never reads the card
  files(kFm);
  uint64_t c0 = host_card_us(0);
  c3 = host_card_us(3);
  for (int i = 0; i < 40; ++i) {
    ui::src_bank_enc(0, 1, false);
    ui::src_bank_page(0, kFm);
    presets::count(F), presets::name(F, i % presets::count(F));
  }
  key(kRight);
  CHECK(host_card_us(0) == c0 && host_card_us(3) == c3);
  ui::src_bank_list(0, 0, kFm, 0, 0, 0);

  // ---- the first add: every saved PAT plays what it played (the built-ins seeded, row = PAT)
  std::vector<std::string> j0, f0;
  for (int p = 0; p < 128; ++p) j0.push_back(juno_at(p)), f0.push_back(fm_at(p));
  open(kJuno, 20);   // JUNO has no free row: the list's row, asked once
  files(kJuno);
  go_to(J, row_of(J, "one"));
  key(kYes);
  CHECK(ui::src_bank_state().browse && junolib.listed() == 127);   // REPLACE 020? YES
  CHECK(!strcmp(junolib.slot(20).name, juno::kPatches[20].name));
  key(kYes);
  CHECK(!ui::src_bank_state().browse && ui::src_bank_state().row == 20 && !strcmp(junolib.slot(20).name, "one"));
  CHECK(!memcmp(junolib.slot(20).b, dump(5, 11).data() + 5, 18));
  for (int p = 0; p < 128; ++p)
    if (p != 20) CHECK(juno_at(p) == j0[p]);
  CHECK(juno_at(20) != j0[20]);
  CHECK(!memcmp(junolib.slot(3).b, juno::kPatches[3].b, 18));   // (a row equal to its factory patch: juno_select plays the factory's, its gain)
  // a moved cursor asks again; a bank's patch; a built-in in the list already; NO up, at the top the list
  open(kJuno, 21);
  files(kJuno);
  go_to(J, row_of(J, "bank"));
  key(kYes);
  int bankrow = row_of(jbank, "A12 bank");
  go_to(jbank, bankrow);
  key(kYes);
  ui::src_bank_enc(0, 1, false), ui::src_bank_enc(0, -1, false);
  key(kYes);
  CHECK(ui::src_bank_state().browse && !strcmp(junolib.slot(21).name, juno::kPatches[21].name));
  key(kYes);
  CHECK(!ui::src_bank_state().browse && !memcmp(junolib.slot(21).b, dump(1, 32).data() + 5, 18));
  files(kJuno);
  go_to(J, 0);
  key(kRight);
  add_at(kJuno, fac, juno::kPatches[3].name);
  CHECK(ui::src_bank_state().row == 3 && junolib.listed() == 127);
  files(kJuno);
  go_to(J, row_of(J, "sub"));
  key(kRight);
  key(kNo);
  CHECK(ui::src_bank_state().browse && ui::src_bank_state().cur == row_of(J, "sub"));
  key(kNo);
  CHECK(!ui::src_bank_state().browse && ui::src_bank_state().on);
  ui::src_bank_list(0, 0, kJuno, 0, 0, 2);

  // ---- FM: the ROM's 96 rows seeded, room after them; a voice read on the card core
  open(kFm, 0);
  files(kFm);
  go_to(F, row_of(F, "mine"));
  key(kYes);
  c3 = host_card_us(3);
  add_at(kFm, mine, presets::name(mine, 7));
  CHECK(host_card_us(3) == c3);
  CHECK(dxlib.listed() == 97 && dxlib.slot(96).voice == 7 && fm_at(96) == list_text(dm, 7));
  CHECK(ui::src_bank_state().row == 96);
  for (int p = 0; p < 96; ++p) CHECK(fm_at(p) == f0[p]);
  files(kFm);
  go_to(F, 0);
  key(kYes);
  add_at(kFm, rom, presets::name(rom, 40));   // the ROM's own row
  CHECK(dxlib.listed() == 97 && ui::src_bank_state().row == 40);
  ui::src_bank_list(0, 0, kFm, 0, 0, 2);
  fmt::post_step();
  while (fmt::post_busy()) serve(), fmt::post_step();
  CHECK(dxlib.has_voice(96));

  // ---- a new walk: the browser's folder is gone, a folder of the other kind adds nothing
  open(kJuno, 0);
  files(kJuno);
  go_to(J, row_of(J, "bank"));
  key(kYes);
  CHECK(ui::src_bank_state().cur == 0);
  bool th = false;
  CHECK(presets::add(J, F, 1, &th) == presets::kAddNo);

  // ---- the list's file carries the voices: the same on a project switch
  std::string ffm96 = fm_at(96), ffm40 = fm_at(40);
  ui::src_bank_list(0, 0, kJuno, 0, 0, 2);
  project_switch_named("BBB", "BBB", false);
  while (lists_busy()) pump();
  CHECK(!junolib.listed() && !dxlib.listed());
  project_switch_named("AAA", "AAA", false);
  while (lists_busy()) pump();
  CHECK(junolib.listed() == 127 && dxlib.listed() == 97 && fm_at(96) == ffm96 && fm_at(40) == ffm40);
  // the triggers and PAT steps read RAM: no card time
  c0 = host_card_us(0), c3 = host_card_us(3);
  for (int pat = 0; pat < 128; ++pat) {
    fm_select(0, pat);
    juno_at(pat);
  }
  CHECK(host_card_us(0) == c0 && host_card_us(3) == c3);

  // ---- an upload under /JUNO: walked again, after the last change; a browser in a folder goes back to the root
  open(kJuno, 0);
  files(kJuno);
  go_to(J, row_of(J, "bank"));
  key(kYes);
  put("/JUNO/new.syx", dump(7, 41));
  srclib::changed("/JUNO/new.syx");
  pump();
  CHECK(presets::ready() && row_of(J, "new") < 0);
  srclib::changed("/Samples/x.wav");
  g_us += 2100000;
  host_fake_time(true, g_us);
  presets::step();
  CHECK(!presets::ready());
  walk();
  CHECK(row_of(J, "new") >= 0);
  ui::src_bank_enc(0, 0, false);
  key(kYes);   // the root's first row: FACTORY opens, nothing added
  CHECK(ui::src_bank_state().browse && junolib.listed() == 127);
  ui::src_bank_list(0, 0, kJuno, 0, 0, 2);
  int nf = row_of(J, "new");
  CHECK(presets::add(J, J, nf, &th) == presets::kAddFull);
  CHECK(presets::add(J, J, nf, &th, 5) == 5 && !th && !strcmp(junolib.slot(5).name, "new"));
  fmt::post_step();
  while (fmt::post_busy()) serve(), fmt::post_step();


  // ---- the touchscreen's and the web page's side: a window of the index and of the list, copied for another core (no card
  // time, checked against the walk), and the console's preset=...:add|set: lines (the same adds, the answer in result_text)
  {
    static char vn[8][presets::kViewName];
    uint8_t vf[8];
    int16_t vk[8];
    presets::View vw;
    c0 = host_card_us(0), c3 = host_card_us(3);
    int n = presets::view(-1, J, 120, 8, vn, vf, vk, &vw);   // the project's list: the slots 120..126
    char rn[24];
    CHECK(n == 7 && vw.total == 127 && !vw.built_in && (vf[0] & presets::kViewUsed) && vk[0] == -1);
    CHECK(!strcmp(vn[0], presets::row_name(J, 120, rn, sizeof rn)) && !strcmp(vn[6], presets::row_name(J, 126, rn, sizeof rn)));
    n = presets::view(-1, F, 94, 8, vn, vf, vk, &vw);
    CHECK(n == 3 && vw.total == 97 && !strcmp(vn[2], presets::row_name(F, 96, rn, sizeof rn)));
    n = presets::view(J, J, 0, 8, vn, vf, vk, &vw);   // a root: FACTORY first (a folder), the banks
    CHECK(n >= 5 && vw.total == presets::count(J) && vw.up == -1 && !strcmp(vw.title, "JUNO") && vw.gen == presets::gen());
    CHECK((vf[0] & presets::kViewDir) && vk[0] == fac && !strcmp(vn[0], "FACTORY"));
    int br = row_of(J, "bank");
    n = presets::view(J, J, br, 1, vn, vf, vk, &vw);
    CHECK(n == 1 && (vf[0] & presets::kViewDir) && vk[0] == jbank && !strcmp(vn[0], "bank"));
    n = presets::view(jbank, J, 0, 8, vn, vf, vk, &vw);
    CHECK(n == 3 && vw.total == 3 && vw.up == J && !strcmp(vw.title, "JUNO/bank") && !strcmp(vn[1], "A12 bank") && !vf[1] && vk[1] == -1);
    n = presets::view(fac, J, 120, 8, vn, vf, vk, &vw);
    CHECK(n == 8 && vw.total == 128 && !strcmp(vn[7], presets::name(fac, 127)));
    n = presets::view(mine, F, 30, 8, vn, vf, vk, &vw);
    CHECK(n == 2 && vw.total == 32 && !strcmp(vn[1], presets::name(mine, 31)) && !strcmp(vw.title, "DX7/mine"));
    CHECK(presets::view(5000, J, 0, 8, vn, vf, vk, &vw) < 0 && presets::view(J, 2, 0, 8, vn, vf, vk, &vw) < 0);
    CHECK(presets::view(J, J, -1, 8, vn, vf, vk, &vw) < 0 && presets::view(J, J, 100000, 8, vn, vf, vk, &vw) == 0);
    int cut = presets::view(J, J, 0, 1, vn, vf, vk, &vw);
    CHECK(cut == 1 && strlen(vn[0]) < (size_t)presets::kViewName);
    CHECK(host_card_us(0) == c0 && host_card_us(3) == c3);

    // set: a row of the index into the slot asked for (a full list too); an add into a full list; a stale walk refused
    uint32_t g = presets::gen(), seq0 = presets::result().seq;
    char cmd[96], o[96];
    int one = row_of(J, "one");
    snprintf(cmd, sizeof cmd, "preset=juno:set:%u:%d:%d:9", g, J, one);
    CHECK(lists::command(cmd, o, sizeof o) && !strcmp(o, "JUNO 009 SET") && !strcmp(junolib.slot(9).name, "one") && junolib.listed() == 127);
    CHECK(!memcmp(junolib.slot(9).b, dump(5, 11).data() + 5, 18) && presets::result().seq == seq0 + 1);
    int sub = presets::child(J, row_of(J, "sub"));
    snprintf(cmd, sizeof cmd, "preset=juno:add:%u:%d:0", g, sub);
    CHECK(lists::command(cmd, o, sizeof o) && !strcmp(o, "LIST FULL") && junolib.listed() == 127);
    snprintf(cmd, sizeof cmd, "preset=juno:add:%u:%d:0", g + 1, sub);
    uint32_t seq1 = presets::result().seq;
    CHECK(lists::command(cmd, o, sizeof o) && strstr(o, "index changed") && presets::result().seq == seq1);
    snprintf(cmd, sizeof cmd, "preset=juno:set:%u:%d:0:200", g, sub);   // a slot out of the list's range: nothing written
    CHECK(lists::command(cmd, o, sizeof o) && !strcmp(o, "NOT ADDED") && junolib.listed() == 127);
    CHECK(lists::command("preset=juno:bogus", o, sizeof o) && strstr(o, "usage") && lists::command("preset=fm:set:1:2:3", o, sizeof o) && strstr(o, "needs"));
    char txt[48];
    CHECK(!strcmp(presets::result_text(txt, sizeof txt), "NOT ADDED"));

    // FM: a voice into slot 3, whatever it holds: read on the card core, the answer once it is in
    snprintf(cmd, sizeof cmd, "preset=fm:set:%u:%d:11:3", g, mine);
    uint32_t seq2 = presets::result().seq;
    c3 = host_card_us(3);
    CHECK(lists::command(cmd, o, sizeof o) && strstr(o, "reading") && presets::result().seq == seq2);
    int s = -9;
    bool there = false;
    for (int i = 0; i < 200 && !presets::add_poll(&s, &there); ++i) pump();
    CHECK(s == 3 && !there && presets::result().seq == seq2 + 1 && dxlib.slot(3).voice == 11 && dxlib.listed() == 97 && fm_at(3) == list_text(dm, 11));
    CHECK(host_card_us(3) == c3 && !strcmp(presets::result_text(txt, sizeof txt), "FM 003 SET"));
    CHECK(fm_at(2) == f0[2] && fm_at(4) == f0[4]);   // the neighbours as they were
    fmt::post_step();
    while (fmt::post_busy()) serve(), fmt::post_step();

    // the card changes: the walk starts again (a folder's rows are refused, the list is not), the walk ends with a new gen
    put("/JUNO/newer.syx", dump(8, 51));
    srclib::changed("/JUNO/newer.syx");
    g_us += 2100000;
    host_fake_time(true, g_us);
    presets::step();
    CHECK(!presets::ready() && presets::view(J, J, 0, 8, vn, vf, vk, &vw) < 0 && presets::view(-1, J, 0, 8, vn, vf, vk, &vw) == 8);
    walk();
    CHECK(presets::gen() != g && presets::view(J, J, 0, 8, vn, vf, vk, &vw) > 0 && vw.gen == presets::gen());
    snprintf(cmd, sizeof cmd, "preset=juno:set:%u:%d:%d:9", g, J, one);   // the old gen: refused
    CHECK(lists::command(cmd, o, sizeof o) && strstr(o, "index changed"));
  }

  // ---- LOAD TO PROJ on an empty list: the same rows kept
  project_switch_named("LOADP", "LOADP", false);
  while (lists_busy()) pump();
  int jroot = cards.folders();
  pool_add(jroot, 0);
  while (lists_busy()) pump();
  CHECK(junolib.listed() == 127 && !strcmp(junolib.slot(0).name, juno::kPatches[0].name) && strcmp(junolib.slot(126).name, juno::kPatches[126].name));
  pool_add(cards.folders() + junolib.folders() + dxlib_bank("mine"), 3);
  CHECK(dxlib.listed() == 97 && fm_at(0) == f0[0]);
  fmt::post_step();
  while (fmt::post_busy()) serve(), fmt::post_step();

  // ---- an FM list from before 0.4.0 (no voice column): the same PATs, its voices into the file at the load
  std::string line = md5_hex(dm) + "\t7\tOld name\n";
  put("/.rytm/fm/OLD.txt", bytes(line));
  project_switch_named("OLD", "OLD", false);
  while (lists_busy()) pump();
  fmt::post_step();
  while (fmt::post_busy()) serve(), fmt::post_step();
  CHECK(dxlib.listed() == 1 && fm_at(0) == list_text(dm, 7) && !strcmp(dxlib.slot(0).name, "Old name"));
  std::string t = slurp("/.rytm/fm/OLD.txt");
  CHECK(t.size() > 256 && t.find("\tOld name\t") != std::string::npos);

  // ---- a read error: what was read plays, the file is never written over
  std::string two = md5_hex(dm) + "\t1\tOne\n" + md5_hex(dm) + "\t2\tTwo\n";
  for (int worker = 1; worker >= 0; --worker) {
    cardq::set_worker(worker);
    put("/.rytm/fm/TRUNC.txt", bytes(two));
    host_fail_read("/.rytm/fm/TRUNC.txt", 45, true);
    project_switch_named("TRUNC", "TRUNC", false);
    while (lists_busy()) pump();
    fmt::post_step();
  while (fmt::post_busy()) serve(), fmt::post_step();
    host_fail_read(nullptr, 0, false);
    CHECK(dxlib.listed() == 1 && dxlib.has_voice(0));
    CHECK(slurp("/.rytm/fm/TRUNC.txt") == two);
    project_switch_named("OLD", "OLD", false);
    while (lists_busy()) pump();
  }
  cardq::set_worker(true);

  // ---- listed, its bank away: a voice put from the index plays
  {
    std::string l = std::string(32, 'a') + "\t3\tAway\n";
    put("/.rytm/fm/AWAY.txt", bytes(l));
    project_switch_named("AWAY", "AWAY", false);
    while (lists_busy()) pump();
    CHECK(dxlib.listed() == 1 && !dxlib.has_voice(0));
    uint8_t md[16], v[128] = {7};
    memset(md, 0xaa, 16);
    CHECK(dxlib.put_voice(md, 3, "Away", v) == 0 && dxlib.has_voice(0) && fm_at(0) == std::string((char*)v, 128));
    fmt::post_step();
  while (fmt::post_busy()) serve(), fmt::post_step();
  }
  // ---- a hostile voice number: its digits never wrap into range
  {
    std::string l = std::string(32, 'a') + "\t4294967303\tX\n" + std::string(32, 'a') + "\t31\tY\n";
    put("/.rytm/fm/HOST.txt", bytes(l));
    static dx7::Slot sl[dx7::kMaxList];
    static uint8_t sv[dx7::kMaxList][128];
    static bool sh[dx7::kMaxList];
    int n = 0;
    CHECK(dx7::Lib::read_list("/.rytm/fm/HOST.txt", sl, &n, sv, sh));
    CHECK(n == 2 && !sl[0].used && sl[1].used && sl[1].voice == 31);
  }
  // ---- save's text at 127 full rows: the control core's cost
  {
    static dx7::Slot sl[dx7::kMaxList];
    static uint8_t sv[dx7::kMaxList][128];
    static bool sh[dx7::kMaxList];
    static char tx[dx7::kMaxList * 330];
    for (int s = 0; s < dx7::kMaxList; ++s) sl[s].used = true, sl[s].voice = 3, strcpy(sl[s].name, "NAME 67890"), sh[s] = true;
    host_fake_time(false, 0);
    uint32_t t0 = now_us(), n = 0;
    for (int i = 0; i < 100; ++i) n = dx7::Lib::text(tx, sizeof tx, sl, dx7::kMaxList, sv, sh);
    uint32_t us = now_us() - t0;
    host_fake_time(true, g_us);
    CHECK(n > 38000 && n <= sizeof tx);
    printf("fm list text, 127 full rows: %lu bytes, %.1f us on this Mac\n", (unsigned long)n, us / 100.0);
  }

  // ---- a project's zip: the presets go with it (project/juno.txt, project/fm.txt), a card without the folders plays them
  using namespace core::bundle;
  project_switch_named("AAA", "AAA", false);
  while (lists_busy()) pump();
  fmt::post_step();
  while (fmt::post_busy()) serve(), fmt::post_step();
  std::vector<std::string> jn;
  for (int s = 0; s < junolib.listed(); ++s) jn.push_back(junolib.slot(s).name);
  std::string j1((const char*)junolib.slot(20).b, 18), n96 = dxlib.slot(96).name, n40 = dxlib.slot(40).name;
  int nfm = dxlib.listed();
  CHECK(slurp("/.rytm/fm/AAA.txt").find('\t', 40) != std::string::npos);
  // a project from before 0.4.0, not opened here: export takes its voices from the banks
  put("/.rytm/fm/OLDX.txt", bytes(md5_hex(dm) + "\t9\tNever\n"));
  put("/.rytm/juno/OLDX.txt", {});
  Env env{};
  env.current = current;
  env.version = "0.4.0-test";
  env.board = "mac";
  set_env(env);
  reset_for_tests();
  project_switch_named("BBB", "BBB", false);
  while (lists_busy()) pump();
  cardq::set_worker(false);
  auto do_export = [&](const char* k) {
    ExportOpts o;
    o.epoch = 1792677780u;
    CHECK(start_export(k, kToCard, o));
    while (busy() && status().phase != kDone && status().phase != kFailed) step();
    CHECK(status().phase == kDone);
    std::string z = "/Bundles/" + std::string(bundle_name());
    finish();
    return z;
  };
  auto do_import = [&](const std::string& z, const char* as) {
    CHECK(start_verify(z.c_str()));
    while (busy() && status().phase != kReady && status().phase != kFailed) step();
    CHECK(status().phase == kReady);
    ImportOpts io;
    snprintf(io.name, sizeof io.name, "%s", as);
    CHECK(start_import(io));
    while (busy() && status().phase != kDone && status().phase != kFailed) step();
    CHECK(status().phase == kDone);
    finish();
  };
  std::string zip = do_export("AAA"), zold = do_export("OLDX");
  CHECK(slurp("/.rytm/fm/OLDX.txt").find("\tNever\t") != std::string::npos);
  CHECK(!system(("rm -rf '" + host_path("/DX7") + "' '" + host_path("/JUNO") + "'").c_str()));
  dxlib.begin(&cards.sd(), nolog);
  junolib.begin(&cards.sd(), nolog);
  CHECK(!dxlib.nodes());
  do_import(zip, "COPY");
  do_import(zold, "COPYOLD");
  project_switch_named("COPY", "COPY", false);
  while (lists_busy()) pump();
  CHECK(junolib.listed() == (int)jn.size() && dxlib.listed() == nfm);
  for (int s = 0; s < (int)jn.size(); ++s) CHECK(jn[s] == junolib.slot(s).name);
  CHECK(std::string((const char*)junolib.slot(20).b, 18) == j1);
  CHECK(fm_at(96) == ffm96 && fm_at(40) == ffm40 && n96 == dxlib.slot(96).name && n40 == dxlib.slot(40).name);
  CHECK(dxlib.has_voice(96) && dxlib.slot(96).bank == -1);   // no bank behind it: its own bytes play
  project_switch_named("COPYOLD", "COPYOLD", false);
  while (lists_busy()) pump();
  CHECK(dxlib.listed() == 1 && fm_at(0) == list_text(dm, 9));
  // its bank not on the card: said in the log, the file as it was
  put("/.rytm/fm/LOST.txt", bytes(md5_hex(dm) + "\t9\tLost\n"));
  CHECK(dxlib.fill_file("/.rytm/fm/LOST.txt") == 1 && slurp("/.rytm/fm/LOST.txt") == md5_hex(dm) + "\t9\tLost\n");

  // ---- the index's shares: a /JUNO over its share leaves /DX7 whole, the browser says FULL
  cardq::set_worker(true);
  CHECK(!system(("mkdir -p '" + host_path("/JUNO/many") + "' '" + host_path("/DX7") + "'").c_str()));
  for (int i = 0; i < presets::kItemsOf[J] + 50; ++i) {
    char n[48];
    snprintf(n, sizeof n, "/JUNO/many/p%05d.syx", i);
    FILE* f = fopen(host_path(n).c_str(), "wb");
    CHECK(f);
    auto d = dump(1, (uint8_t)i);
    fwrite(d.data(), 1, d.size(), f);
    fclose(f);
  }
  put("/DX7/mine.syx", dm);
  srclib::changed("/DX7");
  g_us += 2100000;
  host_fake_time(true, g_us);
  presets::step();
  walk();
  CHECK(presets::full(J) && !presets::full(F));
  CHECK(presets::stats(J).items == presets::kItemsOf[J] && presets::count(presets::child(F, row_of(F, "mine"))) == 32);
  printf("test_presets: PASS\n");
  return 0;
}

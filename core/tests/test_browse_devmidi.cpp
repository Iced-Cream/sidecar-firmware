// core/tests/test_browse_devmidi.cpp - two generic services of the stripped engine: core/browse (0D -> 0E, a folder of
// the card for rytm-mods 0023's file browser: numbers given to folders as they are listed, folders first then files,
// both by name, six entries a message, an empty folder answered, '.' left out, a stale answer dropped) and core/devmidi
// (the USB-A devices' MIDI: to the Rytm, to the processor slots, to listeners; held notes released at an unplug and
// when thru goes off).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <string>
#include <vector>

#include "check.h"
#include "../browse/browse.h"
#include "../devmidi/devmidi.h"
#include "../generic/generic.h"
#include "../hooks.h"
#include "../proc/proc.h"
#include "core_port.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void out_fn(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void in(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  gen::sysex(m.data(), (int)m.size());
}
static void settle() { for (int i = 0; i < 200; ++i) gen::idle_save(); }
static void touch(const std::string& p, int bytes) {
  FILE* f = fopen(p.c_str(), "wb");
  for (int i = 0; i < bytes; ++i) fputc(0, f);
  fclose(f);
}

struct Ent { int kind, id; uint32_t size; std::string name; };
struct Listing { int folder = -1, parent = -1, total = -1, messages = 0; std::vector<Ent> e; };
static Listing parse() {
  Listing l;
  for (auto& m : g_out) {
    if (m.size() < 14 || m[4] != 0x0E) continue;
    ++l.messages;
    l.folder = m[5] << 7 | m[6], l.parent = m[7] << 7 | m[8], l.total = m[11] << 7 | m[12];
    CHECK((m[9] << 7 | m[10]) == (int)l.e.size());   // in order, no gap
    size_t p = 13;
    while (p + 8 <= m.size() - 1) {
      Ent e;
      e.kind = m[p], e.id = m[p + 1] << 7 | m[p + 2];
      e.size = (uint32_t)m[p + 3] << 21 | (uint32_t)m[p + 4] << 14 | (uint32_t)m[p + 5] << 7 | m[p + 6];
      int n = m[p + 7];
      e.name.assign((const char*)&m[p + 8], n);
      p += 8 + n;
      l.e.push_back(e);
    }
    CHECK(m.back() == 0xF7 && p == m.size() - 1);
  }
  return l;
}
static Listing ask(int f) {
  g_out.clear();
  in({0x0D, (uint8_t)(f >> 7), (uint8_t)(f & 127)});
  settle();
  return parse();
}

struct Probe : proc::Proc {
  std::vector<uint32_t> got;
  void process(const proc::Block&) override {}
  void set_param(int, int) override {}
  void midi(int src, uint8_t s, uint8_t a, uint8_t b) override { got.push_back((uint32_t)src << 24 | s << 16 | a << 8 | b); }
  const char* name() const override { return "PROBE"; }
};
static std::vector<uint32_t> g_fwd, g_heard;
static void fwd(uint8_t s, uint8_t a, uint8_t b) { g_fwd.push_back((uint32_t)s << 16 | a << 8 | b); }
static void heard(int src, uint8_t s, uint8_t a, uint8_t b) { g_heard.push_back((uint32_t)src << 24 | s << 16 | a << 8 | b); }
static void run_slots() {   // the render side: the queue drained
  static float z[proc::kChannels][8];
  proc::Block b;
  for (int c = 0; c < proc::kChannels; ++c) b.in[c] = z[c], b.out[c] = z[c];
  b.frames = 8;
  proc::process(b);
}

int main() {
  char tmpl[] = "/tmp/core-browse-XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string root = std::string(tmpl) + "/Samples";
  mkdir(root.c_str(), 0755);
  mkdir((root + "/Kicks").c_str(), 0755);
  mkdir((root + "/drums").c_str(), 0755);
  mkdir((root + "/.hidden").c_str(), 0755);
  mkdir((root + "/Kicks/Deep").c_str(), 0755);
  touch(root + "/b.wav", 100);
  touch(root + "/A long file name that is cut.wav", 7);
  touch(root + "/._b.wav", 3);
  for (int i = 0; i < 14; ++i) touch(root + "/Kicks/k" + std::to_string(10 + i) + ".wav", 1000 + i);
  host_set_card(tmpl);
  hooks.to_rytm = out_fn;
  CHECK(gen::begin());

  // the root: folders first (by name, case ignored), then files; '.' left out; names cut to 23
  Listing r = ask(0);
  CHECK(r.folder == 0 && r.parent == 0 && r.total == 4 && r.messages == 1 && r.e.size() == 4);
  CHECK(r.e[0].kind == 1 && r.e[0].name == "drums" && r.e[1].kind == 1 && r.e[1].name == "Kicks");
  CHECK(r.e[2].kind == 0 && r.e[2].id == 0 && r.e[2].name == "A long file name that i" && r.e[2].size == 7);
  CHECK(r.e[3].kind == 0 && r.e[3].id == 1 && r.e[3].name == "b.wav" && r.e[3].size == 100);
  int kicks = r.e[1].id;
  CHECK(kicks > 0 && r.e[0].id > 0 && kicks != r.e[0].id);

  // a subfolder: 15 entries in three messages of six, its parent named; its own subfolder numbered
  Listing k = ask(kicks);
  CHECK(k.folder == kicks && k.parent == 0 && k.total == 15 && k.messages == 3 && k.e.size() == 15);
  CHECK(k.e[0].kind == 1 && k.e[0].name == "Deep" && k.e[1].name == "k10.wav" && k.e[1].size == 1000);
  CHECK(k.e[14].name == "k23.wav" && k.e[14].id == 13);
  int deep = k.e[0].id;
  Listing d = ask(deep);
  CHECK(d.folder == deep && d.parent == kicks && d.total == 0 && d.messages == 1);
  // the numbers hold: the root again gives the same ones
  r = ask(0);
  CHECK(r.e[1].id == kicks);
  // a number never given: an empty folder
  Listing none = ask(900);
  CHECK(none.folder == 900 && none.total == 0 && none.messages == 1);
  // a second ask before the first is answered: one answer, the newer one's
  g_out.clear();
  in({0x0D, 0, 0});
  in({0x0D, (uint8_t)(kicks >> 7), (uint8_t)(kicks & 127)});
  settle();
  Listing two = parse();
  CHECK(two.folder == kicks && two.e.size() == 15);
  CHECK(browse::stats().asks == 7 && browse::stats().listings >= 6);

  // ---- devmidi: thru to the Rytm, the slots, the listeners
  devmidi::set_forward(fwd);
  CHECK(devmidi::on_midi(heard));
  devmidi::midi(0, 0x90, 60, 100);   // no device there yet: ignored
  CHECK(g_fwd.empty() && devmidi::stats().ignored == 1);
  devmidi::plugged(0, true, "KEYS");
  CHECK(devmidi::present(0) && !strcmp(devmidi::name(0), "KEYS"));
  devmidi::midi(0, 0x90, 60, 100);
  devmidi::midi(0, 0xF8, 0, 0);       // clock: through
  devmidi::midi(0, 0xF2, 1, 2);       // song position: not
  CHECK(g_fwd.size() == 2 && g_fwd[0] == (0x90u << 16 | 60 << 8 | 100) && g_fwd[1] == 0xF80000u);
  CHECK(g_heard.size() == 2);
  // a slot filled: it hears them too
  static Probe probe;
  CHECK(proc::register_slot(0, &probe));
  devmidi::midi(0, 0x91, 62, 90);
  run_slots();
  CHECK(probe.got.size() == 1 && probe.got[0] == (0x91u << 16 | 62 << 8 | 90));
  // thru off: the Rytm's held notes end (60 on ch 1, 62 on ch 2), nothing more goes to it; the slots still hear
  g_fwd.clear();
  devmidi::set_thru(false);
  CHECK(g_fwd.size() == 2 && g_fwd[0] == (0x80u << 16 | 60 << 8) && g_fwd[1] == (0x81u << 16 | 62 << 8));
  devmidi::midi(0, 0x90, 64, 80);
  run_slots();
  CHECK(g_fwd.size() == 2 && probe.got.size() == 2);
  // unplugged with notes held: note-offs to the slots and the listeners (the Rytm's thru is off)
  devmidi::set_thru(true);
  g_heard.clear();
  devmidi::plugged(0, false, nullptr);
  run_slots();
  CHECK(!devmidi::present(0) && g_heard.size() == 3);   // 60, 62, 64
  CHECK(probe.got.size() == 5 && (probe.got[4] >> 16 & 0xF0) == 0x80);
  CHECK(g_fwd.size() == 5);   // thru back on: the Rytm gets them too
  proc::clear_slot(0);

  printf("test_browse_devmidi: ok\n");
  return 0;
}

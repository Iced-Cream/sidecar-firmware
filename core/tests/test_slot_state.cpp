// core/tests/test_slot_state.cpp - each slot's project state in its own folder (docs/device-profiles.md "Several devices"):
// slot 0 (the Rytm) and slot 1 (another box) each have their own project, read from and saved into their own state folder,
// alone or side by side. Nothing of one box's project is the other's, and nothing of one is written into the other's folder.
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/a4.h"
#include "device/device.h"
#include "engine.h"
#include "granular/granular.h"
#include "lib/library.h"
#include "lib/pools.h"
#include "pool/pool.h"
#include "presets/juno_lib.h"
#include "store/store.h"
#include "wavetable/wavetable.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out, g_out1;
static void out0(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void out1(const uint8_t* m, int n) { g_out1.emplace_back(m, m + n); }

static void put(const char* path, const char* text) {
  FILE* f = fopen(host_path(path).c_str(), "wb");
  CHECK(f);
  fputs(text, f);
  fclose(f);
}
static std::string get(const char* path) {
  FILE* f = fopen(host_path(path).c_str(), "rb");
  if (!f) return "";
  std::string s;
  char b[512];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}

// the idle passes until nothing waits: a box's project read, the saves (idle_flush: no second's wait for an edit to settle)
static void settle() {
  idle_flush(true);
  for (int i = 0; i < 4000; ++i) {
    device::step();
    idle_prepare();
    idle_save();
    idle_stream();
    bool loading = project_loading();
    for (int k = 1; k < kVoiceSlots; ++k) {   // (every slot's fill: project_loading is the slot in hand's)
      device::Scope in(device::g_links[k]);
      loading = loading || project_loading();
    }
    if (i > 50 && idle_settled() && !loading) break;
  }
  idle_flush(false);
}

static void put_wav(const char* path, int frames) {   // 16-bit mono 48 kHz, a ramp
  FILE* f = fopen(host_path(path).c_str(), "wb");
  CHECK(f);
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  fwrite("RIFF", 1, 4, f), u32(36 + frames * 2), fwrite("WAVEfmt ", 1, 8, f), u32(16), u16(1), u16(1), u32(48000),
      u32(96000), u16(2), u16(16), fwrite("data", 1, 4, f), u32(frames * 2);
  for (int i = 0; i < frames; ++i) u16((uint16_t)(i * 64));
  fclose(f);
}

static void hello_a4() {
  const uint8_t h[] = {0xf0, 0x7d, 0x52, 0x54, device::kCmdHello, 1, (uint8_t)(device::kIdA4 >> 7),
                       (uint8_t)(device::kIdA4 & 0x7f), 1, 0xf7};
  engine_sysex_link(1, h, (int)sizeof h);
}
static void name_project(int transport, const char* n) {   // a box's 0x10
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x10};
  for (const char* c = n; *c; ++c) m.push_back((uint8_t)*c);
  m.push_back(0xf7);
  engine_sysex_link(transport, m.data(), (int)m.size());
}

int main() {
  if (kVoiceSlots < 2) {
    printf("test_slot_state: PASS (an 8-voice engine: one slot, nothing to test)\n");
    return 0;
  }
  char tmpl[] = "/tmp/core-slotstate-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/.rytm") && fs.mkdir("/.a4"));
  CHECK(fs.mkdir("/.rytm/juno") && fs.mkdir("/.a4/juno") && fs.mkdir("/.a4/lfo"));
  put("/.a4/lfo/A4P.txt", "the A4's own LFO file");   // (slot 1's store holds it; slot 0's modules never see it)
  // each box's JUNO list in its own folder (the A4's project B4: one row; the Rytm's NEW: two)
  put("/.a4/juno/B4.txt", "000102030405060708090a0b0c0d0e0f1011\tA4 ROW\n");
  put("/.rytm/juno/NEW.txt", "000102030405060708090a0b0c0d0e0f1012\tRYTM ONE\n000102030405060708090a0b0c0d0e0f1013\tRYTM TWO\n");
  put("/.rytm/project", "RYP");
  put("/.a4/project", "A4P");
  put_wav("/Samples/a.wav", 4800);
  put_wav("/Samples/b.wav", 9600);
  hooks.to_rytm = out0;
  CHECK(engine_begin());
  device::set_out(1, out1);
  settle();
  CHECK(!strcmp(prj(0).key, "RYP"));   // boot: slot 0's folder; slot 1 has no box yet

  // a lone A4 (no Rytm on USB): its own project, from /.a4; the Rytm's stays slot 0's
  device::rytm_on_usb(false);
  hello_a4();
  settle();
  CHECK(device::g_links[1].box == &device::kA4 && device::g_base == &device::g_links[1]);
  printf("  after the A4's hello: slot 0 %s, slot 1 %s\n", prj(0).key, prj(1).key);
  CHECK(!strcmp(prj(1).key, "A4P") && !strcmp(prj(0).key, "RYP"));
  {
    device::Scope in(device::g_links[1]);   // the A4's own messages see its own project
    CHECK(!strcmp(prj().key, "A4P"));
  }
  // its RAM store: filled from its own folder at its hello, apart from slot 0's
  {
    const uint8_t* d;
    uint32_t n;
    {
      device::Scope in(device::g_links[1]);
      CHECK(store::ready() && !project_loading());
      CHECK(store::get("/.rytm/lfo/A4P.txt", &d, &n) == store::kPresent && n == 21 && !memcmp(d, "the A4's", 8));
    }
    {
      device::Scope in(device::g_links[0]);   // (the lone A4 is the base box now: outside a Scope the slot in hand is 1)
      CHECK(store::ready(0) && store::get("/.rytm/lfo/A4P.txt", &d, &n) == store::kUnknown);   // (slot 0's: not its path)
    }
    printf("  stores: slot 0 %d files, slot 1 %d files\n", store::present(0), store::present(1));
  }

  // the Rytm on USB beside it: each keeps its own
  device::rytm_on_usb(true);
  settle();
  CHECK(device::g_base == &device::g_links[0]);
  CHECK(!strcmp(prj(0).key, "RYP") && !strcmp(prj(1).key, "A4P"));

  // each box's POOL lists and the machines' copies of them (option A: nothing shared): the same row holds another file
  // in each slot; GRANULAR / WAVETABLE compose each slot's own list, and the views read the slot in hand's
  {
    lib::rescan();
    settle();
    const int b7 = pools::code(1, 7);
    char o[256];
    CHECK(pools::command("pool=smp:set:B007:/Samples/a.wav", o, sizeof o) && strstr(o, "B007"));
    CHECK(pools::command("pool=wt:set:B007:/Samples/a.wav", o, sizeof o) && strstr(o, "B007"));
    {
      device::Scope in(device::g_links[1]);
      CHECK(pools::command("pool=smp:set:B007:/Samples/b.wav", o, sizeof o) && strstr(o, "B007"));
      CHECK(pools::command("pool=wt:set:B007:/Samples/b.wav", o, sizeof o) && strstr(o, "B007"));
    }
    settle();
    printf("  B007: slot 0 %s / %s, slot 1 %s / %s\n", pools::at(0, pools::kSamples, b7).path,
           pools::at(0, pools::kTables, b7).path, pools::at(1, pools::kSamples, b7).path, pools::at(1, pools::kTables, b7).path);
    CHECK(!strcmp(pools::at(0, pools::kSamples, b7).path, "/Samples/a.wav"));
    CHECK(!strcmp(pools::at(1, pools::kSamples, b7).path, "/Samples/b.wav"));
    CHECK(!strcmp(gr::entry_path(b7), "/Samples/a.wav") && !strcmp(wt::entry_path(b7), "/Samples/a.wav"));
    {
      device::Scope in(device::g_links[1]);
      CHECK(!strcmp(gr::entry_path(b7), "/Samples/b.wav") && !strcmp(wt::entry_path(b7), "/Samples/b.wav"));
      CHECK(pools::find_hash(pools::kSamples, pools::at(0, pools::kSamples, b7).ref) < 0);   // slot 1's list: no a.wav
    }
    CHECK(gr::entry_state_in(0, b7) == gr::kStateCard && gr::entry_state_in(1, b7) == gr::kStateCard);
    // a voice resolves through its own box's list: voice 0 the Rytm's, voice kBoxVoices the A4's
    CHECK(gr::pick(0, b7) && gr::pick(kBoxVoices, b7));
    CHECK(gr::src_entry(0, b7) == b7 && gr::src_entry(kBoxVoices, b7) == b7);
    // a row seen playing on the A4's voice is in the A4's used set only
    pools::seen_in(slot_of_voice(kBoxVoices), pools::kSamples, b7);
    CHECK(pools::used(1, pools::kSamples, b7) && !pools::used(0, pools::kSamples, b7));
  }

  // the Rytm names another project: slot 0's alone, written into /.rytm; /.a4's untouched
  name_project(0, "NEW");
  settle();
  CHECK(!strcmp(prj(0).key, "NEW") && !strcmp(prj(1).key, "A4P"));
  CHECK(get("/.rytm/project") == "NEW" && get("/.a4/project") == "A4P");

  // a project name from the A4's transport is the A4's: slot 1's alone, written into /.a4
  {
    device::Scope in(device::g_links[1]);
    const uint8_t n[] = {'B', '4', 0xf7};
    set_project(n, 3);
  }
  settle();
  CHECK(!strcmp(prj(0).key, "NEW") && !strcmp(prj(1).key, "B4"));
  CHECK(get("/.rytm/project") == "NEW" && get("/.a4/project") == "B4");
  // the JUNO lists: each slot's read from its own folder at its project change, the other's left as it was
  printf("  JUNO lists: slot 0 %d rows (%s), slot 1 %d rows (%s)\n", junolib.listed(0), junolib.slot(0, 0).name,
         junolib.listed(1), junolib.slot(1, 0).name);
  CHECK(junolib.listed(0) == 2 && !strcmp(junolib.slot(0, 1).name, "RYTM TWO"));
  CHECK(junolib.listed(1) == 1 && !strcmp(junolib.slot(1, 0).name, "A4 ROW"));
  {
    device::Scope in(device::g_links[1]);
    CHECK(junolib.listed() == 1);   // the slot in hand's
  }

  // the A4 unplugged: slot 1 has no box, but keeps its folder, so a write queued for it before it left lands in /.a4
  device::set_out(1, nullptr);
  CHECK(!device::g_links[1].box && device::g_links[1].state && !strcmp(device::g_links[1].state, "/.a4"));
  {
    device::Scope in(device::g_links[1]);
    char b[64];
    CHECK(!strcmp(device::state_path("/.rytm/lfo/A4P.txt", b, sizeof b), "/.a4/lfo/A4P.txt"));
  }

  printf("test_slot_state: PASS\n");
  return 0;
}

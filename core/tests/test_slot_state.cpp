// core/tests/test_slot_state.cpp - each slot's project state in its own folder (docs/device-profiles.md "Several devices"):
// the project in RAM is the base box's, loaded from the base box's folder and saved there. A lone A4 boots on /.rytm (no box
// has said hello yet); its hello makes it the base, so the Rytm's project goes back to /.rytm with its edits and the A4's own
// project comes in from /.a4. Nothing of one box's state is ever written into the other's folder.
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/a4.h"
#include "device/device.h"
#include "engine.h"
#include "lfo.h"
#include "pool/pool.h"

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
static bool exists(const char* path) { return fopen(host_path(path).c_str(), "rb") != nullptr; }

// the idle passes until nothing waits: the store's fill, the saves (idle_flush: no second's wait for an edit to settle)
static void settle() {
  idle_flush(true);
  for (int i = 0; i < 4000; ++i) {
    device::step();
    idle_prepare();
    idle_save();
    idle_stream();
    if (i > 50 && idle_settled() && !project_loading()) break;
  }
  idle_flush(false);
}

static void hello_a4() {
  const uint8_t h[] = {0xf0, 0x7d, 0x52, 0x54, device::kCmdHello, 1, (uint8_t)(device::kIdA4 >> 7),
                       (uint8_t)(device::kIdA4 & 0x7f), 1, 0xf7};
  engine_sysex_link(1, h, (int)sizeof h);
}

int main() {
  if (kVoiceSlots < 2) {
    printf("test_slot_state: PASS (an 8-voice engine: one link, nothing to test)\n");
    return 0;
  }
  char tmpl[] = "/tmp/core-slotstate-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/.rytm") && fs.mkdir("/.a4"));
  put("/.rytm/project", "RYP");
  put("/.a4/project", "A4P");
  hooks.to_rytm = out0;
  CHECK(engine_begin());
  device::set_out(1, out1);
  settle();
  CHECK(!strcmp(project, "RYP"));   // boot: slot 0's folder, no box yet

  // a lone A4 (no Rytm on USB): its hello makes slot 1 the base, and the project in RAM becomes /.a4's
  device::rytm_on_usb(false);
  lfo::Settings s = lfo::get(0);
  s.depth = 21;
  CHECK(lfo::set(0, s));   // an edit of the Rytm's project before the A4 says hello
  hello_a4();
  settle();
  CHECK(device::g_links[1].box == &device::kA4 && device::g_base == &device::g_links[1]);
  printf("  after the A4's hello: project %s\n", project);
  CHECK(!strcmp(project, "A4P"));
  CHECK(get("/.rytm/project") == "RYP" && get("/.a4/project") == "A4P");
  CHECK(lfo::get(0).depth != 21);               // the A4's own LFOs, not the Rytm's edit
  CHECK(get("/.rytm/lfo/RYP.txt").size() > 0);  // the Rytm's edit saved in its own folder on the way out
  CHECK(!exists("/.a4/lfo/RYP.txt"));           // ... and never in the A4's

  // an edit of the A4's project lands in /.a4
  s = lfo::get(0);
  s.depth = 33;
  CHECK(lfo::set(0, s));
  settle();
  CHECK(get("/.a4/lfo/A4P.txt").size() > 0);
  CHECK(get("/.rytm/lfo/RYP.txt").find("33") == std::string::npos || !exists("/.rytm/lfo/A4P.txt"));
  CHECK(!exists("/.rytm/lfo/A4P.txt"));

  // the Rytm comes back on USB: the base is slot 0 again and its project, with the edit, is back in RAM; the A4's edit stays in /.a4
  device::rytm_on_usb(true);
  settle();
  CHECK(device::g_base == &device::g_links[0]);
  CHECK(!strcmp(project, "RYP") && lfo::get(0).depth == 21);
  CHECK(!exists("/.rytm/lfo/A4P.txt"));

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

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

// the idle passes until nothing waits: a box's project read, the saves (idle_flush: no second's wait for an edit to settle)
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
  put("/.rytm/project", "RYP");
  put("/.a4/project", "A4P");
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

  // the Rytm on USB beside it: each keeps its own
  device::rytm_on_usb(true);
  settle();
  CHECK(device::g_base == &device::g_links[0]);
  CHECK(!strcmp(prj(0).key, "RYP") && !strcmp(prj(1).key, "A4P"));

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

// core/tests/test_a4_generic.cpp - the Analog Four MKII (core/device/a4.h) on the generic engine, as an A4 over link=tcp
// or USB reaches it: its hello on a Rytm boot answered kRestart and its id saved; a boot with that choice on the card puts
// its state in /.a4 (and its hello is then kOk); every generic service answers it through its output filter: 51 STORE
// (HELLO / WRITE / READ, the records in /.a4/store and none in /.rytm/store), the remote screen (its 36 hello is the
// Rytm's 35: the link reset and the connect splash; a client's page opened by its 30, drawn, keys and encoders), 0D a
// card folder answered 0E, the step clock's 2C / 2D; a message no service sends it is filtered; its audio row is the
// A4's 8 in / 6 out stream. The step clock's per-box track count is not here (port plan S8a).
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "clock/clock.h"
#include "device/a4.h"
#include "device/audio.h"
#include "device/device.h"
#include "generic/generic.h"
#include "hooks.h"
#include "kv/kv.h"
#include "kv/link.h"
#include "platform.h"
#include "screen/screen.h"

namespace core { void host_fake_time(bool on, uint64_t us); }
using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static int g_restarts = 0;
static void out_fn(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static int sent(uint8_t cmd) {
  int k = 0;
  for (auto& m : g_out) k += m.size() > 4 && m[4] == cmd;
  return k;
}
static const std::vector<uint8_t>* last(uint8_t cmd) {
  for (auto it = g_out.rbegin(); it != g_out.rend(); ++it) if (it->size() > 4 && (*it)[4] == cmd) return &*it;
  return nullptr;
}
static uint64_t g_us = 1000000;
static void run(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 5) g_us += 5000, host_fake_time(true, g_us), gen::tick((uint32_t)(g_us / 1000));
}
static void in(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  gen::sysex(m.data(), (int)m.size());
  for (int i = 0; i < 64 && kv::link::host_step(8); ++i) {}
}
static std::vector<uint8_t> hello_a4() {
  return {device::kCmdHello, 1, (uint8_t)(device::kIdA4 >> 7), (uint8_t)(device::kIdA4 & 0x7F), device::kProto, 'A', '4'};
}
static bool exists(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}
static int g_keys = 0, g_encs = 0, g_draws = 0, g_opens = 0;
static void c_draw(ui::Canvas& c, uint32_t) { ++g_draws; c.put_text(0, ui::kRSmall, 2, 1, "A4"); }
static void c_key(int, bool down) { g_keys += down; }
static void c_enc(int, int d, bool) { g_encs += d; }
static void c_open(bool on, int) { g_opens += on ? 1 : -1; }
static const screen::Client kClient = {"test", c_draw, c_key, c_enc, c_open, nullptr};

int main() {
  host_fake_time(true, g_us);
  char tmpl[] = "/tmp/core-a4gen-XXXXXX";
  CHECK(mkdtemp(tmpl));
  const std::string card = tmpl;
  host_set_card(tmpl);
  hooks.to_rytm = out_fn;
  hooks.device_restart = [] { ++g_restarts; };
  mkdir((card + "/.rytm").c_str(), 0755);

  // the profile: built in, its own folder, the A4's Overbridge row (8 in, 6 out: the driver takes both)
  CHECK(device::by_id(device::kIdA4) == &device::kA4 && !strcmp(device::kA4.state_dir, "/.a4"));
  CHECK(device::kA4.audio && device::kA4.audio->in_channels == 8 && device::kA4.audio->channels == 6);
  CHECK(device::kA4.features & device::kRemoteUi);

  // a Rytm boot (no /.rytm/device): its hello is answered kRestart and its id kept for the next boot
  CHECK(gen::begin());
  CHECK(kv::link::attach_generic());
  CHECK(!device::foreign());
  in(hello_a4());
  CHECK(g_out.size() == 1 && g_out[0][4] == device::kCmdHello && g_out[0][8] == device::kRestart);
  for (int k = 0; k < 200 && !g_restarts; ++k) gen::idle_save();
  CHECK(g_restarts == 1);

  // the next boot: the A4 and /.a4 from the card's choice, before anything else reads it
  device::reset();
  g_out.clear();
  CHECK(gen::begin());
  CHECK(device::foreign() && device::active().id == device::kIdA4 && !strcmp(device::g_link.state, "/.a4"));
  CHECK(exists(card + "/.a4"));
  in(hello_a4());   // its hello again: the folder is its own now, so kOk
  CHECK(g_out.size() == 1 && g_out[0][8] == device::kOk && device::foreign());
  g_out.clear();

  // 51 STORE from the A4: HELLO (project 0x2A), WRITE of 3 bytes, READ back (the messages of test_store_generic.cpp)
  in({0x51, 1, kv::kOpHello, 0, 0, 0, 0, 0x2A});
  CHECK(g_out.size() == 1 && g_out[0][4] == kv::kCmd && g_out[0][6] == kv::kOpHelloAck);
  g_out.clear();
  uint8_t data[3] = {4, 1, 0xA4}, p[8];
  int pn = kv::pack7(data, 3, p);
  std::vector<uint8_t> w = {0x51, 1, kv::kOpWrite, 5, 3, 0, 3, 0, 0};
  w.insert(w.end(), p, p + pn);
  in(w);
  CHECK(g_out.size() == 1 && g_out[0][6] == kv::kOpWritten);
  g_out.clear();
  in({0x51, 1, kv::kOpRead, 6, 3, 0, 0, 1});
  uint8_t back[8];
  CHECK(g_out.size() == 1 && g_out[0][6] == kv::kOpData &&
        kv::unpack7(&g_out[0][16], (int)g_out[0].size() - 17, back) == 3 && !memcmp(back, data, 3));
  g_out.clear();
  // the record is in the A4's folder, none in the Rytm's: the store names "/.rytm/store/0000002a/..." and state_path maps it
  CHECK(exists(card + "/.a4/store/0000002a"));
  CHECK(!exists(card + "/.rytm/store"));

  // the step clock: its 2C ticks and 2D transport pass its input filter
  int st, ln, sp;
  in({clock::kTickCmd, 1, 0, 0, 0, 7, 1, 5, 3, 16, 1});
  CHECK(clock::step_of(5, &st, &ln, &sp) && st == 3);
  in({clock::kStateCmd, 1, clock::kTransport, clock::kStop, 0, 0, 0, 8});
  CHECK(clock::transport() == clock::kStop);

  // the remote screen: its 36 hello (up 5 s, caps 1) is the Rytm's 35: the link reset (the clock forgets) and the splash
  uint32_t hellos = gen::stats().hellos;
  in({0x36, 1, 0, 100 & 0x7F, 1});
  CHECK(gen::stats().hellos == hellos + 1 && screen::greeted() && !clock::step_of(5, &st, &ln, &sp));
  run(50);
  CHECK(sent(0x39) >= 1 && last(0x36) && (*last(0x36))[6] == 64);
  run(3100);
  CHECK((*last(0x36))[6] == 0);
  // a client's page, opened by the A4's 30: drawn (37 39 3D reach it), its keys and encoders, keepalives (3C), closed
  screen::set_client(&kClient);
  g_out.clear();
  in({0x30, 1, 0, 1, 0});
  CHECK(screen::open() && g_opens == 1);
  run(60);
  CHECK(g_draws >= 1 && sent(0x37) == 1 && sent(0x39) >= 1 && sent(0x3D) >= 1);
  in({0x31, 1, 0, 0x40, 1});
  in({0x32, 1, 2, 64 + 3, 0});
  CHECK(g_keys == 1 && g_encs == 3);
  run(600);
  CHECK(sent(0x3C) >= 1);
  in({0x30, 1, 0, 0, 0});
  CHECK(!screen::open() && g_opens == 0);
  screen::set_client(nullptr);

  // 0D: the card's root folder, answered 0E (read on the card core: idle_save steps it)
  mkdir((card + "/Samples").c_str(), 0755);
  g_out.clear();
  in({0x0D, 0, 0});
  for (int k = 0; k < 200 && !sent(0x0E); ++k) gen::idle_save();
  CHECK(sent(0x0E) >= 1);

  // a message no service sends it is filtered (counted); the generic engine's own count of dropped input is unchanged
  uint32_t d0 = device::dropped();
  g_out.clear();
  const uint8_t juno[8] = {0xF0, 0x7D, 0x52, 0x54, 0x15, 0, 0, 0xF7};
  to_rytm(juno, 8);
  CHECK(g_out.empty() && device::dropped() == d0 + 1);

  // a Rytm on USB: its 35 gives the link back to the Rytm, after a restart into /.rytm (the box's folder is not the Rytm's)
  in({0x35, 1, 0, 100, 3});
  CHECK(device::leaving());

  printf("test_a4_generic: PASS\n");
  return 0;
}

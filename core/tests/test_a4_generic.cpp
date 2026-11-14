// core/tests/test_a4_generic.cpp - the Analog Four MKII (core/device/a4.h) in the generic engine: built in; its hello on a
// Rytm boot answered kRestart and its id saved; a boot with that choice on the card puts its state in /.a4; its 51 STORE
// HELLO / WRITE / READ reach the record store, whose files land in /.a4/store and not in /.rytm/store, and the answers pass
// its output filter; the engine's other messages do not; the step clock takes its 6 tracks (a tick on track 6 reaches no
// listener, counted off_track) where the Rytm's takes 13; the Rytm's 35 gives the link back with the 13 tracks.
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "clock/clock.h"
#include "device/a4.h"
#include "device/device.h"
#include "generic/generic.h"
#include "hooks.h"
#include "kv/kv.h"
#include "kv/link.h"
#include "platform.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static std::vector<clock::Tick> g_ticks;
static int g_restarts = 0;
static void out_fn(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void tick_fn(const clock::Tick& t) { g_ticks.push_back(t); }
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

int main() {
  char tmpl[] = "/tmp/core-a4gen-XXXXXX";
  CHECK(mkdtemp(tmpl));
  const std::string card = tmpl;
  host_set_card(tmpl);
  hooks.to_rytm = out_fn;
  hooks.device_restart = [] { ++g_restarts; };
  mkdir((card + "/.rytm").c_str(), 0755);

  // the profile: built in, six tracks, its own folder, no audio row on this line
  CHECK(device::kA4.id == device::kIdA4 && device::kA4.tracks == 6 && device::kA4.voices == 4);
  CHECK(!strcmp(device::kA4.state_dir, "/.a4") && device::kA4.audio == nullptr);
  CHECK(!device::add(&device::kA4));   // its id is taken: it is in the engine's list already
  CHECK(device::by_usb(device::kA4Vid, device::kA4Pid) == nullptr);   // no audio row: the Pi's driver leaves it alone

  // a Rytm boot (no /.rytm/device): its hello is answered kRestart and its id kept for the next boot
  CHECK(gen::begin());
  CHECK(kv::link::attach_generic());
  CHECK(clock::on_tick(tick_fn));
  CHECK(!device::foreign() && clock::tracks() == clock::kTracks);
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

  // the engine's other messages do not reach it; the hello's answer is never filtered (above)
  uint32_t d0 = device::dropped();
  const uint8_t juno[8] = {0xF0, 0x7D, 0x52, 0x54, 0x15, 0, 0, 0xF7};
  to_rytm(juno, 8);
  CHECK(g_out.empty() && device::dropped() == d0 + 1);

  // the step clock: its 6 tracks. One 2C with ticks on tracks 5 and 6: only 5 reaches the listener
  CHECK(clock::tracks() == 6);
  uint32_t off0 = clock::stats().off_track;
  g_ticks.clear();
  in({clock::kTickCmd, 1, 0, 0, 0, 7, 2, 5, 3, 16, 1, 6, 4, 16, 1});
  CHECK(g_ticks.size() == 1 && g_ticks[0].track == 5 && g_ticks[0].step == 3 && g_ticks[0].block == 7);
  CHECK(clock::stats().off_track == off0 + 1);
  int st, ln, sp;
  CHECK(clock::step_of(5, &st, &ln, &sp) && st == 3 && !clock::step_of(6, &st, &ln, &sp));
  // its 2D TRANSPORT and TEMPO pass its input filter
  in({clock::kStateCmd, 1, clock::kTransport, clock::kStop, 0, 0, 0, 8});
  CHECK(clock::transport() == clock::kStop);
  in({clock::kStateCmd, 1, clock::kTempo, 0, (120 * 120) >> 7 & 0x7F, (120 * 120) & 0x7F});
  CHECK(clock::bpm() == 120.0);

  // a Rytm on USB: its 35 gives the link back to the Rytm, after a restart into /.rytm (the box's folder is not the Rytm's)
  in({0x35, 1, 0, 100, 3});
  CHECK(device::leaving());
  // the Rytm's clock keeps 13 tracks: track 12 (the Storage phase's) reaches the listener there
  device::reset();
  CHECK(!device::foreign() && clock::tracks() == clock::kTracks);
  g_ticks.clear();
  in({clock::kTickCmd, 1, 0, 0, 0, 9, 1, 12, 0, 16, 1});
  CHECK(g_ticks.size() == 1 && g_ticks[0].track == 12);

  printf("test_a4_generic: PASS\n");
  return 0;
}

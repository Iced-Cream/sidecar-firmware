// core/tests/test_device.cpp - the device profiles and the hello (core/device/device.h) in the generic engine: the Rytm
// the default; an unknown hello or another protocol told so; a known box taken at its hello (its output filtered, its
// reports translated into the engine's 0x04 / 0x03 / 0x11 by its table, its other messages dropped); the Rytm's 35
// giving the link back; a box with a state folder of its own answered kRestart and the choice saved before one restart;
// the Digitakt's profile accepted by add().
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/device.h"
#include "device/audio.h"
#include "device/digitakt.h"
#include "fmt/fmt.h"
#include "generic/generic.h"
#include "hooks.h"
#include "platform.h"

using namespace core;

static int g_restarts = 0;
static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void send(const std::vector<uint8_t>& m) { gen::sysex(m.data(), (int)m.size()); }
static std::vector<uint8_t> sidecar(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xf7);
  return m;
}
static constexpr uint16_t kBoxId = 0x3F00;   // the range for private tests (docs/device-profiles.md)
// Two tracks, three parameters: P1 fixed 12, P2 from its parameter 2, P3 from 0, P4 fixed 33
static const device::Profile kBox = {
    kBoxId, "Test box", 0x1234, 0x0001,
    4, 2, 3,
    {0x7D, 'T', 'B'},
    0x61, 0x62, 0x63,
    0,
    0x00, 0,   // the voice byte's machine (0: the old engine's PLAITS)
    {-1, 2, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {12, 0, 0, 33, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100},
    {1u << 0x15},   // of the engine's messages it takes only 15
    nullptr};
static std::vector<uint8_t> box(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 'T', 'B'};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xf7);
  return m;
}
static std::vector<uint8_t> hello(int id, int proto = 1) {
  return sidecar({device::kCmdHello, 1, (uint8_t)(id >> 7), (uint8_t)(id & 0x7f), (uint8_t)proto, 'T', 'B'});
}

int main() {
  char tmpl[] = "/tmp/core-device-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  hooks.to_rytm = ::out;
  CHECK(gen::begin());

  // the default: the Rytm, nothing in between
  CHECK(!device::foreign() && device::active().id == device::kIdRytm && hooks.to_rytm == ::out);
  CHECK(device::rytm().tracks == 12 && device::rytm().usb_vid == 0x1935 && device::rytm().usb_pid == 0x0b48);
  CHECK(device::rytm().audio && device::rytm().audio->channels == 12);

  // a hello nobody knows, or of another protocol: told so, the Rytm stays
  send(hello(0x1234));
  CHECK(g_out.size() == 1 && g_out[0][4] == device::kCmdHello && g_out[0].size() == 10 && g_out[0][8] == device::kUnknown);
  CHECK(!device::foreign());
  CHECK(device::add(&kBox) && !device::add(&kBox));   // the id is taken the second time
  g_out.clear();
  send(hello(kBoxId, 9));
  CHECK(g_out.size() == 1 && g_out[0][8] == device::kBadProto && !device::foreign());

  // its hello: answered, and it is the box from here on
  g_out.clear();
  send(hello(kBoxId));
  CHECK(g_out.size() == 1 && g_out[0][6] == kBoxId >> 7 && g_out[0][7] == (kBoxId & 0x7f) && g_out[0][8] == device::kOk);
  CHECK(device::foreign() && device::active().id == kBoxId && hooks.to_rytm != ::out);

  // its trig through the table: the engine's 0x04 on voice 1, P1 fixed, P2 its c, P3 its a, P4 fixed
  {
    std::vector<uint8_t> t = box({0x61, 1, 48, 127, 10, 0, 20, 64, 30, 0});
    const uint8_t* d = t.data();
    int len = (int)t.size();
    uint8_t m[device::kMaxMessage];
    CHECK(device::translate(device::g_link, d, len, m) && d == m && len == 43);
    CHECK(m[4] == 0x04 && m[5] == 1 && m[6] == 48 && m[7] == 127);
    CHECK(m[8] == 12 && m[10] == 30 && m[11] == 0 && m[12] == 10 && m[14] == 33);
    std::vector<uint8_t> off = box({0x63, 1});
    d = off.data(), len = (int)off.size();
    CHECK(device::translate(device::g_link, d, len, m) && len == 7 && m[4] == 0x11 && m[5] == 1);
    std::vector<uint8_t> past = box({0x61, 2, 48, 100, 1, 0, 2, 0, 3, 0});   // a track past its voices
    d = past.data(), len = (int)past.size();
    uint32_t d0 = device::dropped();
    CHECK(!device::translate(device::g_link, d, len, m) && device::dropped() == d0 + 1);
  }
  // the engine's messages to it: only what its profile takes
  g_out.clear();
  const uint8_t juno[8] = {0xf0, 0x7d, 0x52, 0x54, 0x15, 0, 0, 0xf7}, other[8] = {0xf0, 0x7d, 0x52, 0x54, 0x14, 1, 0, 0xf7};
  uint32_t d1 = device::dropped();
  to_rytm(juno, 8);
  to_rytm(other, 8);
  CHECK(g_out.size() == 1 && g_out[0][4] == 0x15 && device::dropped() == d1 + 1);
  // a Rytm plugged in: its hello 35 gives it back; the output is the platform's again
  send(sidecar({0x35, 1, 0, 100, 3}));
  CHECK(!device::foreign() && hooks.to_rytm == ::out && device::active().id == device::kIdRytm);

  // reset() from a box
  send(hello(kBoxId));
  CHECK(device::foreign());
  device::reset();
  CHECK(!device::foreign() && hooks.to_rytm == ::out);

  // a box with a state folder of its own: answered "restart", the choice kept in /.rytm, one restart after the saves
  static const device::Profile kBox2 = [] { device::Profile p = kBox; p.id = kBoxId + 1; p.state_dir = "/.boxtest"; return p; }();
  CHECK(device::add(&kBox2));
  mkdir((std::string(tmpl) + "/.rytm").c_str(), 0755);
  g_out.clear();
  send(hello(kBoxId + 1));   // no restart hook on this board: told it has no such profile, nothing changes
  CHECK(g_out.size() == 1 && g_out[0][8] == device::kUnknown && !device::foreign());
  hooks.device_restart = [] { ++g_restarts; };
  g_out.clear();
  send(hello(kBoxId + 1));
  CHECK(g_out.size() == 1 && g_out[0][8] == device::kRestart && !device::foreign() && !device::g_link.state);
  for (int k = 0; k < 200 && !g_restarts; ++k) gen::idle_save();
  CHECK(g_restarts == 1);
  {
    File f;
    char b[16] = {0};
    CHECK(f.open("/.rytm/device", kRead) && f.read(b, sizeof b - 1) > 0);
    CHECK(atoi(b) == kBoxId + 1);
  }

  // the Digitakt's profile (docs/device-profiles.md): a table add() accepts (not registered by the engine itself)
  CHECK(device::add(&device::kDigitakt));
  CHECK(device::kDigitakt.audio && device::kDigitakt.audio->ret == out::kStereoMix);

  printf("test_device: ok\n");
  return 0;
}

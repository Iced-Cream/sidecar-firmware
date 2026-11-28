// pi/engine/tests/test_a4_tcp.cpp - an Analog Four MKII's mod over link=tcp (tcp_link.cpp: the emulator's oblink.rs 'M'
// stream), on the host: what the TCP link does with the emulator's bytes, without its socket. EmuParser takes the stream
// (N requests and R audio between the M pieces), MidiIn makes whole SysEx of the M bytes as g_MidiIn's consumer does, the
// generic core answers, and each answer goes back as one 'M' (emu_midi_head). Checked: the A4's hello, split across two
// M messages with an N and an R between them, is answered kRestart and its id is saved; on the next boot (its folder
// /.a4) its hello is kOk and its 51 STORE HELLO is answered through the A4's output filter, as one whole M.
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>

#include "check.h"
#include "device/a4.h"
#include "device/device.h"
#include "emu_proto.h"
#include "generic/generic.h"
#include "hooks.h"
#include "kv/kv.h"
#include "kv/link.h"
#include "midi_in.h"
#include "platform.h"

using namespace core;

static std::vector<uint8_t> g_wire;   // what the TCP link sends the emulator: 'M' + len + bytes per answer
static int g_restarts = 0;
static void out_fn(const uint8_t* m, int n) {
  uint8_t h[3];
  int k = emu_midi_head(h, (uint16_t)n);
  g_wire.insert(g_wire.end(), h, h + k);
  g_wire.insert(g_wire.end(), m, m + n);
}
static void sx(const uint8_t* d, int n) { gen::sysex(d, n); }
static void ch(uint8_t, uint8_t, uint8_t) {}
static MidiIn g_in(sx, ch);

struct Sink : EmuParser::Sink {
  uint32_t wanted = 0;
  void Want(uint32_t n) override { wanted += n; }
  void Midi(const uint8_t* d, int n) override { for (int i = 0; i < n; ++i) g_in.Feed(d[i]); }
  void Hello(uint8_t) override {}
};

static void m_msg(std::vector<uint8_t>& s, const std::vector<uint8_t>& b) {
  s.push_back('M'), s.push_back((uint8_t)(b.size() & 0xFF)), s.push_back((uint8_t)(b.size() >> 8));
  s.insert(s.end(), b.begin(), b.end());
}
static std::vector<uint8_t> sysex(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
static std::vector<uint8_t> hello_a4() {
  return sysex({device::kCmdHello, 1, (uint8_t)(device::kIdA4 >> 7), (uint8_t)(device::kIdA4 & 0x7F), device::kProto, 'A', '4'});
}
// the emulator's stream for one SysEx: its first bytes in one M, an N, an R of one frame, the rest in a second M
static std::vector<uint8_t> split(const std::vector<uint8_t>& m, size_t at) {
  std::vector<uint8_t> s;
  m_msg(s, std::vector<uint8_t>(m.begin(), m.begin() + at));
  s.insert(s.end(), {'N', 32, 0, 0, 0});
  s.insert(s.end(), {'R', 1, 0, 0, 0});
  s.insert(s.end(), 48, 0);
  m_msg(s, std::vector<uint8_t>(m.begin() + at, m.end()));
  return s;
}
// the answer on the wire: exactly one 'M' holding one whole SysEx with this command; its bytes
static bool one_answer(uint8_t cmd, std::vector<uint8_t>* body) {
  if (g_wire.size() < 3 || g_wire[0] != 'M') return false;
  size_t n = g_wire[1] | g_wire[2] << 8;
  if (g_wire.size() != 3 + n || n < 6 || g_wire[3] != 0xF0 || g_wire[2 + n] != 0xF7 || g_wire[7] != cmd) return false;
  body->assign(g_wire.begin() + 3, g_wire.end());
  return true;
}

int main() {
  char tmpl[] = "/tmp/pi-a4tcp-XXXXXX";
  CHECK(mkdtemp(tmpl));
  const std::string card = tmpl;
  host_set_card(tmpl);
  mkdir((card + "/.rytm").c_str(), 0755);
  hooks.to_rytm = out_fn;
  hooks.device_restart = [] { ++g_restarts; };
  Sink sink;
  EmuParser p(&sink);

  // a Rytm boot: the A4's hello in two M pieces, an N and an R between them
  CHECK(gen::begin() && kv::link::attach_generic());
  std::vector<uint8_t> s = split(hello_a4(), 5);
  CHECK(p.Feed(s.data(), (int)s.size()) && sink.wanted == 32 && p.Skipped() > 0);
  std::vector<uint8_t> a;
  CHECK(one_answer(device::kCmdHello, &a) && a[8] == device::kRestart);
  for (int k = 0; k < 200 && !g_restarts; ++k) gen::idle_save();
  CHECK(g_restarts == 1);

  // the next boot: /.a4; its hello kOk, its STORE HELLO answered through its filter
  device::reset();
  g_wire.clear();
  CHECK(gen::begin() && device::foreign() && device::active().id == device::kIdA4);
  s = split(hello_a4(), 9);
  CHECK(p.Feed(s.data(), (int)s.size()));
  CHECK(one_answer(device::kCmdHello, &a) && a[8] == device::kOk);
  g_wire.clear();
  s = split(sysex({kv::kCmd, 1, kv::kOpHello, 0, 0, 0, 0, 0x2A}), 3);
  CHECK(p.Feed(s.data(), (int)s.size()));
  for (int i = 0; i < 64 && kv::link::host_step(8); ++i) {}
  CHECK(one_answer(kv::kCmd, &a) && a[6] == kv::kOpHelloAck);
  CHECK(g_in.SysExDropped() == 0 && !p.Bad());

  printf("test_a4_tcp: PASS\n");
  return 0;
}

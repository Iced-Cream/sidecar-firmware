// core/tests/test_store_generic.cpp - 51 STORE in the stripped engine (CORE_GENERIC=1): kv::link::attach_generic()
// registers with core/generic; a HELLO / WRITE / READ through gen::sysex reach the record store on the card (a temp
// folder) and answer through hooks.to_rytm; a lock set fed by a 2C tick reaches a core/proc slot; the Rytm's hello
// (35) forgets the lock sets.
#include <cstring>
#include <vector>

#include "check.h"
#include "generic/generic.h"
#include "hooks.h"
#include "kv/kv.h"
#include "kv/link.h"
#include "locks/locks.h"
#include "platform.h"
#include "proc/proc.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void out_fn(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void in(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  gen::sysex(m.data(), (int)m.size());
  for (int i = 0; i < 64 && kv::link::host_step(8); ++i) {}
}
struct Probe : proc::Proc {
  int v[proc::kParams];
  Probe() { for (int& x : v) x = -1; }
  void process(const proc::Block&) override {}
  void set_param(int p, int value) override { v[p] = value; }
  const char* name() const override { return "probe"; }
};

int main() {
  char tmpl[] = "/tmp/core-gstore-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  hooks.to_rytm = out_fn;
  CHECK(kv::link::attach_generic());
  // HELLO, WRITE (3 bytes), READ back
  in({0x51, 1, kv::kOpHello, 0, 0, 0, 0, 42});
  CHECK(g_out.size() == 1 && g_out[0][6] == kv::kOpHelloAck);
  g_out.clear();
  uint8_t data[3] = {9, 8, 0xF0}, p[8];
  int pn = kv::pack7(data, 3, p);
  std::vector<uint8_t> w = {0x51, 1, kv::kOpWrite, 5, 3, 0, 3, 0, 0};
  w.insert(w.end(), p, p + pn);
  in(w);
  CHECK(g_out.size() == 1 && g_out[0][6] == kv::kOpWritten && g_out[0][11] == 1);
  g_out.clear();
  in({0x51, 1, kv::kOpRead, 6, 3, 0, 0, 1});
  uint8_t back[8];
  CHECK(g_out.size() == 1 && g_out[0][6] == kv::kOpData && kv::unpack7(&g_out[0][16], (int)g_out[0].size() - 17, back) == 3 &&
        !memcmp(back, data, 3));
  // a lock set on the FX track into slot 0's params 0..1; a 2C tick (5 bytes, trig) plays the lock
  Probe pr;
  CHECK(proc::register_slot(0, &pr));
  in({0x51, 1, locks::kSet, 0, 12, 0, 0, 2});
  in({0x51, 1, locks::kBase, 0, 0, 2, 0, 1, 0, 2});
  in({0x51, 1, locks::kLock, 0, 7, 1, 0, 99});
  in({0x2C, 1, 0, 0, 0, 9, 1, 12, 7, 16, 2, 1});
  static float buf[proc::kChannels][32];
  proc::Block b;
  for (int c = 0; c < proc::kChannels; ++c) { b.in[c] = buf[c]; b.out[c] = buf[c]; }
  b.frames = 32;
  proc::process(b);
  CHECK(pr.v[0] == 1 && pr.v[1] == 99);
  // the Rytm's hello: the lock set goes
  CHECK(locks::declared(0));
  in({0x35, 1, 0, 100 & 0x7F, 1});
  CHECK(!locks::declared(0));
  printf("test_store_generic: PASS\n");
  return 0;
}

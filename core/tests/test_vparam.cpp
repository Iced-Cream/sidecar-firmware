// core/tests/test_vparam.cpp - the touchscreen's VOICE knobs (core/ui/remote.h vparam_command: "vparam=T:K:V", the Rytm's P(K+1) of
// track T written as a knob turn, 48) against a fake Rytm that answers: done, refused (status 1: sent again every 100 ms, at most
// 10 times within a second, then refused; there is no "busy"), locked on the steps held, no such parameter; no answer (an older
// build, a full queue) leaves nothing followed; only the final line of a drag is followed (an answer to an earlier write of the
// knob does not clear it; the live lines end in :0); a track that is not a Sidecar machine sends nothing and is flagged; a bad
// line is the help; a knob's next write forgets its failure.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "core.h"
#include "hooks.h"
#include "machines.h"
#include "ui/remote.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_sent;
static void out(const uint8_t* m, int n) { g_sent.emplace_back(m, m + n); }
static int n48(int t, int k) {
  int n = 0;
  for (auto& m : g_sent) n += m.size() >= 10 && m[4] == 0x48 && m[6] == t && m[7] == k;
  return n;
}
static int last48(int t, int k) {   // the value of the last 48 for (t, k), -1 none
  int v = -1;
  for (auto& m : g_sent)
    if (m.size() >= 10 && m[4] == 0x48 && m[6] == t && m[7] == k) v = m[8];
  return v;
}
static int total48() {
  int n = 0;
  for (auto& m : g_sent) n += m.size() >= 10 && m[4] == 0x48;
  return n;
}
static void answer(int t, int k, int status) {
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x48, 1, (uint8_t)t, (uint8_t)k, (uint8_t)status, 0xF7};
  engine_sysex(m, sizeof m);
}
static int g_served[12][16];   // the writes the fake Rytm has answered, per knob: it answers each write once
static void serve(int t, int k, int status) {
  for (int n = n48(t, k); g_served[t][k] < n; ++g_served[t][k]) answer(t, k, status);
}
static uint64_t g_us = 1000000;
static void ms(int n) {   // the control loop's passes, 1 ms each
  for (int i = 0; i < n; ++i) {
    g_us += 1000;
    host_fake_time(true, g_us);
    ui::remote_tick(now_ms());
  }
}
static char g_o[300];
static bool cmd(const std::string& c) {
  g_o[0] = 0;
  return ui::vparam_command(c.c_str(), g_o, sizeof g_o);
}
static bool failed(int t, int k, int* kind = nullptr) { return ui::remote_vparam_failed(t, kind) >> k & 1; }

int main() {
  char tmpl[] = "/tmp/test_vparam.XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string c = tmpl;
  host_set_card(tmpl);
  CHECK(mkdir((c + "/NAM").c_str(), 0755) == 0);
  CHECK(mkdir((c + "/Samples").c_str(), 0755) == 0);
  host_fake_time(true, g_us);
  hooks.to_rytm = out;
  CHECK(engine_begin());
  // tracks 1..3 (0..2): PLAITS on voice 0, not a Sidecar machine, JUNO on voice 1
  const uint8_t m33[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 3, 0, (uint8_t)(kPlaits | 0), 0, 0x7F, 0, (uint8_t)(kJuno | 1), 0xF7};
  engine_sysex(m33, sizeof m33);
  CHECK(ui::remote_track_machine(0) == kPlaits && ui::remote_track_machine(1) == 0x7F);
  ms(5);
  g_sent.clear();

  // a write goes out as a knob turn (48 1 <track> <param> <value>), answered done: followed no further
  CHECK(cmd("vparam=0:2:90") && strstr(g_o, "P3 = 90"));
  CHECK(n48(0, 2) == 1 && last48(0, 2) == 90);
  CHECK(g_sent.back().size() == 10 && g_sent.back()[5] == 1);
  answer(0, 2, 0);
  ms(100);
  CHECK(n48(0, 2) == 1 && !failed(0, 2) && ui::remote_vparam_gen() == 0);

  // refused (1) for 250 ms, then done: sent again every 100 ms meanwhile, no failure
  CHECK(cmd("vparam=0:3:50"));
  for (int i = 0; i < 250; ++i) {
    serve(0, 3, 1);   // (the fake Rytm: refused)
    ms(1);
  }
  CHECK(n48(0, 3) >= 2 && n48(0, 3) <= 4 && last48(0, 3) == 50);
  answer(0, 3, 0);
  ms(100);
  const int sent3 = n48(0, 3);
  ms(300);
  CHECK(n48(0, 3) == sent3 && !failed(0, 3) && ui::remote_vparam_gen() == 0);

  // a new value while it is refused: the resends carry it
  CHECK(cmd("vparam=0:8:10"));
  for (int i = 0; i < 150; ++i) serve(0, 8, 1), ms(1);
  CHECK(cmd("vparam=0:8:20"));
  for (int i = 0; i < 250; ++i) serve(0, 8, 1), ms(1);
  CHECK(last48(0, 8) == 20 && n48(0, 8) >= 3);
  answer(0, 8, 0);
  ms(50);

  // refused for good: at most 10 resends within a second (a new drag line does not start the window again), then refused (kind 2)
  CHECK(cmd("vparam=0:4:60"));
  for (int i = 0; i < 3000; ++i) {
    serve(0, 4, 1), ms(1);
    if (i % 50 == 0 && i < 800) cmd("vparam=0:4:61:0");   // (live lines of a drag going on: they are not followed and do not reset it)
  }
  int kind = 0;
  CHECK(failed(0, 4, &kind) && kind == 2 && ui::remote_vparam_gen() == 1);
  CHECK(n48(0, 4) >= 5 && n48(0, 4) <= 12 + 16);   // (the live lines are writes too: 16 of them)
  const int sent4 = n48(0, 4);
  ms(300);
  CHECK(n48(0, 4) == sent4);

  // only the final line is followed: three writes of a knob (two live), the first two answered done, the last locked. The first
  // answer must not end the follow; a live write's own answer (here locked) is no failure
  CHECK(cmd("vparam=0:9:10:0") && cmd("vparam=0:9:20:0") && cmd("vparam=0:9:30"));
  answer(0, 9, 0);
  ms(5);
  answer(0, 9, 0);
  ms(5);
  CHECK(!failed(0, 9));
  answer(0, 9, 4);
  ms(5);
  CHECK(failed(0, 9, &kind) && kind == 1 && ui::remote_vparam_gen() == 2);
  CHECK(cmd("vparam=0:10:5:0") && n48(0, 10) == 1);
  answer(0, 10, 4);
  ms(5);
  CHECK(!failed(0, 10) && ui::remote_vparam_gen() == 2);   // (a live line alone: nothing followed)

  // locked on the steps held (4): kept for the page as locked
  CHECK(cmd("vparam=0:5:70"));
  answer(0, 5, 4);
  ms(5);
  CHECK(failed(0, 5, &kind) && kind == 1 && ui::remote_vparam_gen() == 3 && failed(0, 4));
  // refused (2: no such parameter on its machine)
  CHECK(cmd("vparam=0:6:70"));
  answer(0, 6, 2);
  ms(5);
  CHECK(failed(0, 6, &kind) && kind == 2 && ui::remote_vparam_gen() == 4);
  // the knob's next write forgets its failure, the others stay
  CHECK(cmd("vparam=0:5:71"));
  CHECK(!failed(0, 5) && failed(0, 4) && failed(0, 6));
  answer(0, 5, 0);
  ms(5);
  CHECK(!failed(0, 5) && ui::remote_vparam_gen() == 4);

  // no answer (an older Rytm build): nothing sent again, nothing failed
  CHECK(cmd("vparam=0:7:5"));
  ms(3000);
  CHECK(n48(0, 7) == 1 && !failed(0, 7));

  // a track that is not a Sidecar machine: nothing sent, flagged refused, said so
  int before = total48();
  CHECK(cmd("vparam=1:0:5") && strstr(g_o, "not a Sidecar machine"));
  CHECK(total48() == before && failed(1, 0, &kind) && kind == 2);
  // a track nothing has named (a 33 not seen for it)
  CHECK(cmd("vparam=11:0:5") && total48() == before && failed(11, 0));

  // bad lines: the help, nothing sent, nothing flagged
  const unsigned gen = ui::remote_vparam_gen();
  static const char* const kBad[] = {"vparam=12:0:5", "vparam=0:16:5", "vparam=0:1:128", "vparam=0:1", "vparam=0:1:x", "vparam=-1:1:5",
                                     "vparam=0:1:5:6", "vparam=0:1:5:1", "vparam=0:1:5:00", "vparam=0:1:5:0:0", "vparam=:1:5", "vparam=0::5", "vparam=0:1:12345", "vparam"};
  for (const char* b : kBad) CHECK(cmd(b) && strstr(g_o, "vparam=T:K:V"));
  CHECK(total48() == before && ui::remote_vparam_gen() == gen);
  CHECK(!cmd("vparamx") && !cmd("ins=V1:A:p1:5") && !cmd("vparams=0:1:5"));
  // the range's ends
  CHECK(cmd("vparam=0:0:0") && last48(0, 0) == 0 && cmd("vparam=0:15:127") && last48(0, 15) == 127);
  answer(0, 0, 0), answer(0, 15, 0);
  ms(10);
  CHECK(!failed(0, 0) && !failed(0, 15));

  // a reset drops what was followed and the failures
  ui::remote_reset();
  CHECK(ui::remote_vparam_failed(0, nullptr) == 0 && ui::remote_vparam_failed(1, nullptr) == 0);
  host_fake_time(false, 0);
  printf("test_vparam: PASS\n");
  return 0;
}

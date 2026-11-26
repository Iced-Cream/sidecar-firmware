// core/tests/test_touch_lines.cpp - the console lines the touchscreen's drags and double taps send (pi/ui: INSERT FX, MASTER
// FX; docs/INSERT-FX.md, docs/MASTER-FX.md), through the parsers that an encoder turn's lines go through. For each line:
// the value lands in range (0..127, a value past it clamped, a negative one refused with the help and nothing set), and the
// edit is told to the modulation layer once as the encoder's turn tells it (fx::edited with the target's tid: a manual edit
// wins over a scene's morph, spec-050 2.5). The lines: ins=Vn:S:pK:V, ins=Vn:send:B:V, out=Vn:lev|pan|wid:V, mfx=S:pK:V,
// mfx=S:mix:V, mfx=lev:V (the touch controls' default on a double tap goes through the same lines).
#include <cstdio>
#include <cstring>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "core.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "mod/target.h"

using namespace core;

static int g_n;
static uint16_t g_tid[8];
static void rec_edit(uint16_t tid, float) {
  if (g_n < 8) g_tid[g_n] = tid;
  ++g_n;
}
// run a line: its answer in o; the edits it told
static bool run(const char* line, char* o, int cap) {
  g_n = 0;
  bool r = ins::command(line, o, cap) || master::command(line, o, cap) || out::command(line, o, cap);
  return r;
}
static bool told_once(uint16_t tid) { return g_n == 1 && g_tid[0] == tid; }

int main() {
  char tmpl[] = "/tmp/test_touch_lines.XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string c = tmpl;
  host_set_card(tmpl);
  CHECK(mkdir((c + "/NAM").c_str(), 0755) == 0);
  CHECK(mkdir((c + "/Samples").c_str(), 0755) == 0);
  host_fake_time(true, 5000000);
  CHECK(engine_begin());
  ins::reset();
  out::reset();
  auto* was = fx::manual_edit;
  fx::manual_edit = rec_edit;
  char o[400];

  // INSERT FX: a voice's slot A / B P1..P4 (the dials)
  ins::set_type(2, 0, ins::kFilter), ins::set_type(2, 1, ins::kDelay);
  for (int s = 0; s < 2; ++s)
    for (int k = 0; k < 4; ++k) {
      char l[40];
      snprintf(l, sizeof l, "ins=V3:%c:p%d:%d", 'A' + s, k + 1, 10 + 30 * k + s);
      CHECK(run(l, o, sizeof o) && ins::param(2, s, k) == 10 + 30 * k + s);
      CHECK(told_once(mod::tid(mod::kIns, 2, s * 4 + k)));
    }
  CHECK(run("ins=V3:A:p1:127", o, sizeof o) && ins::param(2, 0, 0) == 127 && told_once(mod::tid(mod::kIns, 2, 0)));
  CHECK(run("ins=V3:A:p1:0", o, sizeof o) && ins::param(2, 0, 0) == 0 && told_once(mod::tid(mod::kIns, 2, 0)));
  CHECK(run("ins=V3:A:p2:999", o, sizeof o) && ins::param(2, 0, 1) == 127 && told_once(mod::tid(mod::kIns, 2, 1)));   // clamped
  const int keep = ins::param(2, 0, 2);
  CHECK(run("ins=V3:A:p3:-5", o, sizeof o) && strstr(o, "ins=") && ins::param(2, 0, 2) == keep && g_n == 0);        // refused: the help
  CHECK(run("ins=V8:B:p4:64", o, sizeof o) && ins::param(7, 1, 3) == 64 && told_once(mod::tid(mod::kIns, 7, 7)));  // (the last voice)

  // the sends to the three buses
  for (int b = 0; b < 3; ++b) {
    char l[40];
    snprintf(l, sizeof l, "ins=V3:send:%d:%d", b + 1, 20 + 40 * b);
    CHECK(run(l, o, sizeof o) && ins::send_knob(2, b) == 20 + 40 * b && told_once(mod::tid(mod::kIsnd, 2, b)));
  }
  CHECK(run("ins=V3:send:1:0", o, sizeof o) && ins::send_knob(2, 0) == 0 && told_once(mod::tid(mod::kIsnd, 2, 0)));   // the double tap's default
  CHECK(run("ins=V3:send:2:500", o, sizeof o) && ins::send_knob(2, 1) == 127 && told_once(mod::tid(mod::kIsnd, 2, 1)));
  CHECK(run("ins=V3:send:3:-1", o, sizeof o) && strstr(o, "ins=") && ins::send_knob(2, 2) == 100 && g_n == 0);
  CHECK(run("ins=V3:send:4:5", o, sizeof o) && strstr(o, "ins=") && g_n == 0);   // no bus 4

  // OUT: LEV / PAN / WID of a voice
  static const char* const kK[3] = {"lev", "pan", "wid"};
  for (int k = 0; k < 3; ++k) {
    char l[40];
    snprintf(l, sizeof l, "out=V3:%s:%d", kK[k], 30 + 20 * k);
    CHECK(run(l, o, sizeof o) && out::param(2, k) == 30 + 20 * k && told_once(mod::tid(mod::kOut, 2, k)));
    snprintf(l, sizeof l, "out=V3:%s:127", kK[k]);
    CHECK(run(l, o, sizeof o) && out::param(2, k) == 127 && told_once(mod::tid(mod::kOut, 2, k)));
    snprintf(l, sizeof l, "out=V3:%s:128", kK[k]);   // past the range: the help, the value as it was
    CHECK(run(l, o, sizeof o) && strstr(o, "out=Vn") && out::param(2, k) == 127 && g_n == 0);
  }
  CHECK(run("out=V3:lev:100", o, sizeof o) && out::param(2, 0) == 100 && told_once(mod::tid(mod::kOut, 2, 0)));   // the defaults
  CHECK(run("out=V3:pan:64", o, sizeof o) && out::param(2, 1) == 64 && told_once(mod::tid(mod::kOut, 2, 1)));
  CHECK(run("out=V3:wid:64", o, sizeof o) && out::param(2, 2) == 64 && told_once(mod::tid(mod::kOut, 2, 2)));

  // MASTER FX: the slots' P1..P4, MIX of a send TYPE, LEV
  for (int i = 0; i < master::kSlots; ++i) {
    for (int k = 0; k < 4; ++k) {
      char l[40];
      snprintf(l, sizeof l, "mfx=%d:p%d:%d", i + 1, k + 1, 5 + 20 * k + i);
      CHECK(run(l, o, sizeof o) && ins::param(master::chain(i), master::slot(i), k) == 5 + 20 * k + i);
      CHECK(told_once(mod::tid(mod::kIns, master::chain(i), master::slot(i) * 4 + k)));
    }
  }
  CHECK(run("mfx=2:p3:300", o, sizeof o) && ins::param(master::chain(1), master::slot(1), 2) == 127 &&
        told_once(mod::tid(mod::kIns, master::chain(1), master::slot(1) * 4 + 2)));
  CHECK(run("mfx=2:p3:-1", o, sizeof o) && strstr(o, "mfx=") && ins::param(master::chain(1), master::slot(1), 2) == 127 && g_n == 0);
  for (int i = 0; i < master::kSlots; ++i) {
    char l[40];
    snprintf(l, sizeof l, "mfx=%d:mix:%d", i + 1, 50 + i);
    CHECK(run(l, o, sizeof o) && master::control.mix[i] == 50 + i && told_once(mod::tid(mod::kMfx, 0, 1 + i)));
  }
  CHECK(run("mfx=3:mix:300", o, sizeof o) && master::control.mix[2] == 127 && told_once(mod::tid(mod::kMfx, 0, 3)));
  CHECK(run("mfx=3:mix:40", o, sizeof o) && master::control.mix[2] == master::kMixDef && told_once(mod::tid(mod::kMfx, 0, 3)));   // the default
  CHECK(run("mfx=lev:90", o, sizeof o) && master::control.lev == 90 && told_once(mod::tid(mod::kMfx, 0, 0)));
  CHECK(run("mfx=lev:500", o, sizeof o) && master::control.lev == 127 && told_once(mod::tid(mod::kMfx, 0, 0)));
  CHECK(run("mfx=lev:100", o, sizeof o) && master::control.lev == master::kLevUnity && told_once(mod::tid(mod::kMfx, 0, 0)));   // the default
  CHECK(run("mfx=lev:-3", o, sizeof o) && master::control.lev == master::kLevUnity && g_n == 0);

  fx::manual_edit = was;
  host_fake_time(false, 0);
  printf("test_touch_lines: PASS\n");
  return 0;
}

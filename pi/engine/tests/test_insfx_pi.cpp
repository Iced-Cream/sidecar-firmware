// pi/engine/tests/test_insfx_pi.cpp - the display's INSERT FX and MASTER FX views (insfx_pi.cpp) for the touch controls: each
// dial, send, OUT value and master bar carries the BASE a drag starts from (the knob as set, not the played value with its
// LFO or lock offset) and the default a double tap sends (the TYPE's, 0 for a send, 100 / 64 / 64 for OUT, unity LEV, MIX's
// default), and the hash follows them. make -C pi/engine/tests (make -C core engines lib first).
#include <cstdio>
#include <cstring>

#include "../insfx_pi.h"
#include "check.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "fx/out.h"

using namespace core;

int main() {
  ins::reset();
  out::reset();
  sidecar_insfx_view_t v;
  sidecar_mfx_view_t m;
  // voice 3: FILTER in slot A, an LFO-like offset on its P1: played != as set
  ins::set_type(2, 0, ins::kFilter);
  ins::set_param(2, 0, 0, 80);
  ins::mod_offset(2, 0, 0, 20);
  CHECK(ins::control.play[2][0][0] == 100 && ins::param(2, 0, 0) == 80);
  insfx_pi::Fill(&v);
  const sidecar_insfx_slot_t& a = v.slot[2][0];
  CHECK(a.type == ins::kFilter && a.v[0] == 100 && a.base[0] == 80);   // the drag starts at 80, the dial shows 100
  for (int k = 0; k < 4; ++k) CHECK(a.def[k] == ins::labels(ins::kFilter).def[k] && a.base[k] == ins::param(2, 0, k));
  unsigned g0 = v.gen;
  ins::set_param(2, 0, 0, 81);
  insfx_pi::Fill(&v);
  CHECK(v.gen != g0 && v.slot[2][0].base[0] == 81);   // the base is in the hash
  // sends as set; OUT as set and played, the defaults
  ins::set_send(2, 1, 70);
  out::set_route(2, out::kPost);
  out::set_param(2, 0, 90), out::set_param(2, 1, 50), out::set_param(2, 2, 70);
  insfx_pi::Fill(&v);
  CHECK(v.send[2][1] == 70);
  for (int k = 0; k < 3; ++k) CHECK(v.out_base[2][k] == out::param(2, k));
  CHECK(v.out_base[2][0] == 90 && v.out_base[2][1] == 50 && v.out_base[2][2] == 70);
  CHECK(v.out_def[0] == 100 && v.out_def[1] == 64 && v.out_def[2] == 64);
  CHECK(v.out_def[0] == out::control.lev[0] && v.out_def[1] == out::control.pan[0] && v.out_def[2] == out::control.wid[0]);   // (a fresh voice's)
  // the master: P1..P4's base and default, MIX, LEV
  master::set_lev(90);
  ins::set_type(master::chain(2), master::slot(2), ins::kMSpace);
  master::set_mix(2, 55);
  ins::set_param(master::chain(2), master::slot(2), 1, 33);
  insfx_pi::FillMaster(&m);
  CHECK(m.lev == 90 && m.lev_def == master::kLevUnity);
  const sidecar_mfx_slot_t& s = m.slot[2];
  CHECK(s.send && s.mix == 55 && s.mixdef == master::kMixDef);
  CHECK(s.base[1] == 33);
  for (int k = 0; k < 4; ++k) CHECK(s.def[k] == ins::labels(ins::kMSpace).def[k]);
  printf("test_insfx_pi: ok (the bases as set, the defaults, in the hash)\n");
  return 0;
}

// core/tests/nomorph/test_morph_off.cpp - 0.5.0 ships with the scene morph off (core/sidecar_features.h, SIDECAR_MORPH 0):
// MORPH is in no page list, no MENU row and no XY target list, UP + T14 does not open it, and the scene engine takes no
// edit and moves nothing. Built with -DSIDECAR_MORPH=0 (core/Makefile NOMORPH); the morph's own tests run with it on.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "core.h"
#include "engine.h"
#include "mod/mod.h"
#include "mod/scene.h"
#include "mod/target.h"
#include "mod/xypad.h"
#include "ui/extra.h"
#include "ui/page_keys.h"
#include "ui/remote.h"

using namespace core;

static_assert(!SIDECAR_MORPH, "build with -DSIDECAR_MORPH=0");

static uint64_t g_us = 1000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static void from_rytm(uint8_t cmd, uint8_t a, uint8_t b, uint8_t c) {
  const uint8_t m[10] = {0xF0, 0x7D, 0x52, 0x54, cmd, 1, a, b, c, 0xF7};
  engine_sysex(m, 10);
}
static void tick(int n = 4) {
  for (int i = 0; i < n; ++i) at(g_us + 30000), ui::remote_tick(now_ms());
}

int main() {
  at(g_us);
  engine_begin();
  from_rytm(0x35, 0, 6, 127);   // the Rytm's hello (its popups work)
  tick();
  // no page list has it
  for (int i = 0; i < ui::extra_pages(); ++i) CHECK(strcmp(ui::extra_page(i).title, "MORPH") != 0);
  CHECK(!ui::extra_for_box("MORPH"));
  // no MENU row, and no row of any other page that is MORPH
  for (int i = 0; i < ui::menu_rows(); ++i) CHECK(!ui::menu_row_page(i) || strcmp(ui::menu_row_page(i), "MORPH") != 0);
  // the device table has no MORPH row: UP's map and the key captions do not list it
  CHECK(!sc_page_of_name("MORPH"));
  CHECK(!sc_page_of_code(14));
  // UP + T14 (rc5b's mod still forwards it) opens nothing and says so
  ui::remote_show_page("MORPH");
  tick();
  CHECK(!ui::remote_page_title() || strcmp(ui::remote_page_title(), "MORPH") != 0);
  uint32_t shown = ui::remote_popups();
  from_rytm(0x49, 1, 14, 0);
  tick();
  CHECK(!ui::remote_page_title() || strcmp(ui::remote_page_title(), "MORPH") != 0);
  CHECK(ui::remote_popups() == shown + 1);
  CHECK(!strcmp(ui::remote_popup_text(), "MORPH: 0.5.2"));
  // the scene engine takes no edit and the fader stays; a console / web / learn / QPERF command does nothing
  mod::Tid t = 0;
  CHECK(mod::parse("V1:P1", &t));
  CHECK(!scene::lock(0, t, 8000));
  CHECK(scene::state().s[0].n == 0);
  scene::set_ab(0, 1);
  CHECK(scene::state().a == -1 && scene::state().b == -1);
  uint16_t f0 = scene::state().fader;
  scene::set_fader(9000, scene::kWeb);
  CHECK(scene::state().fader == f0);
  char o[200];
  CHECK(scene::command("scene=fader:5000", o, sizeof o));
  CHECK(strstr(o, "not in this version") != nullptr);
  CHECK(scene::state().fader == f0);
  // MORPH is not an XY target: refused by name and by the console; a real target still works
  CHECK(!xypad::command("xy=set:x:1:MORPH:50", o, sizeof o) || strstr(o, "cannot") != nullptr);
  CHECK(!xypad::set(0, 0, xypad::kMorph, 50));
  CHECK(xypad::cfg().s[0][0].t == 0);
  CHECK(xypad::set(0, 0, t, 50));
  printf("test_morph_off: ok\n");
  return 0;
}

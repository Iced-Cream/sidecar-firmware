// core/tests/test_screens_voice.cpp - 0.5.0 screens: SAMPLER's and GRAIN's voice pages drawn whole by ui::voice_screen (the UI
// system's GRAPH): the title bar (the touched knob named in full), the waveform kept, the mode as a boxed word, dials with
// marks and a fader, the touched label inverted, the bottom bar (idle on the SRC view: the pushes; touched: name, bar, value),
// the item budget. PNGs to $RUI_OUT (default build/rui_screens).
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include "check.h"
#include "core.h"
#include "core_port.h"
#include "engine.h"
#include "machines.h"
#include "screens_util.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;
using namespace core::ui;

static Canvas c;
static PageView view;

static const TextItem& slot(int i) { return c.txt[i]; }
static bool draw(const Page* p, ScrSource& s, int touched, bool src_view) {
  int drawn = 0;
  bool r = voice_screen(c, view, *p, s, "BD", 0, 0, touched, src_view, &drawn);
  CHECK(!r || drawn > 0);
  CHECK(scr_items(c) < 64);
  return r;
}

int main() {
  char tmpl[] = "/tmp/core_screens_voice_XXXXXX";
  CHECK(mkdtemp(tmpl));
  CHECK(system((std::string("mkdir -p '") + tmpl + "/Samples' '" + tmpl + "/.rytm'").c_str()) == 0);
  host_set_card(tmpl);
  setenv("CORE_SAMPLE_RAM_MB", "16", 1);   // the sample RAM: PLAY picks GRAIN
  CHECK(engine_begin() && card_ok && psram);
  ScrSource s;
  // SAMPLER: SMP 12, PLAY 35 (FLP), STRT 6, END 98, TUNE 64, FINE 64, LOOP 52, LEV 100
  s.p[10] = 12, s.p[11] = 35, s.p[12] = 6, s.p[13] = 98, s.p[8] = 64, s.p[9] = 64, s.p[14] = 52, s.p[15] = 100;
  {   // voice 0 plays SAMPLER (DSP SAMPLE's flag with SAMPLER's sound tag, the 41-byte 0x03)
    uint8_t r[41] = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)(kDspSample | 0)};
    for (int k = 0; k < 16; ++k) r[6 + 2 * k] = (uint8_t)s.p[k], r[7 + 2 * k] = 0;
    r[38] = (uint8_t)(kSmplTag >> 7), r[39] = (uint8_t)(kSmplTag & 0x7F), r[40] = 0xF7;
    engine_sysex(r, 41);
    CHECK(remote_voice_smpl(0));
  }
  const Page* pg = code_page_for_machine(kSmplMachine);
  CHECK(pg && !strcmp(pg->title, "SAMPLER"));
  int none = 0;
  CHECK(!voice_screen(c, view, *page_for_machine(kPlaits), s, "BD", 0, 0, -1, true, &none));   // another page: not drawn by it
  // idle on the SRC view: the pushes' key boxes; the title, the track, the labels as the Rytm's knobs, the mode in a box
  CHECK(draw(pg, s, -1, true));
  CHECK(!strcmp(slot(kSlotTitle).s, "SAMPLER") && !strcmp(slot(kSlotRight).s, "BD"));
  const char* lab[8] = {"SMP", "STRT", "END", "PLAY", "TUNE", "FINE", "LOOP", "LEV"};
  for (int e = 0; e < 8; ++e) {
    const TextItem& t = slot(kSlotLabels + e);
    CHECK(!strcmp(t.s, lab[e]) && t.x == (e & 3) * 32 + 16 && t.y == (e < 4 ? 26 : 48) && !(t.flags & kTextInvert));
  }
  CHECK(!strcmp(slot(kSlotCells).s, "FLP") && slot(kSlotCells).x == 112);   // the mode: a boxed word right of the waveform
  CHECK(c.get(98, 12) && c.get(125, 12) && c.get(98, 22) && !c.get(105, 20 - 3));   // the box's outline
  CHECK(!strcmp(slot(kSlotFoot).s, "PUSH A") && !strcmp(slot(kSlotFoot + 1).s, "LIST") && !strcmp(slot(kSlotFoot + 2).s, "PUSH B") &&
        !strcmp(slot(kSlotFoot + 3).s, "EDIT"));
  scr_shot("sampler_idle.png", c);
  // the Sidecar's own page has no pushes: no hint, encoder A's value as it was
  CHECK(draw(pg, s, -1, false));
  CHECK(!strcmp(slot(kSlotFoot).s, "SMP") && !strcmp(slot(kSlotFoot + 1).s, "12") && !scr_has(c, "PUSH A"));
  // turning END: its whole name, the label inverted, the bottom bar's name, bar and value
  CHECK(draw(pg, s, 2, true));
  CHECK(!strcmp(slot(kSlotTitle).s, "SAMPLE END") && (slot(kSlotLabels + 2).flags & kTextInvert) && !(slot(kSlotLabels + 1).flags & kTextInvert));
  CHECK(!strcmp(slot(kSlotFoot).s, "END") && !strcmp(slot(kSlotFoot + 1).s, "98") && slot(kSlotFoot + 1).x == 70 && (slot(kSlotFoot + 1).flags & kTextInvert));
  scr_shot("sampler_turning_end.png", c);
  // TUNE is bipolar: signed, a bar from the middle
  s.p[8] = 75;
  CHECK(draw(pg, s, 4, true));
  CHECK(!strcmp(slot(kSlotTitle).s, "TUNE") && !strcmp(slot(kSlotFoot).s, "TUNE") && !strcmp(slot(kSlotFoot + 1).s, "+11"));
  s.p[8] = 60;
  CHECK(draw(pg, s, 4, true) && !strcmp(slot(kSlotFoot + 1).s, "-4"));
  scr_shot("sampler_turning_tune.png", c);
  s.p[8] = 64;
  // PLAY shows the mode's name
  CHECK(draw(pg, s, 3, true) && !strcmp(slot(kSlotTitle).s, "PLAY MODE") && !strcmp(slot(kSlotFoot + 1).s, "FLP"));
  // GRAIN (PLAY in its zone): the same layout, POS SIZE DENS, the cloud under SIZE / DENS
  const Page* gp = page_for_voice(kSmplMachine, 0, 72);
  CHECK(gp && !strcmp(gp->title, "GRAIN"));
  s.p[11] = 72, s.p[12] = 60, s.p[13] = 70, s.p[14] = 50;
  CHECK(draw(gp, s, -1, true));
  CHECK(!strcmp(slot(kSlotTitle).s, "GRAIN") && !strcmp(slot(kSlotCells).s, "GRAIN") && !strcmp(slot(kSlotLabels + 1).s, "POS") &&
        !strcmp(slot(kSlotLabels + 6).s, "DENS") && !strcmp(slot(kSlotFoot).s, "PUSH A"));
  scr_shot("grain_idle.png", c);
  CHECK(draw(gp, s, 2, true) && !strcmp(slot(kSlotTitle).s, "GRAIN SIZE") && !strcmp(slot(kSlotFoot).s, "SIZE") && !strcmp(slot(kSlotFoot + 1).s, "70"));
  scr_shot("grain_turning_size.png", c);
  {   // on the Sidecar's own page the ring's dots and a tag narrow the title bar: a whole name that does not fit gives way to the label
    int dr = 0;
    s.p[13] = 70;
    CHECK(voice_screen(c, view, *gp, s, "V1", 3, 12, 1, false, &dr));
    CHECK(!strcmp(slot(kSlotTitle).s, "POS"));   // "GRAIN POSITION" (70 px) beside 12 dots and "V1"
    CHECK(voice_screen(c, view, *gp, s, "BD", 0, 0, 1, true, &dr) && !strcmp(slot(kSlotTitle).s, "GRAIN POSITION"));
    CHECK(draw(gp, s, 2, true));
  }
  // a second draw with nothing changed draws nothing; a changed knob or touch draws again
  int drawn = 0;
  CHECK(voice_screen(c, view, *gp, s, "BD", 0, 0, 2, true, &drawn) && drawn == 0);
  s.p[13] = 71;
  CHECK(voice_screen(c, view, *gp, s, "BD", 0, 0, 2, true, &drawn) && drawn > 0 && !strcmp(slot(kSlotFoot + 1).s, "71"));
  // the echo's slot: SAMPLER's value box
  CHECK(voice_screen_echo_slot(pg, 2) == kSlotFoot + 1 && voice_screen_echo_slot(page_for_machine(kPlaits), 2) == kSlotFoot);
  CHECK(voice_screen_echo_slot(pg, 3) == 0 && voice_screen_echo_slot(gp, 3) == 0);   // PLAY: the footer's word, no number echoed
  // the SRC view names a knob only for a parameter on one (review M1): SMP (P11) is A, P2 is on no knob of SAMPLER
  CHECK(enc_of_param(*pg, 10) == 0 && enc_of_param(*pg, 13) == 2 && enc_of_param(*pg, 1) == -1 && enc_of_param(*pg, -1) == -1);
  CHECK(system((std::string("rm -rf '") + tmpl + "'").c_str()) == 0);
  printf("test_screens_voice: PASS\n");
  return 0;
}

// core/tests/test_presetpick.cpp - a JUNO preset picked in the bank list (YES on another row: the Rytm's 48 writes PAT)
// plays the new patch whole, with its own CUT RES ENV ATK DEC REL, whatever the voice was doing at the pick.
//
// Hardware report (0.4.0 candidate): the screen showed the new preset at once, the sound stayed the old one until
// another track was selected and back. The knobs CUT..REL are absolute on JUNO (sim/juno/juno_voice.h Slider): the
// patch plays them, not its own sliders, until the Rytm's knobs are set to the patch's (15). Two gaps:
//   - 15 went out only for a 0x03 carrying the new PAT, and the Rytm sends 0x03s only while the voice sounds: a pick
//     on a silent voice reached the Pi first in the next trig's 0x04 (read as a p-lock: no 15), so the new patch
//     played the old patch's six for good;
//   - the Pi held the patch's six for 300 ms only, while the Rytm writes a 15 into the knobs at its next screen draw
//     (0021 ext_ui_tick, from param_value_text): a still screen kept the old six past the hold.
#include <cstdio>
#include <cstring>
#include <vector>

#include "check.h"
#include "engine.h"
#include "machines.h"
#include "platform.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;
static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static uint64_t g_us = 1000000;
static void later(int ms) { g_us += 1000ull * ms, host_fake_time(true, g_us); }
static const int kSix[6] = {5, 6, 7, 11, 12, 14};   // a patch's bytes behind CUT RES ENV ATK DEC REL (voices.cpp kJunoByte)

static void report(int cmd, int pat, const uint8_t* six, int vi = 0) {   // a 0x04 (note 60) or 0x03 for voice vi, JUNO
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, (uint8_t)cmd, (uint8_t)(kJuno | vi)};
  if (cmd == 0x04) m.push_back(60), m.push_back(100);
  float p[16] = {(float)pat, 0, 0, 0, 0, 0, 0, 127};
  for (int i = 0; i < 6; ++i) p[1 + i] = six[i];
  for (float x : p) m.push_back((uint8_t)x), m.push_back(0);
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void sliders(int pat, uint8_t* six) {
  for (int i = 0; i < 6; ++i) six[i] = juno::kPatches[pat].b[kSix[i]] & 0x7F;
}
static int knob_syncs(const uint8_t* six, int vi = 0) {   // 15 <voice vi> <six>: the patch's knobs to the Rytm
  int n = 0;
  for (auto& m : g_out)
    if (m.size() == 13 && m[4] == 0x15 && m[5] == vi && (!six || !memcmp(m.data() + 6, six, 6))) ++n;
  return n;
}
static int pat_writes(int pat, int t = 0) {   // 48 <track t> <P1> <pat> ...: the bank list's commit
  int n = 0;
  for (auto& m : g_out)
    if (m.size() > 9 && m[4] == 0x48 && m[6] == t && m[7] == 0 && m[8] == pat) ++n;
  return n;
}
static void answer(int status, int t = 0) {   // the Rytm's 48 for track t's P1
  const uint8_t m[10] = {0xF0, 0x7D, 0x52, 0x54, 0x48, 1, (uint8_t)t, 0, (uint8_t)status, 0xF7};
  engine_sysex(m, 10);
}
static bool plays(const uint8_t* six, int vi = 0) {   // the voice's CUT..REL now
  for (int i = 0; i < 6; ++i)
    if ((int)v[vi].p[1 + i] != six[i]) return false;
  return true;
}
static void pick(int from, int to, int t = 0, int vi = 0) {   // the bank list on track t (opened on PAT from), the cursor to row to, YES
  ui::src_bank_list(t, vi, kJuno, from, 1, 0);
  CHECK(ui::src_bank_state().on && ui::src_bank_state().row == from);
  ui::src_bank_enc(0, to - from, false);
  CHECK(ui::src_bank_state().row == to);
  ui::src_bank_list(t, vi, kJuno, from, 0, 1);
}
static void machines_rs_cp(bool rs_juno, bool cp_juno) {   // 33 for RS and CP (tracks 2, 3): JUNO (41) or analog (not ours)
  uint8_t m[13] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 2, 2,
                   (uint8_t)(rs_juno ? 41 : 2), (uint8_t)(rs_juno ? kJuno : 0x7F),
                   (uint8_t)(cp_juno ? 41 : 3), (uint8_t)(cp_juno ? kJuno : 0x7F), 0xF7};
  engine_sysex(m, 13);
  CHECK(ui::remote_track_machine(2) == (rs_juno ? (kJuno | 7) : 0x7F));   // (the voice byte: machine | voice 7)
  CHECK(ui::remote_track_machine(3) == (cp_juno ? (kJuno | 7) : 0x7F));
}

int main() {
  host_fake_time(true, g_us);
  hooks.to_rytm = out;
  engine_begin();   // (no card here: false, the voices and the remote UI are up)
  const uint8_t snap[4] = {41, kJuno};   // 33: track 0 (BD) is JUNO (the Rytm's machine 41)
  uint8_t m33[11] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, snap[0], snap[1], 0xF7};
  engine_sysex(m33, 11);
  CHECK(ui::remote_track_machine(0) == kJuno);

  // the track plays factory patch 3; its knobs are the patch's (the Rytm took the first 15)
  uint8_t a[6], b[6], c[6];
  sliders(3, a), sliders(20, b), sliders(40, c);
  CHECK(memcmp(a, b, 6) && memcmp(b, c, 6));
  report(0x04, 3, a);
  CHECK(v[0].juno && juno_pat[0] == 3 && knob_syncs(a) == 1 && plays(a));
  later(400);
  report(0x04, 3, a);
  CHECK(plays(a));

  // ---- the pick on a silent voice: PAT 20 written, the Rytm says written; the next trig carries PAT 20 and, until the
  // Rytm draws its screen, patch 3's six
  g_out.clear();
  pick(3, 20);
  CHECK(pat_writes(20) == 1);
  answer(0);
  CHECK(knob_syncs(b) == 1);   // the knobs asked for at once: no 0x03 needed
  CHECK(juno_pat[0] == 20 && plays(b));
  later(50);
  report(0x04, 20, a);
  CHECK(plays(b));
  later(2000);   // a still screen: 15 not in the knobs yet, two seconds on
  report(0x04, 20, a);
  CHECK(plays(b));
  report(0x04, 20, b);   // the Rytm drew: its knobs are the patch's - the hold is over
  CHECK(plays(b));
  uint8_t turned[6];
  memcpy(turned, b, 6);
  turned[0] = (uint8_t)(b[0] > 60 ? b[0] - 30 : b[0] + 30);   // CUT turned: it plays
  report(0x03, 20, turned);
  CHECK(plays(turned) && knob_syncs(b) == 1);

  // ---- the same while the voice sounds (a 0x03 with the new PAT first): one 15, the hold until the Rytm's knobs move
  g_out.clear();
  pick(20, 40);
  report(0x03, 40, turned);   // the Rytm's report beat its 48 answer
  answer(0);
  CHECK(knob_syncs(c) == 1 && plays(c));
  later(1000);
  report(0x04, 40, turned);
  CHECK(plays(c));
  report(0x03, 40, c);
  CHECK(plays(c));

  // ---- trigs held at the YES: the Rytm locks PAT on those steps (status 4) and the sound keeps PAT 40: no 15
  g_out.clear();
  pick(40, 3);
  answer(4);
  CHECK(knob_syncs(a) == 0 && juno_pat[0] == 40 && plays(c));

  // ---- a knob turned while the patch's six are held (the Rytm never drew): the Rytm's six from then on
  g_out.clear();
  pick(40, 20);
  answer(0);
  CHECK(knob_syncs(b) == 1 && plays(b));
  memcpy(turned, c, 6);
  turned[3] = (uint8_t)(c[3] > 60 ? c[3] - 20 : c[3] + 20);   // ATK
  report(0x03, 20, turned);
  CHECK(plays(turned));

  // ---- rc040 review H1: RS and CP share voice 7, and the Rytm routes a 15 to the track that trigged the voice last
  // (0021 jn_sysex, OWNER(voice)), which the Pi cannot tell. Both JUNO, CP's sound (patch 40) sounding last: a pick on
  // RS must not switch the voice, nor send a 15 that would land on CP's knobs and kit.
  const int kRsCp = 7;
  machines_rs_cp(true, true);
  report(0x04, 40, c, kRsCp);   // CP trigs: its patch 40 on voice 7, the knobs synced to it
  CHECK(v[kRsCp].juno && juno_pat[kRsCp] == 40 && knob_syncs(c, kRsCp) >= 1 && plays(c, kRsCp));
  g_out.clear();
  pick(3, 20, 2, kRsCp);   // RS's list (its own PAT 3), row 20, YES
  CHECK(pat_writes(20, 2) == 1);
  answer(0, 2);
  CHECK(knob_syncs(nullptr, kRsCp) == 0);   // no 15 for voice 7
  CHECK(juno_pat[kRsCp] == 40 && (int)v[kRsCp].p[0] == 40 && plays(c, kRsCp));   // CP's sound untouched
  report(0x03, 20, c, kRsCp);   // RS's sound reports its new PAT (RS sounding: it owns the voice now): the sync, as before
  CHECK(knob_syncs(b, kRsCp) == 1 && juno_pat[kRsCp] == 20 && plays(b, kRsCp));

  // ---- RS JUNO, CP analog: a JUNO voice 7 can only be RS's: the pick syncs at once, as BD's
  machines_rs_cp(true, false);
  report(0x04, 20, b, kRsCp);
  g_out.clear();
  pick(20, 40, 2, kRsCp);
  answer(0, 2);
  CHECK(knob_syncs(c, kRsCp) == 1 && juno_pat[kRsCp] == 40 && plays(c, kRsCp));

  // ---- CP JUNO, RS analog: the same for a pick on CP (track 3)
  machines_rs_cp(false, true);
  report(0x04, 40, c, kRsCp);
  g_out.clear();
  pick(40, 3, 3, kRsCp);
  answer(0, 3);
  CHECK(knob_syncs(a, kRsCp) == 1 && juno_pat[kRsCp] == 3 && plays(a, kRsCp));
  printf("test_presetpick: PASS\n");
  return 0;
}

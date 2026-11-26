// pi/engine/tests/test_voice_gate.cpp - voice_pi::EditGate (which of the VOICE page's eight knobs take a finger, and the Rytm's P
// index each writes) for each kind of track: PLAITS, JUNO, DAISYSP, DSP SAMPLE, SAMPLER, FM, MULTISAMPLE, WAVETABLE, GRANULAR,
// CHOP, LIVE IN, MIDI, an analog track, a track that is not ours, and a HOME tile's picked track. make -C pi/engine/tests
// (make -C core engines lib first).
#include <cstdio>
#include <cstring>
#include <vector>

#include "../voice_pi.h"
#include "check.h"
#include "engine.h"
#include "machines.h"
#include "voices.h"
#include "ui/page_keys.h"
#include "ui/remote.h"

namespace core { void voices_init(); }
using namespace core;

static const char kAll[8][5] = {"A", "B", "C", "D", "E", "F", "G", "H"};
static const char kNone[8][5] = {};

// voice vi plays machine m (a 0x03 report, as the Rytm's: the machine flag in the voice byte, P1..P16)
static void report(int vi, int m) {
  std::vector<uint8_t> r = {0xf0, 0x7d, 0x52, 0x54, 0x03, (uint8_t)(m | vi)};
  for (int i = 0; i < 16; ++i) r.push_back(64), r.push_back(0);
  r.push_back(0xf7);
  engine_sysex(r.data(), (int)r.size());
}

struct Gate { unsigned char e, k[8]; };
static Gate gate(bool sidecar, bool picked, int mm, bool smpl, int vi, const char (*l)[5] = kAll) {
  Gate g;
  voice_pi::EditGate(sidecar, picked, mm, smpl, vi, l, &g.e, g.k);
  return g;
}

int main() {
  voices_init();
  const int ms[] = {kPlaits, kJuno, kDaisy, kDspSample, kFm, kMultiMachine, kWaveMachine, kGranMachine, kChopMachine, kLiveMachine};
  for (int vi = 0; vi < 8; ++vi) report(vi, ms[vi]);
  // an ordinary machine: every knob in use, in order (a P1 that is a list cell for the engine's own pick is the one the Rytm picks)
  Gate g = gate(true, false, kPlaits, false, 0);
  CHECK((g.e & 0xFE) == 0xFE);
  for (int k = 0; k < 8; ++k) CHECK(g.k[k] == k);
  g = gate(true, false, kDaisy, false, 2);
  CHECK((g.e & 0xFE) == 0xFE);
  // P1 is the source pick (a list, a bank: index << 7 | bank): never on MULTISAMPLE, WAVETABLE, GRANULAR, CHOP, LIVE IN, JUNO, FM
  const int src[] = {kMultiMachine, kWaveMachine, kGranMachine, kChopMachine, kLiveMachine, kJuno, kFm};
  for (int m : src) {
    int vi = m == kJuno ? 1 : m == kFm ? 4 : m == kMultiMachine ? 5 : m == kWaveMachine ? 6 : m == kGranMachine ? 7 : 0;
    g = gate(true, false, m, false, vi);
    CHECK(!(g.e & 1));
    CHECK(g.k[0] == 0);
  }
  // DSP SAMPLE (not SAMPLER): P1..P8 as they are, none of them its SMP (P11)
  g = gate(true, false, kDspSample, false, 3);
  for (int k = 0; k < 8; ++k) CHECK(g.k[k] == k);
  CHECK((g.e & 0xFE) == 0xFE);
  // SAMPLER: its page's knobs map to the Rytm's P11 (SMP: banked, not editable) and the others
  g = gate(true, false, kDspSample, true, 3);
  bool sawsmp = false;
  for (int e = 0; e < 8; ++e) {
    if (g.k[e] == 10) { sawsmp = true; CHECK(!(g.e >> e & 1)); }
    else CHECK(g.k[e] < 16);
  }
  CHECK(sawsmp);
  CHECK(g.e != 0);
  bool moved = false;
  for (int e = 0; e < 8; ++e) moved |= g.k[e] != e;
  CHECK(moved);   // (SAMPLER's page maps its knobs: not P1..P8)
  // MIDI, an analog track (not a Sidecar machine), a picked tile, no voice: nothing
  CHECK(gate(true, false, kMidiMachine, false, 0).e == 0);
  CHECK(gate(false, false, kPlaits, false, 0).e == 0);
  CHECK(gate(true, true, kPlaits, false, 0).e == 0);
  CHECK(gate(true, false, kPlaits, false, -1).e == 0);
  // a knob not in use (no label) takes no finger
  CHECK(gate(true, false, kPlaits, false, 0, kNone).e == 0);
  char some[8][5] = {};
  strcpy(some[3], "X"), strcpy(some[5], "Y");
  g = gate(true, false, kPlaits, false, 0, some);
  CHECK((g.e & ~((1 << 3) | (1 << 5))) == 0);
  // UP + T1..T8 (49 1 n) and the sidebar's keys T1..T8 (pi/ui/host/shot.c N1: kPg) open the same page, and the table names
  // the key as the sidebar does (pi/ui/sidecar_ui.c kNavName): UP + T5 is FX (Send FX), not INSERT FX (VOICE > Insert)
  {
    static const char* const kLabel[8] = {"HOME", "SEQ", "PAT", "VOICE", "FX", "SONG", "REC", "SET"};
    static const int kPage[8] = {0, 4, 5, 8, 2, 10, 11, 1};   // sidecar_ui.h: HOME SEQ PAT VOICE SEND FX SONG REC SOUND
    uint32_t seen = 0, track_seen = 0;
    voice_pi::Seen(&seen, &track_seen);
    for (int k = 0; k < 8; ++k) {
      CHECK(sc_page_of_code(k + 1) && sc_streq(sc_page_of_code(k + 1)->name, kLabel[k]));
      const uint8_t m[] = {0xf0, 0x7d, 0x52, 0x54, 0x49, 1, 1, (uint8_t)(k + 1), 0xf7};
      engine_sysex(m, (int)sizeof m);
      const int got = voice_pi::Request(&seen, false, &track_seen);
      if (got != kPage[k]) printf("UP+T%d: page %d, sidebar %d\n", k + 1, got, kPage[k]);
      CHECK(got == kPage[k]);
    }
  }
  printf("test_voice_gate: ok\n");
  return 0;
}

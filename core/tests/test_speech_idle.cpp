// core/tests/test_speech_idle.cpp - PLAITS SPEECH with no note held stays silent when HARM moves. The speech engine's word
// banks (HARM above about 0.43) play a word per trigger and, once it ends, rest on its last frame. A HARM change into
// another word bank reloads the bank, and stock Plaits then drops the LPC controller into its free "scan" mode, which
// plays the bank continuously and, with a trigger patched (the voices always patch it), is not gated by the LPG
// (SpeechEngine's already_enveloped): a drone until the next note, level jumping with the bank. The controller now idles
// on the bank's last frame unless free running (teensy/setup_plaits_lib.sh), so a bank change alone makes no sound.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "check.h"
#include "voices.h"

using namespace core;

static std::vector<uint8_t> report(int cmd, int vb, int note, int vel, const std::vector<float>& p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)vb};
  if (cmd == 0x04) m.push_back(note), m.push_back(vel);
  for (float x : p) { int i = (int)x; m.push_back(i); m.push_back((int)((x - i) * 128) & 0x7f); }
  m.push_back(0xf7);
  return m;
}
static int16_t ring[kBlock * 64][kVoices];
static uint64_t mach[kMachines];
static int blk = 0;
static double sum_abs = 0;   // the voice 2 row's summed |sample| over every render(), a word's energy
static int render(int blocks) {   // the voice 2 row's peak (int16 counts)
  int pk = 0;
  for (int b = 0; b < blocks; ++b, ++blk) {
    render_voices(ring, kBlock * 64, (blk % 64) * kBlock, 0, kVoices, mach);
    for (int s = 0; s < kBlock; ++s) sum_abs += std::abs((int)ring[(blk % 64) * kBlock + s][2]);
    for (int s = 0; s < kBlock; ++s) pk = std::abs((int)ring[(blk % 64) * kBlock + s][2]) > pk ? std::abs((int)ring[(blk % 64) * kBlock + s][2]) : pk;
  }
  return pk;
}
// P1 ENG = SPEECH (zone 12 of 17), P2 HARM h, the rest at the defaults
static std::vector<float> P(float h) { return {(12 * 128 + 64) / 17.0f, h, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100}; }
static void params(float h) { auto t = report(0x03, kPlaits | 2, 0, 0, P(h)); voice_report(0x03, t.data(), (int)t.size()); }
static void note_on(float h) { auto t = report(0x04, kPlaits | 2, 48, 100, P(h)); voice_report(0x04, t.data(), (int)t.size()); }
static void note_off() { uint8_t o[7] = {0xf0, 0x7d, 0x52, 0x54, 0x11, (uint8_t)(kPlaits | 2), 0xf7}; voice_report(0x11, o, 7); }

int main() {
  hooks.to_rytm = [](const uint8_t*, int) {};
  voices_init();
  const int kSec = 4000;   // blocks in a second
  // a word at HARM 75 (word bank 2), held 0.5 s, then released: it sounds, and is gone 4 s later
  note_on(75);
  int word = render(kSec / 2);
  note_off();
  render(kSec * 4);
  int rest = render(kSec);
  printf("word at HARM 75: peak %d; 4 s after the note off %d\n", word, rest);
  CHECK(word > 500);
  CHECK(rest < 2);
  // no note, HARM sweeps through the word banks (an LFO: one report per block): silence all the way
  int worst = 0;
  for (int b = 0; b < kSec * 3; ++b) {
    params(60 + 45 * (float)std::sin(b / 4000.0 * 2 * M_PI * 2));   // HARM 15..105, 2 Hz: the banks -1, 0 .. 4
    int pk = render(1);
    worst = pk > worst ? pk : worst;
  }
  printf("HARM LFO, no note: peak %d\n", worst);
  CHECK(worst < 2);   // before: ~2000 (-24 dBFS), a drone
  // a step into another bank and rest: silent (before: the bank drones until the next note)
  params(100);
  CHECK(render(kSec) < 2);
  params(60);
  CHECK(render(kSec) < 2);
  // the next note still speaks (the trigger starts the word whatever bank HARM moved to)
  note_on(60);
  int again = render(kSec / 2);
  note_off();
  printf("next note at HARM 60: peak %d\n", again);
  CHECK(again > 500);
  render(kSec * 3);
  CHECK(render(kSec) < 2);
  // a word in flight finishes on its own bank when HARM moves on (note held, no new trigger): the same energy as without the
  // move, then silence. Before: the word was cut at the step (and the drone before that)
  note_on(75);
  sum_abs = 0;
  render(kSec * 4);
  double plain = sum_abs;
  note_off();
  render(kSec);
  note_on(75);
  render(kSec / 20);
  params(100);   // bank 2 -> 4, mid-word
  sum_abs = 0;
  int tail = render(kSec * 4);
  printf("word 75 -> 100 mid-word: energy %.0f of %.0f, peak after the step %d\n", sum_abs, plain, tail);
  CHECK(plain > 1e5);
  CHECK(sum_abs > plain * 0.7);   // (the first 50 ms were rendered before the step)
  CHECK(render(kSec) < 2);        // the held note rests silent in the new bank
  // HARM into the LPC phoneme zone (35) or the naive zone (20) mid-word: the word keeps playing from its own bank's frames (stock
  // and the first patch read the 15-entry phoneme table with a word frame index). The engine's LPG gates the output in those
  // zones, so a word is judged by what is left of it when HARM returns to its bank after 0.1 s
  for (float h : {35.0f, 20.0f}) {
    note_off();
    render(kSec);
    note_on(75);
    render(kSec / 20);
    params(h);
    render(kSec / 10);
    params(75);
    sum_abs = 0;
    int pk = render(kSec * 4);
    printf("word 75 -> %.0f for 0.1 s -> 75: energy after %.0f of %.0f, peak %d\n", h, sum_abs, plain, pk);
    CHECK(sum_abs > plain * 0.4);
    CHECK(pk < 3200);   // the over-read gave 3699 (garbage frames); the word peaks at ~2600
    CHECK(render(kSec) < 2);   // and it rests silent
    if (h < 30) continue;   // (the naive zone does not render the LPC controller: the word waits there and finishes on return)
    // a long stay in the zone: the word ends there, the phonemes take over, nothing sounds when HARM comes back
    note_off();
    render(kSec);
    note_on(75);
    render(kSec / 20);
    params(h);
    render(kSec * 4);
    params(75);
    CHECK(render(kSec) < 2);
    params(100);
    CHECK(render(kSec) < 2);
  }
  // a held note under a 2 Hz HARM LFO 15..105: the first word finishes, no drone afterwards
  note_off();
  render(kSec);
  note_on(75);
  sum_abs = 0;
  int lfo_pk = 0;
  for (int b = 0; b < kSec * 3; ++b) {
    params(60 + 45 * (float)std::sin(b / 4000.0 * 2 * M_PI * 2));
    int pk = render(1);
    if (b < kSec / 2) lfo_pk = pk > lfo_pk ? pk : lfo_pk;
  }
  printf("held note, HARM LFO: first 0.5 s peak %d, energy %.0f\n", lfo_pk, sum_abs);
  CHECK(lfo_pk > 2000);   // a whole word's level, not a cut stub
  CHECK(sum_abs > plain * 0.6);   // the word plays on across the phoneme-zone crossings (the engine's LPG mutes its output below HARM 42)
  int late = 0;
  for (int b = 0; b < kSec * 2; ++b) {
    params(60 + 45 * (float)std::sin(b / 4000.0 * 2 * M_PI * 2));
    int pk = render(1);
    late = pk > late ? pk : late;
  }
  CHECK(late < 2);   // after the word ends: silent through every crossing
  note_off();
  render(kSec);
  // a retrig that changes the bank in the same report speaks in the new bank
  note_on(75);
  render(kSec / 20);
  note_on(100);
  int retrig = render(kSec / 2);
  printf("retrig into another bank: peak %d\n", retrig);
  CHECK(retrig > 500);
  note_off();
  render(kSec * 4);
  CHECK(render(kSec) < 2);
  printf("test_speech_idle: PASS\n");
  return 0;
}

// core/tests/test_pagetwo.cpp - a machine chosen in the Rytm's machine list starts with its page 2's (P9..P16) defaults.
//
// Hardware report (0.4.1 candidate): a DSP SAMPLE voice sometimes made no sound; each time its TUN was off 0, its STA
// off 0 or its END not at the end. Stock's set_machine writes the chosen machine's P1..P8 defaults and leaves P9..P16
// (the stock sample words 17..24) as the sound had them [measured in rytm-emu gate040: BD LIVE IN -> DSP SAMPLE kept
// LIVE IN's page 2, 77 0 0 0 0 0 0 0: TUN +13, END 0, LEV 0; fresh from the base: 64 64 0 0 0 120 0 100]. The Pi now
// writes page 2's defaults (48) once the list closes (2B: MachineListView off the top), for a track the list moved to
// another machine; a kit / sound load, NO, or a choice of the machine it had keeps the sound's words.
#include <cstdio>
#include <cstring>
#include <vector>

#include "check.h"
#include "engine.h"
#include "machines.h"
#include "platform.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;
static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }

static void from_rytm(uint8_t cmd, std::vector<uint8_t> b) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, cmd, 1};
  m.insert(m.end(), b.begin(), b.end());
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void machine(int t, int rytm_machine, int vb) { from_rytm(0x33, {(uint8_t)t, 1, (uint8_t)rytm_machine, (uint8_t)vb}); }
static void top(uint32_t vt) {   // 2B: the Rytm's top view (state, views, vt in five 7-bit groups, age)
  from_rytm(0x2B, {0x0A, 0x0A, (uint8_t)(vt >> 28 & 0x7F), (uint8_t)(vt >> 21 & 0x7F), (uint8_t)(vt >> 14 & 0x7F),
                   (uint8_t)(vt >> 7 & 0x7F), (uint8_t)(vt & 0x7F), 0});
}
constexpr uint32_t kList = 0x401E5BF4u, kSrcPage = 0x40234CFCu;   // MachineListView, the SRC page (measured, gate040)
static void list_open() { top(kList); }
static void list_close() { top(kSrcPage); }
// 48 <track> <param> <value>: our writes for track t, P9..P16 (-1 not written)
static std::vector<int> page_two(int t) {
  std::vector<int> w(8, -1);
  for (auto& m : g_out)
    if (m.size() >= 10 && m[4] == 0x48 && m[6] == t && m[7] >= 8 && m[7] < 16) w[m[7] - 8] = m[8];
  return w;
}
static bool none(int t) { return page_two(t) == std::vector<int>(8, -1); }
// a 0x04 (note 60) for voice vi on DSP SAMPLE with these P9..P16 (P1..P8 its SRC defaults, MIX 0)
static void dsps_trig(int vi, const std::vector<int>& p2) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x04, (uint8_t)(kDspSample | vi), 60, 100};
  const int p1[8] = {80, 64, 64, 40, 0, 0, 64, 100};
  for (int k = 0; k < 8; ++k) m.push_back((uint8_t)p1[k]), m.push_back(0);
  for (int k = 0; k < 8; ++k) m.push_back((uint8_t)p2[k]), m.push_back(0);
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}

int main() {
  host_fake_time(true, 1000000);
  hooks.to_rytm = out;
  engine_begin();
  const std::vector<int> kDspsP2 = {64, 64, 0, 0, 0, 120, 0, 100};   // TUN FIN SMP MOD STA END LOP LEV
  const std::vector<int> kLiveP2 = {77, 0, 0, 0, 0, 0, 0, 0};          // what LIVE IN left (its own page-2 defaults)

  // a snapshot: BD (track 0) LIVE IN (Rytm machine 48), SD (1) analog, the rest analog
  {
    std::vector<uint8_t> b = {0, 12};
    for (int t = 0; t < 12; ++t) b.push_back((uint8_t)(t == 0 ? 48 : t)), b.push_back(t == 0 ? kLiveMachine : 0x7F);
    from_rytm(0x33, b);
  }

  // ---- the owner's case: BD from LIVE IN to DSP SAMPLE in the list (the cursor passes WAVETABLE, GRANULAR on the way
  // up, then YES on DSP SAMPLE); before the fix the words stayed LIVE IN's: TUN +13, END 0, LEV 0 - silent
  g_out.clear();
  list_open();
  machine(0, 47, kGranMachine);
  machine(0, 46, kWaveMachine);
  machine(0, 45, kDspSample);
  CHECK(none(0));   // nothing while the list is open (its cursor's previews)
  list_close();
  CHECK(page_two(0) == kDspsP2);
  // the Rytm's answer comes as the next report: the voice plays TUN 0, STA 0, END the end, LEV 100
  dsps_trig(0, kDspsP2);
  CHECK(v[0].dsps && v[0].p[8] == 64 && v[0].p[12] == 0 && v[0].p[13] == 120 && v[0].p[15] == 100);
  // (what the voice got before the fix: LIVE IN's words)
  dsps_trig(0, kLiveP2);
  CHECK(v[0].p[13] == 0 && v[0].p[15] == 0);

  // ---- from an analog machine (its sample layer's words are no DSP SAMPLE settings): SD to DSP SAMPLE
  g_out.clear();
  list_open();
  machine(1, 45, kDspSample);
  list_close();
  CHECK(page_two(1) == kDspsP2 && none(0));
  list_close();   // a second 2B off the list (once a second on the SRC page): nothing again
  CHECK(page_two(1) == kDspsP2);
  {
    int n = 0;
    for (auto& m : g_out) n += m.size() >= 10 && m[4] == 0x48;
    CHECK(n == 8);
  }

  // ---- NO, or the cursor back to the machine it had: the sound's words stay
  g_out.clear();
  list_open();
  machine(1, 0, 0x7F);   // (passing an analog machine)
  machine(1, 45, kDspSample);
  list_close();
  CHECK(none(1));

  // ---- a kit load: a snapshot, or one track's 33 with no list open: the kit's words stay
  g_out.clear();
  machine(2, 45, kDspSample);
  list_close();
  CHECK(none(2));
  list_open();
  {
    std::vector<uint8_t> b = {0, 12};
    for (int t = 0; t < 12; ++t) b.push_back((uint8_t)(t == 3 ? 44 : t)), b.push_back(t == 3 ? 0 : 0x7F);
    from_rytm(0x33, b);
  }
  list_close();
  CHECK(none(3));
  // a pattern change to another kit while the list is open (performance): its machines are the kit's
  list_open();
  machine(4, 45, kDspSample);
  from_rytm(0x3B, {5});
  list_close();
  CHECK(none(4));

  // ---- the other machines: PLAITS, WAVETABLE (its records' defaults), 6-OP FM's E..H only; LIVE IN / CHOP / MIDI /
  // MD / the user slots are the Rytm's own (0026 cu_defs_tick, 0021): none from the Pi
  g_out.clear();
  list_open();
  machine(5, 44, kPlaits);
  list_close();
  CHECK(page_two(5) == kDspsP2);
  list_open();
  machine(6, 46, kWaveMachine);
  list_close();
  CHECK(page_two(6) == std::vector<int>({0, 64, 100, 64, 0, 0, 0, 0}));
  list_open();
  machine(7, 40, kFm);
  list_close();
  CHECK(page_two(7) == std::vector<int>({-1, -1, -1, -1, 0, 0, 0, 0}));
  list_open();
  machine(8, 48, kLiveMachine);
  list_close();
  CHECK(none(8));
  list_open();
  machine(9, 35, 0x50);   // U1
  list_close();
  CHECK(none(9));

  printf("test_pagetwo: PASS\n");
  return 0;
}

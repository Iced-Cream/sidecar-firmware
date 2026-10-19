// core/machines.h - what a report's voice byte names, and the knobs' zones (sim/protocol.py's docstring and
// eng_zone; rytm-mods 0021). Moved from teensy/voiceloop/voiceloop.ino (kVoices, kBlock, eng_zone, the curated
// Plaits list).
#pragma once
#include <stdint.h>

namespace core {

constexpr int kVoices = 8;    // the Rytm's physical voices
constexpr int kBlock = 12;    // frames a render call makes (plaits::kBlockSize; voices.cpp checks it)

// The machine: voice byte & 0x78, matched exactly; anything else plays as PLAITS.
enum Machine : int { kPlaits = 0x00, kJuno = 0x08, kDaisy = 0x10, kDspSample = 0x20, kDigidrum = 0x40, kFm = 0x48 };
// GRAIN as its own machine (1026): DSP SAMPLE's GRAIN mode with DSP SAMPLE's words (P9..P16 TUN FIN SMP SPRY POS SIZE
// DENS LEV: MOD's word is SPRY, the spray), no resonator. NOT on the Rytm: rui1026's 0021 has no room for the machine,
// so no Rytm sends this flag and nothing shows it; GRAIN is DSP SAMPLE's MOD = GRAIN there. The flag is a placeholder
// (one of the free ones: 0x18 0x28 0x30 0x38 0x78) for a later 0021.
constexpr int kGrainMachine = 0x18;
constexpr float kGrainModValue = 64.0f;   // a MOD value in GRAIN's zone (rytm::SampleModes::Zone(64, 7) == kGrain)
inline int machine_of(uint8_t voice_byte) { return voice_byte & 0x78; }
// The user slots U1..U5 (flags 0x50 0x58 0x60 0x68 0x70) -> 0..4; -1 not a user slot (rmp_slot_of_flag's rule).
inline int user_slot_of(int m) { return (m >= 0x50 && m <= 0x70 && !(m & 7)) ? (m - 0x50) >> 3 : -1; }

// The curated ENG lists (rytm-mods 0021, 2026-10-09): the knob's value v (P's integer part, 0..127) in N equal
// zones, zone (v * N) >> 7, at most N - 1. PLAITS: 17 of Plaits' models (NOISE, the 6-OP FMs and the drums left
// out); DIGIDRUM: dd_voice.h's 11 engines; DAISYSP and DSP SAMPLE: MODAL STRING RESON.
inline int eng_zone(float p, int n) {
  int v = (int)p;
  v = v < 0 ? 0 : v > 127 ? 127 : v;
  int z = (v * n) >> 7;
  return z >= n ? n - 1 : z;
}
constexpr int kPlaitsEngs = 17;
constexpr uint8_t kPlaitsModels[kPlaitsEngs] = {0, 1, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20};

// A report's P1..Pn: two bytes each, the value 0..127 then its fraction in 1/128 steps.
inline void decode_params(const uint8_t* raw, int np, float* p) {
  for (int k = 0; k < np; ++k) p[k] = raw[2 * k] + raw[2 * k + 1] / 128.0f;
}

}  // namespace core

// core/rec/rec_names.h - what the two recorders share besides the WAV header (rec_wav.h): the sources and their
// file names, the channel lists ("master,bd,+in_l"), the take names (/Recordings/<prefix>_NNN/NN_NAME.wav) and the
// PLAY / STOP arming. The Pi's multitrack recorder (pi/engine/recorder.h) records any of the 23 sources; the
// Sidecar Mini's (core/rec/mini_rec.h) the Overbridge IN ones (0..12). Portable, cold (program flash on the Teensy).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace rec {

// Sources (their id is the file's NN): 0 MASTER (IN 0/1, stereo), 1..12 IN 0..11 (mono), 13..20 the Sidecar's voices
// 1..8 (mono), 21/22 FX 1/2 returns (stereo).
const int kSources = 23, kMaster = 0, kIn = 1, kVoice = 13, kFx = 21;
const int kInChannels = 12, kVoices = 8, kSide = kVoices + 4;   // the OUT side: 8 voices, FX1 L/R, FX2 L/R
const uint32_t kMaxData = 0xFFF00000u;   // a file's data limit (FAT32: 4 GB - 1, and RIFF's 32-bit sizes)
const uint32_t kInMask = ((1u << kInChannels) - 1) << kIn;   // the 12 IN channels as mono files ("stems")

struct Source { const char* name; uint8_t width; uint8_t side; uint8_t ch; };   // side: from out_block; ch: its first
const Source& source(int id);
// IN L/R (ids 11, 12) carry the Rytm's FX return (rytm-mods 0030-fx-in, core::fxin) while set: named FX_L / FX_R
// (11_FX_L.wav, 12_FX_R.wav). "fx_l" / "fx_r" parse either way.
void set_fxin(bool on);
bool fxin();
// "all", "none", or a comma list of ids or names (case-insensitive: master, main_l, bd, rs-cp, in_l, voice1, fx1, ...);
// "+id"/"-id" add or remove. False: an unknown entry (err says which).
bool parse_list(const char* s, uint32_t* mask, char* err, unsigned errlen);
int list_ids(uint32_t mask, char* out, unsigned cap);   // "0,1,13"
unsigned samples_per_frame(uint32_t mask);
unsigned files(uint32_t mask);
uint32_t bytes_per_second(uint32_t mask, unsigned bits);

// take names: "<prefix>_NNN" (NNN from 001, one more than the highest of that prefix in /Recordings)
bool valid_prefix(const char* p);                       // [A-Za-z0-9_-], 1..24
int take_number(const char* name, const char* prefix);  // NNN of "<prefix>_NNN", else -1
void take_name(char* out, unsigned cap, const char* prefix, int n);
void take_dir(char* out, unsigned cap, const char* take);           // /Recordings/<take>
void file_path(char* out, unsigned cap, const char* take, int id);  // /Recordings/<take>/<NN>_<NAME>.wav
bool is_wav_name(const char* name);                                  // *.wav / *.WAV

// PLAY / STOP while armed (MIDI realtime: FA start, FB continue, FC stop), any core
inline void transport_gate(uint8_t s, bool arm_mode, volatile uint8_t* go) {
  if (s == 0xFA || s == 0xFB) {
    if (arm_mode) *go = 1;
  } else if (s == 0xFC) {
    if (arm_mode) *go = 0;
  }
}

}  // namespace rec

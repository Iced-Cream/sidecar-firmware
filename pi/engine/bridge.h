// pi/engine/bridge.h - the engine core as plain functions for the kernel's Circle-compiled files (kernel, cores,
// guard, console): the core's headers pull Plaits' C++ library headers, which Circle's flags hide. bridge.cpp is
// compiled hosted, with the core.
#pragma once
#include <stdint.h>

namespace bridge {
const int kVoices = 8, kBlock = 12, kMachines = 7;   // core::kVoices, kBlock, kMachines (bridge.cpp checks)
// Before begin(): /Samples/pi-test/sine440.wav (2 s, 440 Hz, 16-bit mono, 48 kHz) and, if the project's pool file
// is missing, /.rytm/pools/_default.txt naming it as slot 1; nothing that exists is touched. False: no card.
bool selftest_content();
// The core's big buffers (core/heap_buffers.h: Plaits' scratch, DSP SAMPLE's heads and rings, the pool's files, the
// card's folder table): the bytes to reserve, then their placement into that memory (zeroed here). Once, before
// begin() and before the cores start. Without CORE_HEAP_BUFFERS (the Mac's port): 0, and nothing to place.
unsigned long heap_bytes();
void heap_place(void* mem);
bool begin(void (*to_host)(const uint8_t* m, int n));   // core::hooks.to_rytm = to_host; core::engine_begin()
void sysex(const uint8_t* d, int len);                  // core::engine_sysex
uint32_t render(int16_t (*block)[kVoices], int v0, int v1, uint64_t* mach);   // one block of voices v0..v1-1
bool idle_prepare();
bool idle_save();
void idle_stream();
// The control core's loop, every pass: the 13-byte 1D (user slots, all EMPTY) every 5 s, as the Teensy's loop sends
// it (core::slots_1d(5000)): a Rytm whose 1D went missing sees a host again and asks for the slots' 1A.
void tick();
// The remote UI (core/ui/remote.h): its tick runs in tick(); the status for its SIDECAR page (core 0's stats, once a
// second: plain words); the console's rui=on|off (the page opened / closed from here) and rui (its numbers, logged).
// The rest of the remote UI's SIDECAR page (1021): the recorder (1 idle, 2 armed, 3 recording, 4 off), the network
// (eth / wifi: 0 none, 1 down, 2 up; the address), U1..U5 as the Rytm shows them. Only up / down and the address of
// the network go to the Rytm, never the Wi-Fi's name or key.
struct RuiMore {
  int rec;
  unsigned rec_secs;
  int eth, wifi;
  unsigned char ip[4];
  char user[5][9];
};
void rui_status(int ob_running, uint32_t underruns, int c1, int c2, int c3, const RuiMore* more = nullptr);
bool rui_command(const char* p);
// The IN L/R source (core/host_link.h, 26 / 27): the console's `fxin`, `fxin=1|0|?` (the answer in out; false: not an fxin
// command) and /status's "fxin":{...} (any core, plain reads)
bool fxin_command(const char* p, char* out, int cap);
int fxin_json(char* out, int cap);
// The Sidecar's LFOs (core/lfo.h): the Rytm's channel messages (a track's trig retrigs, its knob's CC is the base), the
// console's `lfo`, `lfo=N:KEY:VALUE` (false: not an lfo command) and /status's "lfo":{...} (any core, plain reads).
void lfo_midi(uint8_t st, uint8_t d1, uint8_t d2);
bool lfo_command(const char* p, char* out, int cap);
int lfo_json(char* out, int cap);
// The remote UI's input tap (core/ui/extra.h: SCOPE's waveform, the meters): tap_on() while a page reads it, then each
// IN block's frames (channel c of frame i at f[i * stride + c], the Rytm's 12 channels first) from the IN completion.
bool tap_on();
void tap_in(const int32_t* f, int n, int stride);
// The remote UI's REC page (core/ui/extra.h RecInfo): the recorder's state from core 0 (rec_pi.cpp), and the page's
// request for it taken there: 0 none, 1 start, 2 stop, 3 arm, 4 disarm. state 0: no recorder (the page hidden).
struct RuiRec {
  int state;   // 0 none, 1 idle, 2 armed, 3 recording, 4 finishing
  unsigned secs, free_mb, total_mb, need_kbps, in_mask;
  char take[24], error[32];
  int ch_select;   // 1026: the trig keys pick the take's channels on the REC page
};
void rui_rec(const RuiRec& r);
int rui_rec_take();
unsigned rui_rec_toggles();   // 1026: the REC page's trig keys since the last call (bit k: T(k+1); core/ui/extra.h)
// The REC page's playback (core/ui/extra.h PlayInfo; rec_play.h): its state from core 0, the page's input taken there
// (B pushed: toggle; B / C / D turned: channel, take, level deltas). state 0: no take to play.
struct RuiPlay {
  int state;   // 0 none, 1 stopped, 2 playing, 3 stopping
  int level, ch_i, channels, take_i, takes;
  unsigned pos_ms, len_ms;
  char take[24], channel[12], error[32];
};
void rui_play(const RuiPlay& p);
bool rui_play_take(int* toggle, int* channel, int* take, int* level);
void reinit_voices();                  // core::voices_init and every voice back on PLAITS (after the boot self-check)
// The boot bench (one core at a time, before the render loops start): voice 0 on the machine (the note adapter's
// defaults, note 48, a retrigger on note 55 halfway), 4000 blocks = 1 s of audio. Returns the render ticks (generic
// timer) in total; *worst_block the longest single block; *peak (if given) voice 0's largest |sample| (a machine
// that renders silence shows 0). Ends with the voices as at boot (reinit_voices).
uint64_t bench(const char* machine, uint32_t* worst_block, int* peak = nullptr);
const char* machine_name(int voice);   // the voice's current machine: PLAITS JUNO FM DAISY DSPS DD USER
const char* stat_name(int k);          // core's kM* order: fm juno dsps daisy dd plaits user
int card_folders();
const char* project();
int pool_slots();
uint32_t to_host_dropped();
}  // namespace bridge

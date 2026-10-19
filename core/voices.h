// core/voices.h - the eight voices (moved from teensy/voiceloop/voiceloop.ino): their machines' state, what a
// report does to a voice, one block of each voice's audio, DSP SAMPLE's card work between requests. Module state,
// one engine per program, as before. Voice handling is sim/obhost.py's (sim/protocol.py's docstring).
#pragma once
#include "platform.h"   // first: the Teensy port defines PROGMEM, which puts JUNO's factory table (juno_patches.h) in flash

#include <DaisySP.h>
#include <Digidrum.h>
#include <Juno.h>
#include <Plaits.h>
#include <Sampler.h>

#include "engines/stream_voice.h"
#include "hooks.h"
#include "machines.h"
#include "pool/pool.h"
#include "heap_buffers.h"
#include "pool/sample_ram_pool.h"

namespace core {

struct V {
  plaits::Voice voice;
  plaits::Patch patch;
  plaits::Modulations mod;
  float p[16];         // P1..P16, 0..127.99 (P9..P16: EXT page 2)
  float target;        // the note glide moves toward
  bool live;
  bool trig;
  float note, level;
  bool daisy;          // DAISYSP or DSP SAMPLE (else PLAITS)
  bool dsps;           // DSP SAMPLE: P5/P6 are MIX/FB; its sample streams from the card (the GRAIN machine too)
  bool grain;          // the GRAIN machine (machines.h kGrainMachine): dsps in GRAIN mode, P12 SPRY, no resonator
  bool dd;             // DIGIDRUM
  bool juno;           // JUNO
  bool fm;             // 6-OP FM: x.voice plays through fms[] (fm_machine.h)
  int8_t user;         // a user slot U1..U5 (0..4) plays the voice (hooks.user_*); -1 none
};

extern V v[kVoices];
extern rytm::DaisyEngine daisy[kVoices];
extern uint8_t dd_mem[kVoices][512];
ddv* dd_voice(int i);
extern juno::Voice junos[kVoices];
#ifdef CORE_HEAP_BUFFERS   // heap_buffers.h: placed at boot
extern char (*shared)[16384];
#else
extern char shared[kVoices][16384];   // Plaits' scratch; a voice on a user slot keeps its plugin state here
#endif
extern rytm::FmMachine fms[kVoices];
extern uint8_t fm_stage[RYTM_RENDER_CORES][rytm::FmMachine::kStageBytes];   // per render core (plaits::fm_core())
// DSP SAMPLE: the streams, their buffers, the prepares between requests
extern rytm::StreamVoice streams[kVoices];
#ifdef CORE_HEAP_BUFFERS   // heap_buffers.h: placed at boot
extern uint8_t (*stream_heads)[rytm::StreamVoice::kHead];
extern uint8_t (*stream_rings_a)[rytm::StreamVoice::kRing];
extern uint8_t (*stream_rings_b)[rytm::StreamVoice::kRing];
#else
extern uint8_t stream_heads[kVoices][rytm::StreamVoice::kHead];
extern uint8_t stream_rings_a[4][rytm::StreamVoice::kRing];
extern uint8_t stream_rings_b[4][rytm::StreamVoice::kRing];
#endif
extern int prepared[kVoices][3];   // folder, file, STA
extern float trig_note[kVoices];
extern int want_prep[kVoices][4];
extern bool prep_pending[kVoices], trig_pending[kVoices];
extern float pending_note[kVoices];
extern uint32_t trig_wait_t[kVoices], max_trig_wait_us;
extern uint32_t refills, refill_kb, max_refill_us;
// the sample RAM (pool/sample_ram_pool.h); psram null without one
extern rytm::SampleRamPool* psram;
extern rytm::SampleModes* engines;
extern bool from_ram[kVoices];
extern int ram_pin[kVoices][2];
// 6-OP FM and JUNO
extern int fm_gen, fm_key[kVoices], fm_pat[kVoices];
extern int juno_pat[kVoices], juno_key[kVoices];
extern uint8_t juno_kb[kVoices][18], juno_hold[kVoices][6];
extern uint32_t juno_hold_t[kVoices];
extern bool juno_holding[kVoices];

int plaits_model(const V& x);
int dd_engine(const V& x);
int daisy_model(const V& x);
int dsp_mode(const V& x);
void apply(V& x);
void ram_release(int vi);
void prepare_stream(int vi);
void trigger_stream(int vi, int note);
bool prepare_pending();
void fm_select(int vi, int pat);
void fm_relist();
int juno_pick(int pat, const uint8_t** b, const char** name);
void juno_select(int vi, int pat, bool sync, bool force);
void juno_relist();
bool refill_neediest(uint32_t below);

// Boot: the sample RAM (after the card) and the voices (after the project: pool_load resets prepared[]).
void sample_ram_init();
void voices_init();
// A 0x04 / 0x03 report (d: the whole F0..F7, len bytes) and a 0x11 note-off.
void voice_report(int cmd, const uint8_t* d, int len);
void voice_note_off(int vi);
void voice_params_changed(int vi);   // x.p changed on the control side (core/lfo.h): the machine takes it
void voice_silence(int vi);   // MIDI CC 120: note-off and the output ramped out in 5 ms, silent until the next trig
// One block (kBlock frames) of voices v0..v1-1 into ring rows (w + s) % ring_len, column = voice, int16; the
// cycles each machine took added to mach[k] (kM*); returns the block's cycles. Voices v0..v1-1 only: several cores
// may render disjoint ranges at once (pi/engine) when the build sets RYTM_RENDER_CORES to the core count and the
// platform defines rytm_render_core() (the state voices share is per core then: the 6-OP FM stage and
// plaits::fm_user_bank, GRAIN's budget, stmlib's Random, rand(); tested by core/tests/mc/), each core with its own
// mach array (two cores with voices of one machine race on mach[k]), and nothing else runs meanwhile (engine.h:
// steps 1, 2 and 4 between blocks).
enum { kMFm, kMJuno, kMDsps, kMDaisy, kMDd, kMPlaits, kMUser, kMachines };
uint32_t render_voices(int16_t (*ring)[kVoices], int ring_len, int w, int v0, int v1, uint64_t* mach);

}  // namespace core

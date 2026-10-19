// A C interface to Mutable Instruments Plaits (MIT, (c) Emilie Gillet), for the
// Teensy simulator: one voice per handle, any number of frames per call.
#include <cstring>
#include <new>

#include "plaits/dsp/voice.h"
#include "plaits/resources.h"
#include "stmlib/utils/buffer_allocator.h"

#include "fm_machine.h"

namespace {
struct Handle {
  plaits::Voice voice;
  char shared[16384];
  plaits::Patch patch;
  plaits::Modulations mod;
};
// A 6-OP FM voice (fm_machine.h): a Plaits voice of its own.
struct FmHandle {
  plaits::Voice voice;
  char shared[16384];
  plaits::Patch patch;
  plaits::Modulations mod;
  rytm::FmMachine fm;
  uint8_t stage[rytm::FmMachine::kStageBytes];
};
}  // namespace

extern "C" {

void plaits_render2(void* p, int engine, float note, float harmonics, float timbre,
                    float morph, float decay, float lpg_colour, int trigger,
                    float level, float fm_amount, float timbre_amount,
                    float morph_amount, float aux_mix, float* out, int n);

void* plaits_new() {
  Handle* h = new (std::nothrow) Handle();
  if (!h) return nullptr;
  stmlib::BufferAllocator allocator(h->shared, sizeof(h->shared));
  h->voice.Init(&allocator);
  std::memset(&h->patch, 0, sizeof(h->patch));
  std::memset(&h->mod, 0, sizeof(h->mod));
  h->patch.note = 48.0f;
  h->patch.harmonics = 0.5f;
  h->patch.timbre = 0.5f;
  h->patch.morph = 0.5f;
  h->patch.decay = 0.5f;
  h->patch.lpg_colour = 0.5f;
  h->mod.trigger_patched = true;
  return h;
}

void plaits_free(void* p) { delete static_cast<Handle*>(p); }

// engine 0..23; note in semitones (MIDI numbering); the rest 0..1. `trigger` > 0
// starts a note on the first block of this call. Writes n floats, -1..1.
void plaits_render(void* p, int engine, float note, float harmonics, float timbre,
                   float morph, float decay, float lpg_colour, int trigger,
                   float level, float* out, int n) {
  plaits_render2(p, engine, note, harmonics, timbre, morph, decay, lpg_colour,
                 trigger, level, 0.0f, 0.0f, 0.0f, 0.0f, out, n);
}

// With the internal envelope's modulation amounts (fm -1..1, timbre and morph
// 0..1: with nothing patched, Plaits applies its decay envelope through them)
// and an OUT -> AUX crossfade (0..1).
void plaits_render2(void* p, int engine, float note, float harmonics, float timbre,
                    float morph, float decay, float lpg_colour, int trigger,
                    float level, float fm_amount, float timbre_amount,
                    float morph_amount, float aux_mix, float* out, int n) {
  Handle* h = static_cast<Handle*>(p);
  h->patch.engine = engine;
  h->patch.note = note;
  h->patch.harmonics = harmonics;
  h->patch.timbre = timbre;
  h->patch.morph = morph;
  h->patch.decay = decay;
  h->patch.lpg_colour = lpg_colour;
  h->mod.level = level;
  h->patch.frequency_modulation_amount = fm_amount;
  h->patch.timbre_modulation_amount = timbre_amount;
  h->patch.morph_modulation_amount = morph_amount;
  plaits::Voice::Frame frames[plaits::kMaxBlockSize];
  int done = 0;
  bool first = true;
  while (done < n) {
    int size = n - done;
    if (size > (int)plaits::kMaxBlockSize) size = plaits::kMaxBlockSize;
    h->mod.trigger = (trigger && first) ? 1.0f : 0.0f;
    first = false;
    h->voice.Render(h->patch, h->mod, frames, size);
    for (int i = 0; i < size; ++i)
      out[done + i] = (frames[i].out * (1.0f - aux_mix) + frames[i].aux * aux_mix) / 32768.0f;
    done += size;
  }
}

}  // extern "C"

// --- the 6-OP FM machine (fm_machine.h) ---
extern "C" {

void* fm_new() {
  FmHandle* h = new (std::nothrow) FmHandle();
  if (!h) return nullptr;
  stmlib::BufferAllocator allocator(h->shared, sizeof(h->shared));
  h->voice.Init(&allocator);
  std::memset(&h->patch, 0, sizeof(h->patch));
  std::memset(&h->mod, 0, sizeof(h->mod));
  h->fm.Init();
  return h;
}

void fm_free(void* p) { delete static_cast<FmHandle*>(p); }

// 128 packed bytes, or NULL: silence.
void fm_voice(void* p, const uint8_t* v) { static_cast<FmHandle*>(p)->fm.SetVoice(v); }

// P1..P8 (8 floats, 0..127.99).
void fm_knobs(void* p, const float* k) { static_cast<FmHandle*>(p)->fm.SetKnobs(k); }

void fm_note(void* p, float note, float velocity) { static_cast<FmHandle*>(p)->fm.NoteOn(note, velocity); }

void fm_note_off(void* p) { static_cast<FmHandle*>(p)->fm.NoteOff(); }

void fm_render(void* p, float* out, int n) {
  FmHandle* h = static_cast<FmHandle*>(p);
  h->fm.Render(&h->voice, &h->patch, &h->mod, h->stage, out, n);
}

// Plaits' ROM voice i (0..95: its three DX7 banks in order), 128 packed bytes.
void fm_rom_voice(int i, uint8_t* out) {
  const uint8_t* banks[3] = {plaits::syx_bank_0, plaits::syx_bank_1, plaits::syx_bank_2};
  std::memcpy(out, banks[(i / 32) % 3] + (i % 32) * 128, 128);
}

}  // extern "C"

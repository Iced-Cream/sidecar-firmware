// C entry points for the simulator (sim/samplemodes.py): DSP SAMPLE's
// playback modes (sample_modes.h) and the sample RAM's allocator
// (sample_ram.h), the same code the Teensy runs (libraries/Sampler).
#include <new>

#ifndef RYTM_NO_OPS
#define RYTM_SAMPLE_OPS 1   // count reads, run them through the cache model below
#endif                      // (libsampler_fast: neither, for timing as the Teensy runs)
#include "sample_modes.h"
#include "sample_ram.h"

// The Teensy 4.1's L1 data cache, as a model for the PSRAM estimate
// (docs/psram.md): 32 KB, 4 ways, 32-byte lines, LRU (Cortex-M7, as the
// i.MX RT1062 has it; the Teensy core maps PSRAM write-back cached). Every
// sample read of every voice goes through it; a miss is a 32-byte PSRAM read
// on the Teensy. Nothing else competes for it here (on the Teensy the voices'
// own state and the other machines do): a lower bound on the misses.
#ifdef RYTM_SAMPLE_OPS
namespace {
const int kSets = 256, kWays = 4;
uintptr_t tags[kSets][kWays];
uint32_t ages[kSets][kWays], tick = 0;
uint64_t touches = 0, misses = 0;
}  // namespace

namespace rytm {
void SampleOpsTouch(const void* p) {
  uintptr_t line = (uintptr_t)p >> 5;
  int set = line & (kSets - 1);
  ++touches;
  ++tick;
  int lru = 0;
  for (int w = 0; w < kWays; ++w) {
    if (tags[set][w] == line + 1) {
      ages[set][w] = tick;
      return;
    }
    if (ages[set][w] < ages[set][lru]) lru = w;
  }
  ++misses;
  tags[set][lru] = line + 1;
  ages[set][lru] = tick;
}
}  // namespace rytm
#endif

extern "C" {

#ifdef RYTM_SAMPLE_OPS
void sm_cache_reset() {
  for (int s = 0; s < kSets; ++s)
    for (int w = 0; w < kWays; ++w) tags[s][w] = 0, ages[s][w] = 0;
  touches = misses = 0;
  tick = 0;
}
void sm_cache_stats(unsigned long long* out) {
  out[0] = touches;
  out[1] = misses;
}
#endif
int sm_active_grains() { return rytm::SampleModes::ActiveGrains(); }

void* sm_new(unsigned seed) {
  rytm::SampleModes* e = new (std::nothrow) rytm::SampleModes();
  if (e) e->Init(seed);
  return e;
}
void sm_free(void* p) { delete static_cast<rytm::SampleModes*>(p); }
void sm_attach_pluck(void* p, float* line) { static_cast<rytm::SampleModes*>(p)->AttachPluck(line); }
// pcm: frames + 1 values (the guard), kept alive by the caller.
void sm_set(void* p, const int16_t* pcm, unsigned frames, float rate) {
  static_cast<rytm::SampleModes*>(p)->Set(pcm, frames, rate);
}
void sm_trigger(void* p, float note, const float* sp) { static_cast<rytm::SampleModes*>(p)->Trigger(note, sp); }
void sm_note_off(void* p) { static_cast<rytm::SampleModes*>(p)->NoteOff(); }
void sm_stop(void* p) { static_cast<rytm::SampleModes*>(p)->Stop(); }
// Renders in 12-sample blocks, as the voices run (knobs read per block).
void sm_render(void* p, const float* sp, float* out, int n) {
  rytm::SampleModes* e = static_cast<rytm::SampleModes*>(p);
  for (int i = 0; i < n; i += 12) e->Render(sp, out + i, n - i < 12 ? n - i : 12);
}
int sm_playing(void* p) { return static_cast<rytm::SampleModes*>(p)->playing(); }
int sm_mode(void* p) { return static_cast<rytm::SampleModes*>(p)->mode(); }
void sm_ops(void* p, unsigned* out) {
  const rytm::SampleModes::Ops& o = static_cast<rytm::SampleModes*>(p)->ops;
  out[0] = o.reads;
  out[1] = o.grains;
}
int sm_pluck_len() { return rytm::SampleModes::kPluckLen; }
int sm_path(int psram, int resident, int mode) { return rytm::ChoosePath(psram, resident, mode); }

void* sr_new(unsigned bytes, unsigned align) {
  rytm::SampleRam* r = new (std::nothrow) rytm::SampleRam();
  if (r) r->Init(nullptr, bytes, align);
  return r;
}
void sr_free(void* p) { delete static_cast<rytm::SampleRam*>(p); }
int sr_alloc(void* p, unsigned key, unsigned bytes, unsigned now, unsigned* evicted, int max_ev, int* nev,
             int may_evict) {
  return static_cast<rytm::SampleRam*>(p)->Alloc(key, bytes, now, evicted, max_ev, nev, may_evict != 0);
}
int sr_find(void* p, unsigned key) { return static_cast<rytm::SampleRam*>(p)->Find(key); }
void sr_release(void* p, int id) { static_cast<rytm::SampleRam*>(p)->Free(id); }
void sr_touch(void* p, int id, unsigned now) { static_cast<rytm::SampleRam*>(p)->Touch(id, now); }
void sr_pin(void* p, int id) { static_cast<rytm::SampleRam*>(p)->Pin(id); }
void sr_unpin(void* p, int id) { static_cast<rytm::SampleRam*>(p)->Unpin(id); }
// id's block: off, bytes; false if gone.
int sr_block(void* p, int id, unsigned* off, unsigned* bytes) {
  rytm::SampleRam::Block* b = static_cast<rytm::SampleRam*>(p)->Get(id);
  if (!b) return 0;
  *off = b->off;
  *bytes = b->bytes;
  return 1;
}
unsigned sr_used(void* p) { return static_cast<rytm::SampleRam*>(p)->used(); }
unsigned sr_largest_hole(void* p) { return static_cast<rytm::SampleRam*>(p)->largest_hole(); }
unsigned sr_evictions(void* p) { return static_cast<rytm::SampleRam*>(p)->evictions(); }
int sr_count(void* p) { return static_cast<rytm::SampleRam*>(p)->count(); }
}

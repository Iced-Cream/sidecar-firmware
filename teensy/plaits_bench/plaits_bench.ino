// Plaits on the Teensy 4.1: CPU cost per voice for each of the 24 models.
//
// For each model it renders 2 s of audio in 12-sample blocks (Plaits' block,
// 48 kHz), retriggering every 0.25 s and moving timbre/morph, and times every
// block with the cycle counter. 100 % = one voice taking all of a 600 MHz core in
// real time (12500 cycles per sample). Results over USB serial; send 'r' to run
// again. Needs libraries/Plaits (../setup_plaits_lib.sh).
#include <Arduino.h>
#include <new>

#include <Plaits.h>

static plaits::Voice voice;
static char shared[16384];
static plaits::Patch patch;
static plaits::Modulations mod;
static plaits::Voice::Frame frames[plaits::kMaxBlockSize];

static const char* const kNames[24] = {
    "VA VCF", "PHASE DIST", "6-OP FM 1", "6-OP FM 2", "6-OP FM 3", "WAVE TERRAIN",
    "STRING MACH", "CHIPTUNE",  // 8..23: the classic Plaits models
    "VA", "WAVESHAPE", "2-OP FM", "GRANULAR", "HARMONIC", "WAVETABLE", "CHORDS",
    "SPEECH", "SWARM", "NOISE", "PARTICLE", "STRING", "MODAL", "BASS DRUM",
    "SNARE", "HI-HAT"};

static void run() {
  const uint32_t per_sample = F_CPU_ACTUAL / 48000;
  const int blocks = 2 * 48000 / plaits::kBlockSize;
  float total_avg = 0, worst_avg = 0;
  Serial.printf("Plaits on Teensy 4.1 @ %lu MHz, block %d, 48 kHz\n",
                (unsigned long)(F_CPU_ACTUAL / 1000000), (int)plaits::kBlockSize);
  Serial.println(" #  model         avg %   max %   voices (avg)");
  for (int e = 0; e < 24; ++e) {
    patch.engine = e;
    uint64_t sum = 0;
    uint32_t peak = 0;
    for (int b = 0; b < blocks; ++b) {
      mod.trigger = (b % 1000 == 0) ? 1.0f : 0.0f;   // every 0.25 s
      patch.timbre = 0.5f + 0.4f * sinf(b * 0.001f);
      patch.morph = 0.5f + 0.4f * cosf(b * 0.0013f);
      uint32_t t0 = ARM_DWT_CYCCNT;
      voice.Render(patch, mod, frames, plaits::kBlockSize);
      uint32_t dt = ARM_DWT_CYCCNT - t0;
      sum += dt;
      if (dt > peak) peak = dt;
    }
    float avg = 100.0f * (float)sum / blocks / plaits::kBlockSize / per_sample;
    float mx = 100.0f * peak / plaits::kBlockSize / per_sample;
    total_avg += avg;
    if (avg > worst_avg) worst_avg = avg;
    Serial.printf("%2d  %-12s %6.2f  %6.2f   %5.1f\n", e, kNames[e], avg, mx, 100.0f / avg);
  }
  Serial.printf("mean %.2f %% per voice; worst model %.2f %% -> %.1f voices of the worst\n",
                total_avg / 24, worst_avg, 100.0f / worst_avg);
  Serial.printf("RAM per voice: %u bytes (Voice) + %u (shared buffer)\n",
                (unsigned)sizeof(voice), (unsigned)sizeof(shared));
}

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 8000) {}
  ARM_DEMCR |= ARM_DEMCR_TRCENA;
  ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
  stmlib::BufferAllocator allocator(shared, sizeof(shared));
  voice.Init(&allocator);
  patch.note = 48.0f;
  patch.harmonics = 0.5f;
  patch.timbre = 0.5f;
  patch.morph = 0.5f;
  patch.frequency_modulation_amount = 0.0f;
  patch.timbre_modulation_amount = 0.0f;
  patch.morph_modulation_amount = 0.0f;
  patch.decay = 0.5f;
  patch.lpg_colour = 0.5f;
  mod.trigger_patched = true;
  mod.level_patched = false;
  run();
}

void loop() {
  if (Serial.available() && Serial.read() == 'r') run();
}

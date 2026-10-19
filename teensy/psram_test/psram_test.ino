// PSRAM check for the sample RAM (docs/psram.md, "Monday"): run once the two
// APS6404L chips are soldered on the Teensy 4.1's bottom pads.
//
//   teensy/run_sketch.sh psram_test 120
//
// Over USB serial it reports:
//  - the PSRAM the core found at boot (external_psram_size, MB; 16 expected)
//    and the FlexSPI2 clock (the core's default 105.6 MHz)
//  - a memory test over all of it: fixed patterns (0x55.., 0xAA.., 0, ~0),
//    each word's own address, and three pseudo-random fills; every word
//    written, the cache flushed and dropped, every word read back. Errors are
//    counted and the first few printed (address, wrote, read). A bad joint
//    usually shows as one chip (0x7000_0000.. or 0x7080_0000..) or one data
//    bit failing everywhere.
//  - bandwidth through the cache: sequential reads and writes (MB/s), a
//    random 32-byte line's read (the cost of a cache miss: samplebench.py's
//    T_MISS), and the sample engine's access patterns over eight 1 MB
//    "samples": 8 voices x 4 grains (the grain budget) and 8 x 8, 8 voices
//    of WAVE cycles (600 and 2048 frames), and 8 one-shots - each as % of
//    the Teensy, PSRAM against the same loop over internal RAM.
// Send 'r' to run again. Writes PSRAM only (nothing else is in it).
#include <Arduino.h>

extern "C" uint8_t external_psram_size;

namespace {

uint32_t* const kBase = (uint32_t*)0x70000000;
uint32_t words = 0;   // 32-bit words of PSRAM

inline uint32_t xs(uint32_t x) {   // xorshift32
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return x;
}

float flexspi2_mhz() {
  static const float sel[4] = {396.0f, 720.0f, 664.62f, 528.0f};   // CCM_CBCMR FLEXSPI2_CLK_SEL
  uint32_t r = CCM_CBCMR;
  uint32_t s = (r >> 8) & 3, podf = (r >> 29) & 7;
  return sel[s] / (podf + 1);
}

void flush_all() { arm_dcache_flush_delete(kBase, words * 4); }

// One pass: write f(i) to every word, flush, read back. Errors returned.
template <typename F>
uint32_t pass(const char* name, F f) {
  uint32_t t0 = millis();
  for (uint32_t i = 0; i < words; ++i) kBase[i] = f(i);
  flush_all();
  uint32_t t1 = millis();
  uint32_t errs = 0;
  for (uint32_t i = 0; i < words; ++i) {
    uint32_t want = f(i), got = kBase[i];
    if (got != want) {
      if (errs < 6)
        Serial.printf("    error at 0x%08lX: wrote %08lX read %08lX (xor %08lX)\n", (unsigned long)(uint32_t)&kBase[i],
                      (unsigned long)want, (unsigned long)got, (unsigned long)(want ^ got));
      ++errs;
    }
  }
  Serial.printf("  %-14s %s (%lu errors; write %lu ms, read %lu ms)\n", name, errs ? "FAIL" : "ok",
                (unsigned long)errs, (unsigned long)(t1 - t0), (unsigned long)(millis() - t1));
  return errs;
}

uint32_t memtest() {
  Serial.printf("memory test, %lu MB:\n", (unsigned long)external_psram_size);
  uint32_t e = 0;
  e += pass("0x55555555", [](uint32_t) { return 0x55555555u; });
  e += pass("0xAAAAAAAA", [](uint32_t) { return 0xAAAAAAAAu; });
  e += pass("0x00000000", [](uint32_t) { return 0u; });
  e += pass("0xFFFFFFFF", [](uint32_t) { return 0xFFFFFFFFu; });
  e += pass("address", [](uint32_t i) { return (uint32_t)&kBase[i]; });
  static const uint32_t seeds[3] = {0x2545F491u, 0x9E3779B9u, 0x12345678u};
  for (uint32_t s : seeds) {
    char n[20];
    snprintf(n, sizeof n, "random %08lX", (unsigned long)s);
    e += pass(n, [s](uint32_t i) { return xs(s ^ (i * 0x9E3779B1u)) ^ i; });
  }
  Serial.printf("memory test: %s (%lu errors)\n", e ? "FAILED" : "passed", (unsigned long)e);
  return e;
}

float mbps(uint32_t bytes, uint32_t cycles) { return bytes / (cycles / (float)F_CPU_ACTUAL) / 1e6f; }

void bandwidth() {
  Serial.printf("bandwidth (FlexSPI2 %.1f MHz, CPU %lu MHz):\n", flexspi2_mhz(), (unsigned long)(F_CPU_ACTUAL / 1000000));
  const uint32_t n = 1u << 20;   // 1 MB
  volatile uint32_t sink = 0;
  // sequential read
  flush_all();
  uint32_t t = ARM_DWT_CYCCNT, s = 0;
  for (uint32_t i = 0; i < n / 4; ++i) s += kBase[i];
  t = ARM_DWT_CYCCNT - t;
  sink = s;
  Serial.printf("  sequential read     %6.1f MB/s\n", mbps(n, t));
  // sequential write (and the write-back)
  t = ARM_DWT_CYCCNT;
  for (uint32_t i = 0; i < n / 4; ++i) kBase[i] = i;
  arm_dcache_flush(kBase, n);
  t = ARM_DWT_CYCCNT - t;
  Serial.printf("  sequential write    %6.1f MB/s\n", mbps(n, t));
  // memcpy PSRAM -> RAM, 8 KB at a time (as a load's conversion reads it back)
  static uint8_t buf[8192];
  flush_all();
  t = ARM_DWT_CYCCNT;
  for (uint32_t o = 0; o < n; o += sizeof buf) memcpy(buf, (uint8_t*)kBase + o, sizeof buf);
  t = ARM_DWT_CYCCNT - t;
  Serial.printf("  memcpy to RAM       %6.1f MB/s\n", mbps(n, t));
  // a random line: dependent reads (each address from the last value), so
  // the misses do not overlap - the latency of one
  flush_all();
  const uint32_t lines = words / 8, reps = 100000;
  uint32_t x = 1;
  t = ARM_DWT_CYCCNT;
  for (uint32_t i = 0; i < reps; ++i) x = xs(x + kBase[(x % lines) * 8]);
  t = ARM_DWT_CYCCNT - t;
  sink = x;
  float miss_ns = t / (float)reps / (F_CPU_ACTUAL / 1e9f);
  Serial.printf("  random line read    %6.0f ns (%lu cycles): samplebench.py's T_MISS\n", miss_ns,
                (unsigned long)(t / reps));
  (void)sink;
}

// The sample engine's reads (sample_modes.h): per 12-sample block, voice after
// voice, each voice's streams sample by sample - int16 frames, two a read
// (interpolation), each stream moving `step` frames a sample. Each voice its
// own 1 MB "sample" (8 MB of PSRAM, or a 64 KB stand-in in RAM, wrapping).
DMAMEM int16_t ram_samples[8][4096];   // 64 KB of RAM2: the same loop without PSRAM

uint32_t pattern(const int16_t* const* base, uint32_t frames, int voices, int streams, float step, uint32_t wave,
                 uint32_t secs_blocks) {
  float pos[8][8];
  uint32_t cyc[8][2];
  uint32_t seed = 7;
  for (int v = 0; v < voices; ++v)
    for (int g = 0; g < streams; ++g) {
      seed = xs(seed);
      pos[v][g] = (float)(seed % (frames / 2));
    }
  for (int v = 0; v < voices; ++v) {
    seed = xs(seed);
    cyc[v][0] = wave ? (seed % (frames / wave - 1)) * wave : 0;
    cyc[v][1] = cyc[v][0] + wave;
  }
  float ph[8] = {0};
  float acc = 0.0f;
  uint32_t t = ARM_DWT_CYCCNT;
  for (uint32_t b = 0; b < secs_blocks; ++b)
    for (int v = 0; v < voices; ++v) {
      const int16_t* p = base[v];
      for (int i = 0; i < 12; ++i) {
        if (wave) {   // WAVE: two adjacent cycles, phase at 261.6 Hz
          uint32_t j = (uint32_t)(ph[v] * wave), j1 = j + 1 < wave ? j + 1 : 0;
          acc += p[cyc[v][0] + j] + p[cyc[v][0] + j1] + p[cyc[v][1] + j] + p[cyc[v][1] + j1];
          ph[v] += 261.6f / 48000.0f;
          if (ph[v] >= 1.0f) ph[v] -= 1.0f;
          continue;
        }
        for (int g = 0; g < streams; ++g) {
          uint32_t k = (uint32_t)pos[v][g];
          acc += p[k] + p[k + 1];
          pos[v][g] += step;
          if (pos[v][g] >= frames - 2) pos[v][g] -= frames / 2;
        }
      }
    }
  t = ARM_DWT_CYCCNT - t;
  static volatile float sink;
  sink = acc;
  (void)sink;
  return t;
}

void patterns() {
  Serial.println("access patterns (8 voices; % of the Teensy for 1 s of audio; PSRAM vs RAM):");
  const int16_t* ps[8];
  const int16_t* rs[8];
  for (int v = 0; v < 8; ++v) {
    ps[v] = (const int16_t*)kBase + v * 512 * 1024 + v * 264;   // 1 MB apart, staggered cache sets
    rs[v] = ram_samples[v];
  }
  struct Case {
    const char* name;
    int streams;
    float step;
    uint32_t wave;
  } cases[] = {
      {"ONE x8", 1, 1.0f, 0},          {"ONE +12 x8", 1, 2.0f, 0},     {"GRAIN 4 grains x8", 4, 1.0f, 0},
      {"GRAIN 4 +12 x8", 4, 2.0f, 0},  {"GRAIN 8 grains x8", 8, 1.0f, 0}, {"WAVE 600 x8", 0, 0, 600},
      {"WAVE 1024 x8", 0, 0, 1024},    {"WAVE 2048 x8", 0, 0, 2048},
  };
  const uint32_t blocks = 48000 / 12;   // 1 s
  for (const Case& c : cases) {
    flush_all();
    uint32_t tp = pattern(ps, 500000, 8, c.streams, c.step, c.wave, blocks);
    // the same loop over internal RAM (8 x 8 KB, so WAVE 2048 runs on 1024-frame
    // cycles there: a stand-in; what matters is the difference)
    uint32_t tram = pattern(rs, 4096, 8, c.streams, c.step, c.wave > 1024 ? 1024 : c.wave, blocks);
    float pp = 100.0f * tp / F_CPU_ACTUAL, pr = 100.0f * tram / F_CPU_ACTUAL;
    Serial.printf("  %-18s PSRAM %5.1f %%  RAM %5.1f %%  stall %5.1f %%\n", c.name, pp, pr, pp - pr);
  }
}

void run() {
  Serial.printf("\nPSRAM: %lu MB (external_psram_size)\n", (unsigned long)external_psram_size);
  if (!external_psram_size) {
    Serial.println("no PSRAM found: nothing to test (the chips, their joints, or the 3.3 V pad?)");
    return;
  }
  words = (uint32_t)external_psram_size << 18;
  memtest();
  bandwidth();
  patterns();
  Serial.println("done; 'r' runs again");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) {}
  ARM_DEMCR |= ARM_DEMCR_TRCENA;
  ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
  memset(ram_samples, 0, sizeof ram_samples);
  run();
}

void loop() {
  if (Serial.available() && Serial.read() == 'r') run();
}

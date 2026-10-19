// The DAISYSP voice on the Teensy 4.1: CPU cost per voice for each model, as
// plaits_bench does for Plaits. 2 s per model in 12-sample blocks at 48 kHz,
// retriggering every 0.25 s with brightness and structure moving; 100 % = one
// voice taking all of the core in real time. Send 'r' to run again. Needs
// libraries/DaisySP (../setup_daisysp_lib.sh).
#include <Arduino.h>

#include <DaisySP.h>

static rytm::DaisyEngine engine;
static const char* const kNames[3] = {"MODAL", "STRING", "RESON"};

static void run() {
  const uint32_t per_sample = F_CPU_ACTUAL / 48000;
  const int kBlock = 12, blocks = 2 * 48000 / kBlock;
  float out[kBlock];
  Serial.printf("DAISYSP on Teensy 4.1 @ %lu MHz, block %d, 48 kHz\n",
                (unsigned long)(F_CPU_ACTUAL / 1000000), kBlock);
  Serial.println(" #  model    avg %   max %   voices (avg)");
  for (int m = 0; m < 3; ++m) {
    engine.Init(48000.0f);
    uint64_t sum = 0;
    uint32_t peak = 0;
    for (int b = 0; b < blocks; ++b) {
      bool trig = b % 1000 == 0;   // every 0.25 s
      float bri = 0.5f + 0.4f * sinf(b * 0.001f), str = 0.5f + 0.4f * cosf(b * 0.0013f);
      uint32_t t0 = ARM_DWT_CYCCNT;
      engine.Render(m, 48.0f, str, bri, 0.3f, 0.7f, 0.25f, trig, 0.8f, out, kBlock);
      uint32_t dt = ARM_DWT_CYCCNT - t0;
      sum += dt;
      if (dt > peak) peak = dt;
    }
    float avg = 100.0f * (float)sum / blocks / kBlock / per_sample;
    float mx = 100.0f * peak / kBlock / per_sample;
    Serial.printf("%2d  %-7s %6.2f  %6.2f   %5.1f\n", m, kNames[m], avg, mx, 100.0f / avg);
  }
  Serial.printf("RAM per voice: %u bytes\n", (unsigned)sizeof(engine));
}

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 8000) {}
  ARM_DEMCR |= ARM_DEMCR_TRCENA;
  ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
  run();
}

void loop() {
  if (Serial.available() && Serial.read() == 'r') run();
}

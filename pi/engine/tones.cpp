// pi/engine/tones.cpp - the Sidecar's UAC2 test tables (sidecar_gadget.h, uac2test=on), hosted (newlib's libm):
// round(amp * 8388607 * sin(2 pi i / period)) in double, the integers teensy/uac2/uac2_test.py checks (as the
// Teensy's uac2_probe computes them; measured exact there).
#include <math.h>
#include <stdint.h>

void sidecar_tone_table(int period, double amp, int32_t* out) {
  for (int i = 0; i < period; i++) out[i] = (int32_t)lround(amp * 8388607.0 * sin(2.0 * M_PI * i / period));
}

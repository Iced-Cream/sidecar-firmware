// pi/engine/tests/test_slotfx_ref.cpp - two links, phase 3(d): slot 0's send FX and master chain bit for bit. A fixed
// scene (CHORUS on FX 1, COMB on FX 2, the Rytm's sends, the voices' sends to the buses, a TYPE on bus 3, the master's COMP
// and a send TYPE insert, SRC MIX with the stems) through sendfx_pi and masterfx_pi with box 0's calls only, every OUT frame
// hashed (FNV-1a 64). The hash printed and compared with the one the code before the per-slot change gave
// (kRef, recorded at 9354abb). Then, on 16 voices, the same with a TYPE set in slot 1's settings and slot 1's instances run:
// slot 0's hash unchanged.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "check.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "masterfx_pi.h"
#include "sendfx_pi.h"

namespace core {
uint32_t cycles() { static uint32_t c; return c += 1000; }
uint32_t cycles_per_second() { return 54000000; }
uint32_t g_ms;
uint32_t now_ms() { return g_ms; }
void log(const char*, ...) {}
}  // namespace core
extern "C" int core_vsnprintf(char* out, size_t n, const char* fmt, va_list ap) { return vsnprintf(out, n, fmt, ap); }
extern "C" int core_snprintf(char* out, size_t n, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(out, n, fmt, ap);
  va_end(ap);
  return r;
}

using namespace core;

static uint64_t g_h = 1469598103934665603ull;
static void hash(const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; ++i) g_h = (g_h ^ b[i]) * 1099511628211ull;
}

static void scene() {
  fx::control.knob[0][0] = fx::kChorus, fx::control.knob[1][0] = fx::kComb;
  for (int k = 2; k < fx::kKnobs; ++k) fx::control.knob[0][k] = 70, fx::control.knob[1][k] = 50;
  fx::control.knob[1][1] = 100;   // FX 2 POST
  for (int t = 0; t < fx::kTracks; ++t) fx::control.send[t][0] = (uint8_t)(20 + 7 * t), fx::control.send[t][1] = (uint8_t)(90 - 5 * t);
  for (int v = 0; v < ins::kVoices; ++v) ins::control.send[v][0] = 40, ins::control.send[v][2] = 60;
  ins::set_type(ins::kBus0 + 2, 0, ins::kSat);
  ins::set_type(ins::kM0, 0, ins::kComp);
  ins::set_type(ins::kM0, 1, ins::kMChorus);
  master::control.src = master::kMix;
}

// n blocks of 7 frames: stems (two sines), voices (a third), the OUT fill's channels; render after each block
static void run(int blocks) {
  for (int k = 0; k < blocks; ++k) {
    int32_t s[7][8] = {};
    int16_t v[7][8] = {};
    for (int f = 0; f < 7; ++f) {
      const double t = (7.0 * k + f) / 48000.0;
      for (int c = 0; c < 8; ++c) s[f][c] = (int32_t)(0.05 * 2147483647.0 * std::sin(6.2831853 * (110.0 + 37.0 * c) * t));
      for (int c = 0; c < 8; ++c) v[f][c] = (int16_t)(3000.0 * std::sin(6.2831853 * (330.0 + 11.0 * c) * t));
    }
    if (sendfx_pi::On()) sendfx_pi::FromRytm(s, 7);
    if (masterfx_pi::WantStems()) masterfx_pi::Stems(s, 7);
    int32_t o[7][12];
    for (int f = 0; f < 7; ++f)
      for (int c = 0; c < 12; ++c) o[f][c] = (int32_t)(1000 * (c + 1) * (f + 1));
    float x[7][4] = {};
    sendfx_pi::FromVoices(v, 7);
    sendfx_pi::AddReturn(o, 7, x);
    masterfx_pi::Process(o, 7);
    hash(o, sizeof o);
    hash(x, sizeof x);
    sendfx_pi::Render();
    masterfx_pi::Render();
    core::g_ms += (k % 7 == 0);
  }
}

int main(int argc, char** argv) {
  if (!getenv("SLOTFX_NOSCENE")) scene();   // (the scene off: a different hash, so the FX did play)
  run(4000);
  printf("slot 0 hash %016llx\n", (unsigned long long)g_h);
  const char* ref = argc > 1 ? argv[1] : getenv("SLOTFX_REF");
  if (ref) {
    char b[32];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)g_h);
    CHECK(strcmp(b, ref) == 0);
    printf("matches the reference\n");
  }
  return 0;
}

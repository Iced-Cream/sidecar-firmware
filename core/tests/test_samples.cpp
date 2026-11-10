// core/tests/test_samples.cpp - the sample service (core/samples) and the library's SAMPLE block (core/dsp/sample.h):
// 51 STORE's SADD / SDROP / SINFO through the engine (gen::sysex, the card ops run by host_step: no card core here), WAVs
// of each format read into RAM, the project's pool list kept on the card and read back when the project returns, and
// SAMPLE playing an entry: one shot, loop, backwards, OFF, a missing entry.
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "dsp/dsp.h"
#include "dsp/sample.h"
#include "generic/generic.h"
#include "hooks.h"
#include "kv/kv.h"
#include "kv/link.h"
#include "platform.h"
#include "samples/samples.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static char g_card[64];
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void send(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, kv::kCmd, kv::kVer};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  gen::sysex(m.data(), (int)m.size());
  for (int i = 0; i < 64 && kv::link::host_step(8); ++i) {}
}
static void settle() {   // the card work, past the retire wait (kRetireMs) too
  for (int i = 0; i < 400; ++i) {
    kv::link::host_step(8);
    if (i % 4 == 0) usleep(1000);
  }
}
static void project(uint32_t p) {
  send({kv::kOpName, (uint8_t)(p >> 28 & 0x0F), (uint8_t)(p >> 21 & 0x7F), (uint8_t)(p >> 14 & 0x7F), (uint8_t)(p >> 7 & 0x7F),
        (uint8_t)(p & 0x7F), 4, 'T', 'E', 'S', 'T'});
}
static std::vector<uint8_t> add(uint8_t seq, const char* path) {
  std::vector<uint8_t> b = {samples::kAdd, seq, 0x45};
  for (const char* s = path; *s; ++s) b.push_back((uint8_t)*s);
  g_out.clear();
  send(b);
  for (auto& m : g_out) if (m.size() > 10 && m[6] == samples::kIndex && m[7] == seq) return {m[9], m[10]};
  return {};
}

// a WAV on the test card
static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int k = 0; k < 4; ++k) v.push_back((uint8_t)(x >> 8 * k)); }
static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
static void wav(const char* name, int fmt, int bits, int ch, uint32_t rate, const std::vector<float>& frames_lr) {
  std::vector<uint8_t> d;
  for (float x : frames_lr) {
    if (fmt == 3) { uint32_t u; memcpy(&u, &x, 4); put32(d, u); continue; }
    int32_t q = (int32_t)lrintf(x * 8388607.0f);   // 24-bit full scale
    if (bits == 16) put16(d, (uint16_t)(int16_t)(q >> 8));
    else if (bits == 24) { d.push_back((uint8_t)q); d.push_back((uint8_t)(q >> 8)); d.push_back((uint8_t)(q >> 16)); }
    else if (bits == 8) d.push_back((uint8_t)((q >> 16) + 128));
    else put32(d, (uint32_t)(q << 8));
  }
  std::vector<uint8_t> f = {'R', 'I', 'F', 'F'};
  put32(f, 4 + 8 + 16 + 8 + 12 + 8 + (uint32_t)d.size());
  f.insert(f.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(f, 16);
  put16(f, (uint16_t)fmt); put16(f, (uint16_t)ch); put32(f, rate); put32(f, rate * ch * bits / 8);
  put16(f, (uint16_t)(ch * bits / 8)); put16(f, (uint16_t)bits);
  f.insert(f.end(), {'L', 'I', 'S', 'T'});   // a chunk to step over
  put32(f, 4);
  f.insert(f.end(), {'I', 'N', 'F', 'O'});
  f.insert(f.end(), {'d', 'a', 't', 'a'});
  put32(f, (uint32_t)d.size());
  f.insert(f.end(), d.begin(), d.end());
  char p[200];
  snprintf(p, sizeof p, "%s/Samples", g_card);
  mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/Samples/%s", g_card, name);
  FILE* o = fopen(p, "wb");
  fwrite(f.data(), 1, f.size(), o);
  fclose(o);
}

// SAMPLE rendering: n frames of L, R
static float g_l[4096], g_r[4096];
static void render(dsp::Sample& s, int n) {
  proc::Block b{};
  b.frames = 0;
  for (int done = 0; done < n; done += 32) {
    b.sout[0] = g_l + done;
    b.sout[1] = g_r + done;
    b.frames = 32;
    s.process(b);
  }
}
static float peak(const float* x, int a, int b) {
  float m = 0.0f;
  for (int i = a; i < b; ++i) m = fmaxf(m, fabsf(x[i]));
  return m;
}

int main() {
  snprintf(g_card, sizeof g_card, "/tmp/core-samples-XXXXXX");
  CHECK(mkdtemp(g_card));
  host_set_card(g_card);
  Fs fs;
  CHECK(fs.begin());
  hooks.to_rytm = out;
  CHECK(kv::link::attach_generic());
  static uint8_t arena[1 << 20];
  CHECK(samples::begin(arena + 3, sizeof arena - 3));

  // the files: a 16-bit mono ramp at 48 kHz (C4 = one frame a frame), 24-bit stereo, float, 8-bit, not a WAV
  std::vector<float> ramp, st, flt, b8;
  for (int i = 0; i < 2000; ++i) ramp.push_back(0.25f + 0.5f * (float)i / 2000.0f);
  for (int i = 0; i < 1000; ++i) { st.push_back(0.5f); st.push_back(-0.25f); }
  for (int i = 0; i < 500; ++i) flt.push_back(i & 1 ? 0.75f : -0.75f);
  for (int i = 0; i < 300; ++i) b8.push_back(0.5f);
  wav("ramp.wav", 1, 16, 1, 48000, ramp);
  wav("st24.wav", 1, 24, 2, 44100, st);
  wav("flt.wav", 3, 32, 1, 48000, flt);
  wav("b8.wav", 1, 8, 1, 22050, b8);
  {
    char p[200];
    snprintf(p, sizeof p, "%s/Samples/junk.wav", g_card);
    FILE* o = fopen(p, "wb");
    fputs("not a wave file at all", o);
    fclose(o);
  }

  project(0x1234567);
  // SADD: indices in order, the same path again its own index, the refusals
  CHECK((add(1, "/Samples/ramp.wav") == std::vector<uint8_t>{0, samples::kOk}));
  CHECK((add(2, "/Samples/st24.wav") == std::vector<uint8_t>{1, samples::kOk}));
  CHECK((add(3, "/Samples/ramp.wav") == std::vector<uint8_t>{0, samples::kOk}));
  CHECK((add(4, "/Samples/flt.wav") == std::vector<uint8_t>{2, samples::kOk}));
  CHECK((add(5, "/Samples/b8.wav") == std::vector<uint8_t>{3, samples::kOk}));
  CHECK((add(6, "/Samples/junk.wav") == std::vector<uint8_t>{4, samples::kOk}));
  CHECK((add(7, "/Samples/none.wav") == std::vector<uint8_t>{0x7F, samples::kMissing}));
  CHECK((add(8, "Samples/ramp.wav") == std::vector<uint8_t>{0x7F, samples::kBad}));
  CHECK((add(9, "/Samples/../x.wav") == std::vector<uint8_t>{0x7F, samples::kBad}));
  settle();
  CHECK(samples::state(0) == samples::kReady && samples::state(1) == samples::kReady && samples::state(2) == samples::kReady);
  CHECK(samples::state(3) == samples::kReady && samples::state(4) == samples::kFailed);
  samples::View v;
  CHECK(samples::get(0, &v) && v.frames == 2000 && v.ch == 1 && v.rate == 48000.0f);
  CHECK(abs(v.pcm[0] - 8192) <= 2 && abs(v.pcm[1999] - 24568) <= 4);
  CHECK(samples::get(1, &v) && v.frames == 1000 && v.ch == 2 && v.rate == 44100.0f);
  CHECK(abs(v.pcm[0] - 16384) <= 2 && abs(v.pcm[1] + 8192) <= 2);
  CHECK(samples::get(2, &v) && v.frames == 500 && abs(v.pcm[0] + 24575) <= 2 && abs(v.pcm[1] - 24575) <= 2);
  CHECK(samples::get(3, &v) && v.frames == 300 && abs(v.pcm[0] - 16384) <= 256);
  CHECK(!samples::get(4, &v) && !samples::get(5, &v));
  // SINFO
  g_out.clear();
  send({samples::kInfo, 10, 0x45, 1});
  CHECK(g_out.size() == 1 && g_out[0][6] == samples::kName && g_out[0][9] == 1 && g_out[0][10] == samples::kReady);
  CHECK(g_out.size() == 1 && (g_out[0][11] << 21 | g_out[0][12] << 14 | g_out[0][13] << 7 | g_out[0][14]) == 1000);
  CHECK(g_out.size() == 1 && std::string((const char*)&g_out[0][15], g_out[0].size() - 16) == "st24.wav");

  // SAMPLE: SMP 1 (entry 0, the ramp), C4: frame for frame
  {
    dsp::Sample s;
    s.set_param(1, 1 * 64);
    render(s, 64);
    CHECK(peak(g_l, 0, 64) == 0.0f);                  // nothing before a note
    s.midi(proc::kSrcRytm, 0x90, 60, 100);
    render(s, 2048);
    CHECK(fabsf(g_l[100] - 0.25f - 0.5f * 100 / 2000.0f) < 0.002f && g_l[100] == g_r[100]);
    CHECK(g_l[10] < g_l[100]);                         // the fade-in
    CHECK(peak(g_l, 2000, 2048) == 0.0f);             // one shot: done
    // loop STA 0 .. END 64 (half the ramp): still sounding after the length
    s.set_param(4, 64 * 64);
    s.set_param(5, 16383);
    s.midi(proc::kSrcRytm, 0x90, 60, 100);
    render(s, 4096);
    CHECK(peak(g_l, 3000, 4096) > 0.25f && peak(g_l, 3000, 4096) < 0.51f);
    // backwards: STA 127, END 0: starts high, falls
    s.set_param(3, 16383);
    s.set_param(4, 0);
    s.set_param(5, 0);
    s.midi(proc::kSrcRytm, 0x90, 60, 100);
    render(s, 2048);
    CHECK(g_l[200] > g_l[1000] && g_l[1000] > g_l[1800] && g_l[200] > 0.6f);
    // an octave up: twice as fast, done by ~1000
    s.set_param(3, 0);
    s.set_param(4, 16383);
    s.midi(proc::kSrcRytm, 0x90, 72, 100);
    render(s, 2048);
    CHECK(peak(g_l, 1010, 2048) == 0.0f && peak(g_l, 100, 900) > 0.3f);
    // OFF and a missing entry: silence
    s.set_param(1, 0);
    s.midi(proc::kSrcRytm, 0x90, 60, 100);
    render(s, 256);
    CHECK(peak(g_l, 0, 256) == 0.0f);
    s.set_param(1, 6 * 64);
    s.midi(proc::kSrcRytm, 0x90, 60, 100);
    render(s, 256);
    CHECK(peak(g_l, 0, 256) == 0.0f);
    // stereo (entry 1): L and R its own; LEV 50 halves it
    s.set_param(1, 2 * 64);
    s.set_param(0, 50 * 64);
    s.midi(proc::kSrcRytm, 0x90, 60, 100);
    render(s, 512);
    CHECK(fabsf(g_l[300] - 0.25f) < 0.01f && fabsf(g_r[300] + 0.125f) < 0.01f);
  }
  // the library's own entry
  CHECK(dsp::find("SAMPLE") && dsp::find("SAMPLE")->nparams == 7);

  // the list on the card; another project has none; back, the entries come again by index
  char lp[200];
  snprintf(lp, sizeof lp, "%s/.rytm/samples/01234567.lst", g_card);
  struct stat sb;
  CHECK(stat(lp, &sb) == 0 && sb.st_size > 20);
  project(0x0ABCDEF);
  settle();
  CHECK(samples::state(0) == samples::kEmpty && !samples::get(0, &v));
  CHECK((add(11, "/Samples/flt.wav") == std::vector<uint8_t>{0, samples::kOk}));
  settle();
  CHECK(samples::get(0, &v) && v.frames == 500);
  project(0x1234567);
  settle();
  CHECK(samples::state(0) == samples::kReady && samples::state(2) == samples::kReady && samples::state(4) == samples::kFailed);
  CHECK(samples::get(0, &v) && v.frames == 2000 && !strcmp(samples::path(1), "/Samples/st24.wav"));
  // SDROP: gone, the list without it; its index is taken by the next add
  send({samples::kDrop, 0x45, 1});
  CHECK(samples::state(1) == samples::kEmpty);
  CHECK((add(12, "/Samples/b8.wav") == std::vector<uint8_t>{3, samples::kOk}));
  CHECK((add(13, "/Samples/st24.wav") == std::vector<uint8_t>{1, samples::kOk}));
  settle();
  CHECK(samples::get(1, &v) && v.ch == 2);

  // a file bigger than what is left: TOO BIG, the others still load
  samples::reset_for_tests();
  CHECK(samples::begin(arena, 6000));
  project(0x777);
  settle();
  CHECK((add(14, "/Samples/ramp.wav") == std::vector<uint8_t>{0, samples::kOk}));   // 4000 B
  CHECK((add(15, "/Samples/st24.wav") == std::vector<uint8_t>{1, samples::kOk}));   // 4000 B: no room
  CHECK((add(16, "/Samples/b8.wav") == std::vector<uint8_t>{2, samples::kOk}));     // 600 B
  settle();
  CHECK(samples::state(0) == samples::kReady && samples::state(1) == samples::kTooBig && samples::state(2) == samples::kReady);

  samples::Stats ss = samples::stats();
  printf("test_samples: PASS (%u loads, %u fails, %d listed)\n", ss.loads, ss.fails, ss.listed);
  return 0;
}

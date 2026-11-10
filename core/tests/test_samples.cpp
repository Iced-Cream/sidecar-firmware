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
#include "dsp/grain.h"
#include "dsp/wtosc.h"
#include "dsp/poly.h"
#include "wavetable/tables.h"
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
static std::vector<uint8_t> add_ref(uint8_t seq, int folder, int file) {   // 3D SADDB: a file as core/browse listed it
  g_out.clear();
  send({samples::kAddRef, seq, 0x45, (uint8_t)(folder >> 7), (uint8_t)(folder & 0x7F), (uint8_t)(file >> 7), (uint8_t)(file & 0x7F)});
  for (auto& m : g_out) if (m.size() > 10 && m[6] == samples::kIndex && m[7] == seq) return {m[9], m[10]};
  return {};
}

// a WAV on the test card
static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int k = 0; k < 4; ++k) v.push_back((uint8_t)(x >> 8 * k)); }
static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
static void wav(const char* name, int fmt, int bits, int ch, uint32_t rate, const std::vector<float>& frames_lr,
                const char* clm = nullptr) {
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
  if (clm) {   // Serum's: "<!>2048 ..." (padded to even)
    uint32_t n = (uint32_t)strlen(clm);
    f.insert(f.end(), {'c', 'l', 'm', ' '});
    put32(f, n);
    f.insert(f.end(), clm, clm + n);
    if (n & 1) f.push_back(0);
    uint32_t riff = (uint32_t)f.size() - 8 + 8 + (uint32_t)d.size();
    for (int k = 0; k < 4; ++k) f[4 + k] = (uint8_t)(riff >> 8 * k);
  }
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
static void render_any(proc::Proc& s, int n) {
  proc::Block b{};
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

// upstream MULTISAMPLE (core/msamp/msamp.cpp on trigcondition/sidecar-firmware dev: start_region, play_key,
// release_voice, trig, render_impl's mono path, RAM only) for one voice: POLY {SAMPLE} with HKEY must match
namespace msref {
struct Note {
  bool on, held, relq, rel, sounded, one_shot;
  uint8_t key, entry, mode, stage;
  uint32_t end, loop_s, loop_e, att, hold, n, order;
  double pos;
  float inc, gain, dcoef, sus, rcoef, lvl;
};
constexpr int kPerVoice = 16, kLayers = 4;
Note notes[kPerVoice];
float p[16], head_g = 1.0f;
uint8_t key_vel[128];
uint32_t order_n = 0;
int inst = 0;
float tscale(float v) { return exp2f((v - 64.0f) / 16.0f); }
float coef(float secs) { float n = secs * 48000.0f; return n < 48 ? expf(-6.9078f / 48.0f) : expf(-6.9078f / n); }
int pick() {
  int b = -1;
  for (int k = 0; k < kPerVoice; ++k) if (!notes[k].on) return k;
  for (int k = 0; k < kPerVoice; ++k) if (!notes[k].held && (b < 0 || (int32_t)(notes[k].order - notes[b].order) < 0)) b = k;
  if (b < 0) for (int k = 0; k < kPerVoice; ++k) if (b < 0 || (int32_t)(notes[k].order - notes[b].order) < 0) b = k;
  return b;
}
void start_region(const samples::Layer& r, int key, int vel, float cg, bool rtrig) {
  samples::View m;
  if (!samples::get(r.entry, &m)) return;
  Note& x = notes[pick()];
  x = Note{};
  uint32_t e = r.end == samples::kNone || r.end + 1 > m.frames ? m.frames : r.end + 1;
  uint32_t off = r.offset < e ? r.offset : e - 1;
  float strt = p[10] < 0 ? 0 : p[10] > 127 ? 127 : p[10];
  x.pos = off + (double)(e - off) * (strt / 128.0);
  x.end = e;
  bool hl = m.has_loop || (r.iloop && r.iloop_e < m.frames);
  uint32_t mls = m.has_loop ? m.loop_s : r.iloop_s, mle = m.has_loop ? m.loop_e : r.iloop_e;
  uint32_t ls = r.loop_start != samples::kNone ? r.loop_start : hl ? mls : off;
  uint32_t le = r.loop_end != samples::kNone ? r.loop_end + 1 : hl ? mle + 1 : e;
  if (le > e) le = e;
  bool can_loop = le > ls + 1;
  int lz = dsp::vzone((int)(p[11] * 64), 3);
  uint8_t mode = r.loop_mode;
  if (mode == samples::kLoopAuto) mode = (hl || r.loop_start != samples::kNone || r.loop_end != samples::kNone) ? samples::kLoopContinuous : samples::kNoLoop;
  x.one_shot = mode == samples::kOneShot;
  if (lz == 1) mode = samples::kNoLoop;
  else if (lz == 2 && mode != samples::kLoopSustain) mode = samples::kLoopContinuous;
  x.mode = !can_loop ? 0 : mode == samples::kLoopContinuous ? 1 : mode == samples::kLoopSustain ? 2 : 0;
  x.loop_s = ls, x.loop_e = le;
  float semis = (key - r.keycenter) * r.pitch_keytrack / 100.0f + r.transpose + r.tune / 100.0f + (p[8] - 64) + (p[9] - 64) / 64.0f;
  x.inc = m.rate / 48000.0f * exp2f(semis / 12.0f);
  float vt = r.amp_veltrack / 100.0f * (p[1] < 0 ? 0 : p[1] > 127 ? 1 : p[1] / 127.0f);
  if (vt < 0) vt = 0;
  float v = (vel < 1 ? 1 : vel) / 127.0f;
  float vg = 1 - vt + vt * v * v;
  x.gain = powf(10.0f, r.volume / 20.0f) * r.amplitude / 100.0f * vg * (p[2] < 0 ? 0 : p[2] / 100.0f) * cg;
  float att = r.attack * tscale(p[4]), hold = r.hold, dec = r.decay * tscale(p[5]), rel = r.release * tscale(p[7]);
  float sus = r.sustain / 100.0f, sv = p[6];
  sus = sv <= 64 ? sus * sv / 64.0f : sus + (1 - sus) * (sv - 64) / 63.0f;
  x.att = (uint32_t)(att > 30 ? 30 * 48000 : att * 48000);
  x.hold = (uint32_t)(hold > 30 ? 30 * 48000 : hold * 48000);
  x.dcoef = dec <= 0 ? 0 : coef(dec);
  x.sus = sus < 0 ? 0 : sus > 1 ? 1 : sus;
  x.rcoef = coef(rel);
  x.on = true, x.held = !x.one_shot && !rtrig, x.key = (uint8_t)key, x.entry = r.entry, x.order = ++order_n;
}
void play_key(int key, int vel, int trig) {
  samples::Layer rs[kLayers];
  int n = samples::layers(inst, key, vel, trig, 0.5f, rs, kLayers);
  for (int i = 0; i < n; ++i) start_region(rs[i], key, vel, 1.0f, trig == 1);
}
void release_voice(bool rtrig) {
  uint8_t keys[kPerVoice];
  int nk = 0;
  for (Note& n : notes) {
    if (!n.held || !n.on) { n.held = false; continue; }
    n.held = false, n.relq = true;
    bool seen = false;
    for (int j = 0; j < nk; ++j) seen = seen || keys[j] == n.key;
    if (!seen) keys[nk++] = n.key;
  }
  if (rtrig) for (int j = 0; j < nk; ++j) play_key(keys[j], key_vel[keys[j]], 1);
}
void trig(int note, int vel) {
  release_voice(false);
  dsp::chord::Chord c{(int)p[12], (int)p[13], (int)p[14], (int)p[15]};
  uint8_t ks[8];
  int nk = dsp::chord::expand(note, c, ks);
  for (int i = 0; i < nk; ++i) { key_vel[ks[i]] = (uint8_t)vel; play_key(ks[i], vel, 0); }
}
float herm(float xm1, float x0, float x1, float x2, float t) {
  float c = (x1 - xm1) * 0.5f, w = x0 - x1, a = c + w + w + (x2 - x0) * 0.5f, b = c + w + a;
  return ((a * t - b) * t + c) * t + x0;
}
void render(float* dst, int nfr) {
  float acc[256] = {0}, wk[kPerVoice];
  uint8_t kk[kPerVoice];
  int nw = 0;
  for (Note& n : notes) {
    if (!n.on) continue;
    samples::View m;
    samples::get(n.entry, &m);
    if (!n.rel && !n.one_shot && n.sounded && n.relq) n.rel = true, n.stage = 4;
    const int ch = m.ch == 2 ? 2 : 1;
    int64_t end = n.end;
    bool fin = false;
    for (int s = 0; s < nfr; ++s) {
      bool looping = n.mode == 1 || (n.mode == 2 && !n.rel);
      int64_t i = (int64_t)n.pos;
      float t = (float)(n.pos - (double)i);
      int64_t len = (int64_t)n.loop_e - n.loop_s;
      float x[4];
      for (int j = 0; j < 4; ++j) {
        int64_t f = i - 1 + j;
        if (looping && f >= n.loop_e) f -= len;
        f = f < 0 ? 0 : f >= end ? end - 1 : f;
        const int16_t* q = m.pcm + (size_t)f * ch;
        x[j] = ch == 2 ? (float)(((int32_t)q[0] + (int32_t)q[1]) >> 1) : q[0];
      }
      switch (n.stage) {
        case 0: if (n.n >= n.att) { n.stage = 1, n.n = 0, n.lvl = 1; } else { n.lvl = (float)(n.n + 1) / (float)n.att; ++n.n; } break;
        case 1: if (n.n >= n.hold) n.stage = n.dcoef > 0 ? 2 : 3, n.lvl = n.dcoef > 0 ? 1 : n.sus; else ++n.n; break;
        case 2: n.lvl = n.sus + (n.lvl - n.sus) * n.dcoef; if (n.lvl - n.sus < 1e-4f) n.stage = 3, n.lvl = n.sus; break;
        case 3: n.lvl = n.sus; break;
        default: n.lvl *= n.rcoef; if (n.lvl < 1e-4f) fin = true; break;
      }
      if (fin) break;
      n.sounded = true;
      acc[s] += herm(x[0], x[1], x[2], x[3], t) * (1.0f / 32768.0f) * n.lvl * n.gain;
      n.pos += n.inc;
      if (looping && n.pos >= n.loop_e) n.pos -= (double)len;
      else if (n.pos >= (double)end) { fin = true; break; }
    }
    if (!fin) {
      int j = 0;
      while (j < nw && kk[j] != n.key) ++j;
      if (j == nw) kk[nw] = n.key, wk[nw++] = 0;
      if (n.lvl > wk[j]) wk[j] = n.lvl;
    }
    if (fin) n.on = false;
  }
  float e = 0;
  for (int j = 0; j < nw; ++j) e += wk[j];
  const float atk = 1.0f - expf(-1000.0f * nfr / (48000.0f * 3.0f)), rel = 1.0f - expf(-1000.0f * nfr / (48000.0f * 15.0f));
  float g0 = head_g, target = e > 1.0f ? 1.0f / sqrtf(e) : 1.0f;
  float g1 = g0 + (target - g0) * (target < g0 ? atk : rel);
  if (fabsf(g1 - target) < 1e-5f) g1 = target;
  head_g = g1;
  float dg = (g1 - g0) / (float)nfr;
  for (int s = 0; s < nfr; ++s) dst[s] = nw ? acc[s] * (g0 + dg * (float)(s + 1)) : 0.0f;
}
}  // namespace msref

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
  // SADDB: the root's files as a listing numbers them (folders first, then files by name, case ignored): Kick (a
  // folder), b8, flt, junk, a long name a listing cuts (found whole), ramp, st24
  {
    char p[200];
    snprintf(p, sizeof p, "%s/Samples/Kick", g_card);
    mkdir(p, 0755);
  }
  wav("long name past twenty-three chars.wav", 1, 16, 1, 48000, b8);
  CHECK((add_ref(20, 0, 4) == std::vector<uint8_t>{0, samples::kOk}));   // ramp: already entry 0
  CHECK((add_ref(21, 0, 3) == std::vector<uint8_t>{5, samples::kOk}));
  CHECK(!strcmp(samples::path(5), "/Samples/long name past twenty-three chars.wav"));
  CHECK((add_ref(22, 0, 7) == std::vector<uint8_t>{0x7F, samples::kMissing}));    // no 8th file
  CHECK((add_ref(23, 900, 0) == std::vector<uint8_t>{0x7F, samples::kMissing}));  // a folder never listed
  send({samples::kDrop, 0x45, 5});
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
    s.set_param(5, 1 * 64);
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
    // VEL 127: velocity 64 about halves it; VEL 0 (above) ignores velocity
    s.set_param(7, 16383);
    s.midi(proc::kSrcRytm, 0x90, 60, 64);
    render(s, 512);
    CHECK(fabsf(g_l[300] - 0.25f * 64.0f / 127.0f) < 0.01f);
  }
  // slices (SLN, SLC): the ramp in 4; a note picks the slice, not the pitch
  {
    dsp::Sample s;
    s.set_param(1, 1 * 64);
    s.set_param(8, 4 * 64);
    s.set_param(9, 1 * 64);
    s.midi(proc::kSrcRytm, 0x90, 60, 127);   // slice 1: frames 500..1000
    render(s, 1024);
    CHECK(fabsf(g_l[100] - (0.25f + 0.5f * 600 / 2000.0f)) < 0.003f && peak(g_l, 520, 1024) == 0.0f);
    s.midi(proc::kSrcRytm, 0x90, 62, 127);   // slice 3, at the same speed
    render(s, 1024);
    CHECK(fabsf(g_l[100] - (0.25f + 0.5f * 1600 / 2000.0f)) < 0.003f && peak(g_l, 520, 1024) == 0.0f);
    s.midi(proc::kSrcRytm, 0x90, 57, 127);   // 1 - 3 = slice 2 (modulo)
    render(s, 1024);
    CHECK(fabsf(g_l[100] - (0.25f + 0.5f * 1100 / 2000.0f)) < 0.003f);
    s.set_param(8, 0);                         // off: the whole ramp at the note's pitch again
    s.midi(proc::kSrcRytm, 0x90, 60, 127);
    render(s, 2048);
    CHECK(fabsf(g_l[100] - (0.25f + 0.5f * 100 / 2000.0f)) < 0.003f);
  }
  // an instrument (an .sfz, upstream's parser): its samples listed as entries of their own (ramp.wav found as entry 0),
  // zones by key and velocity, the key centre's pitch, volume, round robin; SAMPLE plays the zone a note falls in
  {
    std::vector<float> hi(1000, 0.5f), rr(800, 0.25f);
    wav("hi.wav", 1, 16, 1, 48000, hi);
    wav("rr.wav", 1, 16, 1, 48000, rr);
    char p[200];
    snprintf(p, sizeof p, "%s/Samples/keys.sfz", g_card);
    FILE* o = fopen(p, "wb");
    fputs("// a test instrument\n<group> loop_mode=one_shot\n"
          "<region> sample=ramp.wav lokey=0 hikey=59 pitch_keycenter=48\n"
          "<region> sample=hi.wav lokey=60 hikey=99 pitch_keycenter=60 volume=-6.0206\n"
          "<region> sample=hi.wav key=100 seq_length=2 seq_position=1\n"
          "<region> sample=rr.wav key=100 seq_length=2 seq_position=2\n", o);
    fclose(o);
    std::vector<uint8_t> a = add(30, "/Samples/keys.sfz");
    CHECK(a.size() == 2 && a[1] == samples::kOk);
    int in = a.size() == 2 ? a[0] : 0;
    settle();
    CHECK(samples::instrument(in) && samples::state(in) == samples::kReady);
    samples::View v;
    CHECK(!samples::get(in, &v));   // not a sample
    samples::Layer z[4];
    CHECK(samples::layers(in, 50, 100, 0, 0.5f, z, 4) == 1 && z[0].entry == 0 && z[0].keycenter == 48);
    CHECK(samples::layers(in, 70, 100, 0, 0.5f, z, 4) == 1 && !strcmp(samples::path(z[0].entry), "/Samples/hi.wav") &&
          fabsf(z[0].volume + 6.0206f) < 0.001f && z[0].loop_mode == samples::kOneShot);
    int hi_e = z[0].entry;
    CHECK(samples::state(hi_e) == samples::kReady);
    CHECK(samples::layers(in, 100, 100, 0, 0.5f, z, 4) == 1 && z[0].entry == hi_e);
    CHECK(samples::layers(in, 100, 100, 0, 0.5f, z, 4) == 1 && !strcmp(samples::path(z[0].entry), "/Samples/rr.wav"));
    CHECK(samples::layers(in, 100, 100, 0, 0.5f, z, 4) == 1 && z[0].entry == hi_e);
    CHECK(samples::layers(in, 100, 100, 1, 0.5f, z, 4) == 0);   // no release regions
    dsp::Sample s;
    s.set_param(1, (in + 1) * 64);
    s.midi(proc::kSrcRytm, 0x90, 48, 127);   // the ramp at its key centre: frame for frame
    render(s, 2048);
    CHECK(fabsf(g_l[100] - (0.25f + 0.5f * 100 / 2000.0f)) < 0.003f && peak(g_l, 2010, 2048) == 0.0f);
    s.midi(proc::kSrcRytm, 0x90, 72, 127);   // hi.wav an octave up, at half its level: done by ~500
    render(s, 1024);
    CHECK(fabsf(g_l[100] - 0.25f) < 0.003f && peak(g_l, 510, 1024) == 0.0f);
    CHECK(samples::stats().listed >= 3);
  }
  // MULTISAMPLE on the library: POLY {SAMPLE} (HKEY 1) against upstream's machine (msref): two velocity layers on a
  // key (one a 44.1 kHz stereo sample with a smpl loop, one mono with a region loop and its own ampeg), release regions,
  // a chord, a retrig (tails), a note off with RTRG, knobs off 64
  {
    std::vector<float> a, b2, c;
    for (int i = 0; i < 30000; ++i) {
      float x = sinf(2.0f * 3.14159265f * 220.0f * (float)i / 44100.0f) * (0.6f - 0.4f * (float)i / 30000.0f);
      a.push_back(x), a.push_back(0.3f * sinf(2.0f * 3.14159265f * 331.0f * (float)i / 44100.0f));
    }
    for (int i = 0; i < 9000; ++i) b2.push_back(0.4f * sinf(2.0f * 3.14159265f * (float)i / 37.0f) + 0.1f * ((i * 7919) % 200 - 100) / 100.0f);
    for (int i = 0; i < 6000; ++i) c.push_back(0.5f * expf(-(float)i / 900.0f) * ((i / 13) % 2 ? 1.0f : -1.0f));
    wav("la.wav", 1, 24, 2, 44100, a);
    // a smpl chunk on la.wav: loop 12000..19999 (frames)
    {
      char pth[200];
      snprintf(pth, sizeof pth, "%s/Samples/la.wav", g_card);
      FILE* o = fopen(pth, "ab");
      std::vector<uint8_t> sm = {'s', 'm', 'p', 'l'};
      put32(sm, 60);
      for (int k = 0; k < 7; ++k) put32(sm, 0);
      put32(sm, 1); put32(sm, 0);   // one loop
      put32(sm, 0); put32(sm, 0); put32(sm, 12000); put32(sm, 19999); put32(sm, 0); put32(sm, 0);
      fwrite(sm.data(), 1, sm.size(), o);
      fclose(o);
      // the RIFF size
      FILE* f = fopen(pth, "r+b");
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fseek(f, 4, SEEK_SET);
      uint8_t r4[4] = {(uint8_t)(sz - 8), (uint8_t)((sz - 8) >> 8), (uint8_t)((sz - 8) >> 16), (uint8_t)((sz - 8) >> 24)};
      fwrite(r4, 1, 4, f);
      fclose(f);
    }
    wav("lb.wav", 1, 16, 1, 48000, b2);
    wav("rel.wav", 1, 16, 1, 48000, c);
    char pth[200];
    snprintf(pth, sizeof pth, "%s/Samples/ms.sfz", g_card);
    FILE* o = fopen(pth, "wb");
    fputs("<global> ampeg_attack=0.01 ampeg_decay=0.2 ampeg_sustain=60 ampeg_release=0.15\n"
          "<region> sample=la.wav lokey=40 hikey=90 pitch_keycenter=57 amp_veltrack=80 volume=-3\n"
          "<region> sample=lb.wav lokey=40 hikey=90 pitch_keycenter=60 loop_mode=loop_sustain loop_start=1000 loop_end=4999"
          " tune=-20 amplitude=70 ampeg_hold=0.05 ampeg_release=0.4 offset=200\n"
          "<region> sample=rel.wav lokey=40 hikey=90 pitch_keycenter=64 trigger=release volume=-6\n", o);
    fclose(o);
    std::vector<uint8_t> r = add(40, "/Samples/ms.sfz");
    CHECK(r.size() == 2 && r[1] == samples::kOk);
    settle();
    const int in = r[0];
    CHECK(samples::instrument(in));
    samples::Layer ly[4];
    CHECK(samples::layers(in, 60, 100, 0, 0.5f, ly, 4) == 2);
    {
      samples::View lv;
      CHECK(samples::get(ly[0].entry, &lv) && lv.has_loop && lv.loop_s == 12000 && lv.loop_e == 19999 && lv.ch == 2 && lv.rate == 44100.0f);
    }
    // the upstream knobs: INST VEL LEV RTRG ATK DEC SUS REL TUNE FINE STRT LOOP CHD INV SPR VOI
    const float up[16] = {0, 100, 90, 100, 70, 58, 80, 60, 64.5f, 70, 6, 0, 1, 0, 0, 0};
    for (int k = 0; k < 16; ++k) msref::p[k] = up[k];
    msref::inst = in;
    alignas(64) static uint8_t mem[1 << 20];
    dsp::Poly::Spec sp{};
    sp.notes = 8, sp.nb = 1, sp.nm = 0, sp.blk[0] = dsp::find("SAMPLE"), sp.weight = 0x00;
    CHECK(dsp::Poly::bytes(sp) <= sizeof mem);
    dsp::Poly& py = *new (mem) dsp::Poly(sp, mem);
    auto w = [](float v) { return (int)lrintf(v * 64.0f); };
    py.set_param(0, w(up[2])); py.set_param(1, (in + 1) * 64); py.set_param(2, w(up[8])); py.set_param(3, w(up[10]));
    py.set_param(5, w(up[11])); py.set_param(6, w(up[9])); py.set_param(7, w(up[1])); py.set_param(10, w(up[3]));
    for (int k = 0; k < 4; ++k) py.set_param(11 + k, w(up[4 + k]));
    for (int k = 0; k < 4; ++k) py.set_param(64 + k, (int)up[12 + k] * 64);
    py.set_param(72, 0); py.set_param(73, 100 * 64); py.set_param(75, 64);
    static float ours[20000], theirs[20000];
    float d = 0.0f, pk = 0.0f;
    for (int f = 0; f < 20000; f += 32) {
      if (f == 0) { py.midi(proc::kSrcRytm, 0x90, 60, 100); msref::trig(60, 100); }
      if (f == 4000) { py.midi(proc::kSrcRytm, 0x90, 62, 80); msref::trig(62, 80); }
      if (f == 9024) { py.midi(proc::kSrcRytm, 0x80, 62, 0); msref::release_voice(true); }
      render_any(py, 32);
      memcpy(ours + f, g_l, 32 * sizeof(float));
      msref::render(theirs + f, 32);
    }
    // (upstream MULTISAMPLE leaves out the block in which its last note ends: under 1e-4 of the region's level, there)
    float dl = 0.0f;
    for (int f = 0; f < 20000; ++f) {
      d = fmaxf(d, fabsf(ours[f] - theirs[f])), pk = fmaxf(pk, fabsf(theirs[f]));
      if (f < 16000) dl = fmaxf(dl, fabsf(ours[f] - theirs[f]));
    }
    printf("test_samples: MULTISAMPLE peak %.3f, the library off upstream by %.2g (%.2g before the last note's end)\n", pk, d, dl);
    CHECK(pk > 0.1f && dl < 1e-6f && d < 1e-4f && py.sounding() == 0);
    CHECK(fabsf(theirs[9100]) > 0.0f);
  }
  // GRAIN on a pool entry (SMP 1, the ramp): silent before a note; a note's cloud reads around POS (0.5: the ramp's
  // middle, ~0.5), its level even (1 / sqrt(overlap))
  {
    alignas(64) static uint8_t gm[dsp::kSlotBytes];
    dsp::Grain& g = *(dsp::Grain*)dsp::find("GRAIN")->make(gm, nullptr);
    g.set_param(1, 1 * 64);
    g.set_param(2, 64 * 64);
    g.set_param(11, 119 * 64);   // the playhead still
    render_any(g, 2048);
    CHECK(peak(g_l, 0, 2048) == 0.0f);
    g.midi(proc::kSrcRytm, 0x90, 60, 100);
    render_any(g, 4096);
    CHECK(peak(g_l, 0, 4096) > 0.1f && peak(g_l, 0, 4096) < 2.0f && g_l[3000] == g_r[3000]);
  }
  // WTOSC on pool tables (TBL 6 + entry): a WAV of 4 frames of 2048 (sines, harmonics 1..4, rising level) and one of
  // 3 frames of 600 found by its 'clm ' chunk (stereo: L and R halved). Before its table is built the note plays BASIC;
  // built (wt_poll, a frame a step), the frames' levels are the sines with the table's loudest at 0.95.
  {
    std::vector<float> t4, t3;
    for (int f = 0; f < 4; ++f)
      for (int i = 0; i < 2048; ++i) t4.push_back(0.2f * (float)(f + 1) * sinf(2.0f * 3.14159265f * (float)((f + 1) * i) / 2048.0f));
    for (int f = 0; f < 3; ++f)
      for (int i = 0; i < 600; ++i) {
        float x = 0.5f * sinf(2.0f * 3.14159265f * (float)i / 600.0f);
        t3.push_back(x), t3.push_back(f == 1 ? -x : x);   // frame 1: L and R cancel
      }
    wav("t4.wav", 1, 16, 1, 48000, t4);
    wav("t3.wav", 1, 16, 2, 48000, t3, "<!>600 10000000 wavetable (www.xferrecords.com)");
    std::vector<uint8_t> a4 = add(30, "/Samples/t4.wav"), a3 = add(31, "/Samples/t3.wav");
    CHECK(a4.size() == 2 && a4[1] == samples::kOk && a3.size() == 2 && a3[1] == samples::kOk);
    settle();
    CHECK(samples::get(a3[0], &v) && v.clm == 600 && v.frames == 1800 && samples::get(a4[0], &v) && v.clm == 0);
    alignas(64) static uint8_t wm[sizeof(dsp::WtOsc) + 64];
    dsp::WtOsc& w = *new (wm) dsp::WtOsc();
    w.set_param(1, (6 + a4[0]) * 64);
    w.midi(proc::kSrcRytm, 0x90, 69, 100);
    alignas(64) static uint8_t wb[sizeof(dsp::WtOsc) + 64];
    dsp::WtOsc& basic = *new (wb) dsp::WtOsc();
    basic.midi(proc::kSrcRytm, 0x90, 69, 100);
    static float bl[4096], br[4096];
    render_any(w, 256);
    memcpy(bl, g_l, sizeof(float) * 256);
    render_any(basic, 256);
    CHECK(!memcmp(bl, g_l, sizeof(float) * 256));   // not built yet: BASIC
    uint32_t nf = 0;
    CHECK(!dsp::wt_pool(a4[0], &nf));
    int steps = 0;
    while (dsp::wt_poll() && steps < 1000) { ++steps; usleep(1000); render_any(w, 32); }
    const float* t = dsp::wt_pool(a4[0], &nf);
    CHECK(t && nf == 4 && dsp::wt_stats().loads == 1 && dsp::wt_stats().resident == 1);
    for (int f = 0; f < 4; ++f) {   // level 0 (2048 samples): the frame's sine, 0.2 (f + 1) / 0.8 x 0.95
      const float* l0 = wt::level(t + (size_t)f * wt::kFrameFloats, 0);
      float e = 0.0f;
      for (int i = 0; i < 2048; ++i)
        e = fmaxf(e, fabsf(l0[i] - 0.95f * (float)(f + 1) / 4.0f * sinf(2.0f * 3.14159265f * (float)((f + 1) * i) / 2048.0f)));
      CHECK(e < 2e-3f);
    }
    render_any(w, 1024);   // POS 0: frame 0, the sine at 0.2375, A4
    CHECK(fabsf(peak(g_l, 200, 1024) - 0.2375f) < 0.01f);
    // the 'clm ' one: 3 frames of 600; frame 1 silent (L + R cancel), the others the sine at 0.95
    w.set_param(1, (6 + a3[0]) * 64);
    render_any(w, 32);   // asks for it; the old table plays on
    CHECK(fabsf(peak(g_l, 0, 32)) > 0.01f);
    steps = 0;
    while (dsp::wt_poll() && steps < 1000) { ++steps; usleep(1000); render_any(w, 32); }
    t = dsp::wt_pool(a3[0], &nf);
    CHECK(t && nf == 3 && dsp::wt_stats().resident == 2);
    CHECK(fabsf(wt::frame_peak(t) - 0.95f) < 1e-3f && wt::frame_peak(t + wt::kFrameFloats) < 1e-3f);
    // dropped: its table goes at the next step
    send({samples::kDrop, 0x45, a4[0]});
    dsp::wt_poll();
    CHECK(!dsp::wt_pool(a4[0], &nf) && dsp::wt_stats().resident == 1);
    (void)br;
  }
  // the library's own entry
  CHECK(dsp::find("SAMPLE") && dsp::find("SAMPLE")->nparams == 16);

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

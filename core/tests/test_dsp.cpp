// core/tests/test_dsp.cpp - the DSP library and its graphs (core/dsp): PLACE / FREE / INFO through the host link, the
// refusals, graphs of blocks joined by route points and run in graph order (as pi/engine/audio_return.cpp runs them),
// params through the lock service (target = the slot), TRIG to one slot's block, FREE's fade and release, the HELLO, and each primitive's
// behaviour: identity at its defaults where it has one, delay times, feedback that stays bounded, filters, no NaN.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "check.h"
#include "dsp/blocks.h"
#include "dsp/dsp.h"
#include "dsp/gen.h"
#include "hooks.h"
#include "kv/link.h"
#include "locks/locks.h"
#include "proc/proc.h"
#include "route/route.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void capture(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }

static bool send(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x51, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return kv::link::from_host(m.data(), (int)m.size());
}
static std::vector<std::vector<uint8_t>> answers() {
  g_out.clear();
  kv::link::host_step(16);
  return g_out;
}
// PLACE: the slot (>= 0) or -code
static int place(int seq, int owner, int slot, const char* name) {
  std::vector<uint8_t> b = {0x30, (uint8_t)seq, (uint8_t)owner, (uint8_t)slot};
  for (const char* p = name; *p; ++p) b.push_back((uint8_t)*p);
  send(b);
  auto a = answers();
  CHECK(a.size() == 1 && a[0].size() == 11);
  if (a[0][6] == 0x70) { CHECK(a[0][7] == seq && a[0][8] == owner); return a[0][9]; }
  CHECK(a[0][6] == 0x71);
  return -a[0][9];
}
static int rt(int seq, int owner, int src, int dst, int gain) {
  send({0x20, (uint8_t)seq, (uint8_t)owner, (uint8_t)src, (uint8_t)dst, (uint8_t)(gain >> 7), (uint8_t)(gain & 0x7F)});
  auto a = answers();
  CHECK(a.size() == 1);
  return a[0][6] == 0x60 ? a[0][9] : -a[0][9];
}
// the lock service: set `set` on track 1 for slot `slot`, params first..first+n-1, then BASE values
static void base(int set, int slot, int first, std::vector<int> v) {
  CHECK(send({0x10, (uint8_t)set, 1, (uint8_t)slot, (uint8_t)first, (uint8_t)v.size()}));
  std::vector<uint8_t> b = {0x11, (uint8_t)set, 0, (uint8_t)v.size()};
  for (int x : v) b.push_back((uint8_t)(x >> 7)), b.push_back((uint8_t)(x & 0x7F));
  CHECK(send(b));
}

// one engine block as audio_return.cpp runs it: IN (12 channels, the given frames) -> OUT
static float g_in[12][256], g_o[12][256];
static float g_si[route::kSlotChans][256], g_so[route::kSlotChans][256];
static void block(int n) {
  static float* si[route::kSlotChans];
  static float* so[route::kSlotChans];
  for (int k = 0; k < route::kSlotChans; ++k) si[k] = g_si[k], so[k] = g_so[k];
  proc::Block b{};
  for (int c = 0; c < 12; ++c) b.in[c] = g_in[c], b.out[c] = g_o[c];
  for (int c = 0; c < 12; ++c) for (int f = 0; f < n; ++f) g_o[c][f] = 0.0f;
  for (int k = 0; k < route::kSlotChans; ++k) for (int f = 0; f < n; ++f) g_si[k][f] = g_so[k][f] = 0.0f;
  b.frames = n;
  route::process_in(b.in, b.out, si, n);
  proc::begin();
  uint8_t order[route::kSlots];
  route::order(order);
  for (int i = 0; i < route::kSlots; ++i) {
    proc::run(order[i], b, si, so);
    route::process_slot(order[i], so, b.out, si, n);
  }
  proc::end();
  route::end_block(n);
}
static void quiet_in() { memset(g_in, 0, sizeof g_in); }

// a primitive on its own, outside any graph: n frames of x through it (stereo, the same on both sides)
static std::vector<float> run_block(proc::Proc& p, const std::vector<float>& x, int side = 0) {
  std::vector<float> y(x.size());
  static float in[2][64], out[2][64];
  proc::Block b{};
  b.sin[0] = in[0]; b.sin[1] = in[1]; b.sout[0] = out[0]; b.sout[1] = out[1];
  for (size_t at = 0; at < x.size(); at += 64) {
    int n = (int)(x.size() - at < 64 ? x.size() - at : 64);
    for (int f = 0; f < n; ++f) in[0][f] = in[1][f] = x[at + f];
    b.frames = n;
    p.process(b);
    for (int f = 0; f < n; ++f) y[at + f] = out[side][f];
  }
  return y;
}
static float peak(const std::vector<float>& y, size_t from = 0, size_t to = 0) {
  float m = 0.0f;
  for (size_t i = from; i < (to ? to : y.size()); ++i) {
    CHECK(std::isfinite(y[i]));
    m = std::fmax(m, std::fabs(y[i]));
  }
  return m;
}
static float rms(const std::vector<float>& y, size_t from, size_t to) {
  double s = 0;
  for (size_t i = from; i < to; ++i) s += (double)y[i] * y[i];
  return (float)std::sqrt(s / (double)(to - from));
}
static std::vector<float> sine(float hz, size_t n, float a = 0.5f) {
  std::vector<float> x(n);
  for (size_t i = 0; i < n; ++i) x[i] = a * std::sin(2.0f * dsp::kPi * hz * (float)i / dsp::kRate);
  return x;
}
static std::vector<float> noise(size_t n, float a = 0.5f) {
  std::vector<float> x(n);
  uint32_t s = 12345;
  for (size_t i = 0; i < n; ++i) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; x[i] = a * ((float)(s >> 8) / 8388608.0f - 1.0f); }
  return x;
}

static void test_primitives() {
  alignas(64) static uint8_t mem[dsp::kSlotBytes];
  // each block fits a slot
  for (int i = 0; i < dsp::library_size(); ++i) CHECK(dsp::library(i)->bytes <= dsp::kSlotBytes);

  // SHAPER and WIDTH at their defaults: the identity
  {
    proc::Proc* p = dsp::find("SHAPER")->make(mem, nullptr);
    auto x = noise(4096), y = run_block(*p, x);
    for (size_t i = 0; i < x.size(); ++i) CHECK(std::fabs(y[i] - x[i]) < 1e-6f);
    p->~Proc();
    p = dsp::find("WIDTH")->make(mem, nullptr);
    y = run_block(*p, x);
    for (size_t i = 0; i < x.size(); ++i) CHECK(std::fabs(y[i] - x[i]) < 1e-5f);
    p->~Proc();
  }
  // DELAY: an impulse comes back after TIME; with FEEDBACK 0 once, with FEEDBACK it repeats, quieter each time
  {
    auto* d = (dsp::Delay*)dsp::find("DELAY")->make(mem, nullptr);
    // TIME for 10 ms: expmap(v, 0.05, 2000) = 10 -> v = 16383 * ln(200) / ln(40000)
    int v = (int)std::lround(16383.0 * std::log(10.0 / 0.05) / std::log(2000.0 / 0.05));
    d->set_param(0, v);
    d->set_param(1, 0);
    // the time smoother starts at the default: let it settle on silence first
    run_block(*d, std::vector<float>(48000, 0.0f));
    std::vector<float> x(4800, 0.0f);
    x[0] = 1.0f;
    auto y = run_block(*d, x);
    int at = 0;
    for (int i = 1; i < (int)y.size(); ++i) if (std::fabs(y[i]) > std::fabs(y[at])) at = i;
    CHECK(std::abs(at - 480) <= 1 && std::fabs(y[at]) > 0.5f);
    CHECK(peak(y, 600, 4800) < 1e-6f);   // no repeat with FEEDBACK 0
    d->set_param(1, 8192);               // FEEDBACK ~ 0.5
    run_block(*d, std::vector<float>(48000, 0.0f));
    y = run_block(*d, x);
    CHECK(peak(y, 470, 490) > 0.5f && peak(y, 950, 970) > 0.2f && peak(y, 950, 970) < peak(y, 470, 490));
    // at full FEEDBACK and full DRIVE, loud noise for 5 s: bounded, finite
    d->set_param(1, 16383);
    d->set_param(4, 16383);
    y = run_block(*d, noise(240000, 1.0f));
    CHECK(peak(y) < 4.01f);
    // a tail left alone decays to (almost) nothing with FEEDBACK at 0.5 and damping
    d->set_param(1, 8192);
    d->set_param(4, 0);
    d->set_param(2, 4000);
    y = run_block(*d, std::vector<float>(96000, 0.0f));
    CHECK(peak(y, 90000, 96000) < 1e-3f);
    d->~Delay();
  }
  // FILTER: LP at 1 kHz takes a 10 kHz sine down by more than 30 dB and leaves 100 Hz alone; HP the other way round
  {
    proc::Proc* f = dsp::find("FILTER")->make(mem, nullptr);
    int v = (int)std::lround(16383.0 * std::log(1000.0 / 20.0) / std::log(20000.0 / 20.0));
    f->set_param(1, v);
    run_block(*f, std::vector<float>(4800, 0.0f));   // (the cutoff smoother settles)
    auto hi = run_block(*f, sine(10000, 9600)), lo = run_block(*f, sine(100, 9600));
    CHECK(20 * std::log10(rms(hi, 4800, 9600) / (0.5f / std::sqrt(2.0f))) < -30.0f);
    CHECK(std::fabs(20 * std::log10(rms(lo, 4800, 9600) / (0.5f / std::sqrt(2.0f)))) < 0.5f);
    f->set_param(0, 16383 / 4 + 1);   // HP
    hi = run_block(*f, sine(10000, 9600));
    lo = run_block(*f, sine(100, 9600));
    CHECK(std::fabs(20 * std::log10(rms(hi, 4800, 9600) / (0.5f / std::sqrt(2.0f)))) < 0.5f);
    CHECK(20 * std::log10(rms(lo, 4800, 9600) / (0.5f / std::sqrt(2.0f))) < -30.0f);
    // full resonance and drive on loud noise: finite
    f->set_param(2, 16383);
    f->set_param(3, 16383);
    peak(run_block(*f, noise(48000, 1.0f)));
    f->~Proc();
  }
  // SHAPER: CLIP with drive keeps within +-LEVEL x 2; BITS 1 gives two levels; RATE holds samples
  {
    proc::Proc* s = dsp::find("SHAPER")->make(mem, nullptr);
    s->set_param(1, 16383);
    CHECK(peak(run_block(*s, noise(4800, 1.0f))) <= 1.0f + 1e-6f);
    s->set_param(1, 0);
    s->set_param(2, 0);   // 1 bit
    auto y = run_block(*s, sine(100, 4800, 0.9f));
    for (float v : y) CHECK(std::fabs(v) < 1e-6f || std::fabs(std::fabs(v) - 1.0f) < 1e-6f);
    s->set_param(2, 16383);
    s->set_param(3, 0);   // 100 Hz: each value held ~480 samples
    y = run_block(*s, noise(4800));
    int changes = 0;
    for (size_t i = 1; i < y.size(); ++i) changes += y[i] != y[i - 1];
    CHECK(changes >= 8 && changes <= 12);
    s->~Proc();
  }
  // MOD: RING at depth 1 on DC gives the oscillator (mean ~0); AMP at depth 0 is the identity
  {
    proc::Proc* m = dsp::find("MOD")->make(mem, nullptr);
    auto y = run_block(*m, std::vector<float>(48000, 0.5f));
    double mean = 0;
    for (float v : y) mean += v;
    CHECK(std::fabs(mean / 48000.0) < 0.01 && peak(y) > 0.49f);
    m->set_param(0, 16383);
    m->set_param(2, 0);
    auto x = noise(4800);
    y = run_block(*m, x);
    for (size_t i = 0; i < x.size(); ++i) CHECK(std::fabs(y[i] - x[i]) < 1e-6f);
    m->~Proc();
  }
}

// TRIG's receiver: a block that keeps the note events it is given (added to the library as a shipped block is)
struct NoteRec : proc::Proc {
  std::vector<std::vector<int>> got;
  void process(const proc::Block&) override {}
  void set_param(int k, int v) override { got.push_back({-1, k, v}); }
  void midi(int src, uint8_t st, uint8_t d1, uint8_t d2) override { got.push_back({src, st, d1, d2}); }
  const char* name() const override { return "NOTEREC"; }
};
static proc::Proc* make_rec(void* mem, const void*) { return new (mem) NoteRec(); }
static const dsp::ParamInfo kRecP[] = {{"A", 0}, {"B", 0}, {"C", 0}, {"D", 0}};
static const dsp::BlockInfo kRecInfo{"NOTEREC", 4, kRecP, sizeof(NoteRec), make_rec};

static void test_trig() {
  CHECK(dsp::add_block(&kRecInfo));
  CHECK(place(60, 7, 3, "NOTEREC") == 3);
  CHECK(place(61, 7, 4, "NOTEREC") == 4);
  NoteRec* r3 = static_cast<NoteRec*>(proc::slot(3));
  NoteRec* r4 = static_cast<NoteRec*>(proc::slot(4));
  uint32_t t0 = dsp::stats().trigs, bad0 = dsp::stats().ops_bad;
  // a trig (note 60, vel 100, block 0x12345) and a release to slot 3; a vel 0 trig arrives as 1
  CHECK(send({0x37, 7, 3, 0, 60, 100, 0x00, 0x04, 0x46, 0x45}));
  CHECK(send({0x37, 7, 3, 1, 60, 0, 0, 0, 0, 1}));
  CHECK(send({0x37, 7, 3, 0, 62, 0, 0, 0, 0, 2}));
  CHECK(answers().empty());                      // no answer
  CHECK(r3->got.empty());                         // render side: on the next block
  block(64);
  CHECK(r3->got.size() == 3 && r4->got.empty());
  CHECK((r3->got[0] == std::vector<int>{proc::kSrcRytm, 0x90, 60, 100}));
  CHECK((r3->got[1] == std::vector<int>{proc::kSrcRytm, 0x80, 60, 0}));
  CHECK((r3->got[2] == std::vector<int>{proc::kSrcRytm, 0x90, 62, 1}));
  CHECK(dsp::stats().trigs == t0 + 3 && dsp::stats().last_blk == 2);
  // dropped: another owner's slot, an empty slot, a bad kind, a short message
  CHECK(!send({0x37, 8, 3, 0, 60, 100, 0, 0, 0, 0}));
  CHECK(!send({0x37, 7, 9, 0, 60, 100, 0, 0, 0, 0}));
  CHECK(!send({0x37, 7, 3, 2, 60, 100, 0, 0, 0, 0}));
  CHECK(!send({0x37, 7, 3, 0, 60, 100}));
  block(64);
  CHECK(r3->got.size() == 3 && dsp::stats().ops_bad == bad0 + 4 && dsp::stats().trigs == t0 + 3);
  // a trig with its knob values (params 1..2 = 300, 16383): set before the note, in the same block
  r3->got.clear();
  CHECK(send({0x37, 7, 3, 0, 48, 90, 0, 0, 0, 3, 1, 2, 300 >> 7, 300 & 0x7F, 0x7F, 0x7F}));
  // PARAM: params 0..3 straight to the block
  CHECK(send({0x38, 7, 3, 0, 4, 0, 1, 0, 2, 0, 3, 0, 4}));
  block(64);
  CHECK(r3->got.size() == 7);
  CHECK((r3->got[0] == std::vector<int>{-1, 1, 300}) && (r3->got[1] == std::vector<int>{-1, 2, 16383}));
  CHECK((r3->got[2] == std::vector<int>{proc::kSrcRytm, 0x90, 48, 90}));
  CHECK((r3->got[3] == std::vector<int>{-1, 0, 1}) && (r3->got[6] == std::vector<int>{-1, 3, 4}));
  CHECK(dsp::stats().params == 1);
  // dropped whole: params past the block's 4, a tail with the wrong length, another owner's PARAM
  bad0 = dsp::stats().ops_bad;
  CHECK(!send({0x37, 7, 3, 0, 48, 90, 0, 0, 0, 3, 3, 2, 0, 1, 0, 1}));
  CHECK(!send({0x37, 7, 3, 0, 48, 90, 0, 0, 0, 3, 0, 2, 0, 1}));
  CHECK(!send({0x38, 8, 3, 0, 1, 0, 1}));
  CHECK(!send({0x38, 7, 3, 2, 3, 0, 1, 0, 1, 0, 1}));
  block(64);
  CHECK(r3->got.size() == 7 && dsp::stats().ops_bad == bad0 + 4);
  send({0x31, 7, 0x7F});
  for (int i = 0; i < 4; ++i) { block(128); answers(); }
  CHECK(dsp::stats().used == 0);
  dsp::remove_block(&kRecInfo);
}

// OSC, NOISE, ENV (core/dsp/gen.h) on their own
static void test_gen() {
  // OSC: note 69 is 440 Hz (a sine, WAV 0): ~880 zero crossings a second; a saw and a pulse stay bounded
  {
    dsp::Osc o;
    o.set_param(1, 0);
    o.midi(proc::kSrcRytm, 0x90, 69, 100);
    std::vector<float> y = run_block(o, std::vector<float>(48000, 0.0f));
    int z = 0;
    for (size_t i = 1; i < y.size(); ++i) z += (y[i - 1] < 0.0f) != (y[i] < 0.0f);
    CHECK(std::abs(z - 880) <= 4 && peak(y) > 0.9f && peak(y) < 1.1f);
    o.set_param(1, 85 * 64);
    CHECK(peak(run_block(o, std::vector<float>(4800, 0.0f))) < 1.3f);
    o.set_param(1, 127 * 64);
    o.set_param(6, 127 * 64);   // SUB
    CHECK(peak(run_block(o, std::vector<float>(4800, 0.0f))) < 2.4f);
    // TUN +12: an octave up
    o.set_param(1, 0); o.set_param(6, 0); o.set_param(2, 76 * 64);
    y = run_block(o, std::vector<float>(48000, 0.0f));
    z = 0;
    for (size_t i = 1; i < y.size(); ++i) z += (y[i - 1] < 0.0f) != (y[i] < 0.0f);
    CHECK(std::abs(z - 1760) <= 6);
    // LEV 0, THRU 127: the input as it is
    o.set_param(0, 0); o.set_param(7, 127 * 64);
    std::vector<float> x = sine(1000.0f, 960);
    y = run_block(o, x);
    float d = 0.0f;
    for (size_t i = 0; i < x.size(); ++i) d = std::fmax(d, std::fabs(y[i] - x[i]));
    CHECK(d < 1e-3f);
  }
  // NOISE: white is loud, dark has less high end than white; THRU passes the input
  {
    dsp::Noise n;
    n.set_param(2, 0);
    std::vector<float> w = run_block(n, std::vector<float>(48000, 0.0f));
    CHECK(rms(w, 0, w.size()) > 0.3f && peak(w) <= 1.01f);
    auto hf = [](const std::vector<float>& y) { double s = 0; for (size_t i = 1; i < y.size(); ++i) s += std::fabs(y[i] - y[i - 1]); return s; };
    n.set_param(1, 0);
    std::vector<float> dk = run_block(n, std::vector<float>(48000, 0.0f));
    CHECK(hf(dk) < 0.2 * hf(w) && peak(dk) < 4.0f);
    n.set_param(0, 0); n.set_param(2, 127 * 64);
    std::vector<float> x = sine(500.0f, 960);
    std::vector<float> y = run_block(n, x);
    float d = 0.0f;
    for (size_t i = 0; i < x.size(); ++i) d = std::fmax(d, std::fabs(y[i] - x[i]));
    CHECK(d < 1e-3f);
  }
  // ENV: silent before a note (VCA all), a hit rises to the input's level and falls; ctl(0) follows
  {
    dsp::Env e;
    std::vector<float> one(4800, 0.5f);
    CHECK(peak(run_block(e, one)) < 1e-6f && e.ctl(0) == 0.0f);
    e.midi(proc::kSrcRytm, 0x90, 60, 127);
    std::vector<float> y = run_block(e, std::vector<float>(64, 0.5f));
    CHECK(peak(y) > 0.45f && e.ctl(0) > 0.5f);
    y = run_block(e, std::vector<float>(96000, 0.5f));
    CHECK(y.back() < 1e-3f && e.ctl(0) < 1e-3f);
    // ADSR: held at SUS until the note off, then released
    e.set_param(4, 1 * 64); e.set_param(2, 64 * 64); e.set_param(1, 0);
    e.midi(proc::kSrcRytm, 0x90, 60, 127);
    y = run_block(e, std::vector<float>(48000, 1.0f));
    CHECK(std::fabs(y.back() - 64.0f / 127.0f) < 0.01f);
    e.midi(proc::kSrcRytm, 0x80, 60, 0);
    y = run_block(e, std::vector<float>(96000, 1.0f));
    CHECK(y.back() < 1e-3f);
    // VEL all: a soft note's hit is lower
    e.set_param(4, 0); e.set_param(6, 127 * 64);
    e.midi(proc::kSrcRytm, 0x90, 60, 32);
    run_block(e, std::vector<float>(64, 1.0f));
    CHECK(e.ctl(0) > 0.1f && e.ctl(0) < 0.3f);
    // VCA 0: the input passes, the envelope is only ctl
    e.set_param(5, 0);
    y = run_block(e, std::vector<float>(64, 0.5f));
    CHECK(std::fabs(y.back() - 0.5f) < 1e-6f);
  }
  // MOD: its oscillator as ctl(0) / ctl(1), within -1..1, moving
  {
    dsp::Modulator m;
    m.set_param(1, 6000);   // ~5 Hz
    float lo = 2.0f, hi = -2.0f;
    for (int i = 0; i < 400; ++i) { run_block(m, std::vector<float>(64, 0.0f)); lo = std::fmin(lo, m.ctl(0)); hi = std::fmax(hi, m.ctl(0)); }
    CHECK(lo < -0.9f && hi > 0.9f && lo >= -1.0f && hi <= 1.0f);
  }
}

// mod points (route op 24 MOD, core/proc): ENV's ctl onto a FILTER's cutoff, the loop check, the graph order, depth,
// UNROUTE, FREE
static int mod(int seq, int owner, int src, int ctl, int dst, int param, int depth) {
  send({0x24, (uint8_t)seq, (uint8_t)owner, (uint8_t)src, (uint8_t)ctl, (uint8_t)dst, (uint8_t)param, (uint8_t)(depth >> 7),
        (uint8_t)(depth & 0x7F)});
  auto a = answers();
  CHECK(a.size() == 1);
  return a.size() == 1 && a[0][6] == 0x60 ? a[0][9] : a.size() == 1 ? -a[0][9] : -99;
}
static void test_mods() {
  CHECK(place(70, 7, 3, "ENV") == 3);
  CHECK(place(71, 7, 2, "FILTER") == 2);
  dsp::Block* flt = static_cast<dsp::Block*>(proc::slot(2));
  CHECK(send({0x38, 7, 2, 1, 1, 4000 >> 7, 4000 & 0x7F}));   // CUTOFF 4000
  block(64);
  CHECK(flt->param(1) == 4000);
  int mp = mod(73, 7, 3, 0, 2, 1, 16383);
  CHECK(mp == route::kPoints && route::mod_point(0).open && route::mod_point(0).dst == 2);
  CHECK(mod(74, 7, 2, 0, 3, 0, 8192) == -route::kLoop);   // the FILTER feeds the ENV's: a loop
  CHECK(mod(75, 7, 3, 0, 3, 0, 8192) == -route::kLoop);   // onto itself
  CHECK(rt(76, 7, route::slot_chan(2, 0), route::slot_chan(3, 0), 8192) == -route::kLoop);   // audio back the other way
  CHECK(mod(77, 7, 3, 9, 2, 1, 8192) == -route::kBad);    // no ctl 9
  block(64);
  uint8_t ord[route::kSlots];
  route::order(ord);
  int at2 = -1, at3 = -1;
  for (int i = 0; i < route::kSlots; ++i) { if (ord[i] == 2) at2 = i; if (ord[i] == 3) at3 = i; }
  CHECK(at3 >= 0 && at3 < at2);   // the ENV runs before the FILTER it moves
  CHECK(flt->param(1) == 4000);   // no note yet: the envelope is 0
  CHECK(send({0x37, 7, 3, 0, 60, 127, 0, 0, 0, 0}));
  block(64);
  block(64);
  CHECK(flt->param(1) > 12000);   // 4000 + ~1 x 16383, clamped
  // GAIN on the mod point: its depth; 8192 none
  CHECK(send({0x23, 7, (uint8_t)mp, 8192 >> 7, 0}));
  block(64);
  CHECK(flt->param(1) == 4000);
  CHECK(send({0x23, 7, (uint8_t)mp, 0, 0}));     // -1: down
  CHECK(send({0x37, 7, 3, 0, 60, 127, 0, 0, 0, 0}));
  block(64);
  block(64);
  CHECK(flt->param(1) < 1000);
  // a PARAM while modulated moves the base (the offset stays)
  int before = flt->param(1);
  CHECK(send({0x38, 7, 2, 1, 1, 16383 >> 7, 16383 & 0x7F}));
  block(64);
  CHECK(flt->param(1) < 16383 && flt->param(1) > before);
  // UNROUTE: back to the base
  CHECK(send({0x22, 7, (uint8_t)mp}));
  CHECK(!route::mod_point(0).open);
  block(64);
  CHECK(flt->param(1) == 16383);
  // FREE of the ENV closes a mod point from it; the HELLO every one
  CHECK(mod(78, 7, 3, 0, 2, 1, 12000) == route::kPoints);
  CHECK(send({0x31, 7, 3}));
  CHECK(!route::mod_point(0).open);
  for (int i = 0; i < 4; ++i) { block(128); answers(); }
  CHECK(place(79, 7, 3, "MOD") == 3);
  CHECK(mod(80, 7, 3, 0, 2, 1, 12000) == route::kPoints);
  send({0x00, 0, 0, 0, 0, 1});
  answers();
  CHECK(!route::mod_point(0).open);
  for (int i = 0; i < 4; ++i) { block(128); answers(); }
  CHECK(dsp::stats().used == 0);
}

int main() {
  hooks.to_rytm = capture;
  CHECK(kv::link::attach_generic());
  test_primitives();
  test_gen();

  // the arena: 16 x 1 MiB, 64-byte aligned
  void* arena = std::aligned_alloc(64, (size_t)dsp::kSlots * dsp::kSlotBytes);
  CHECK(!dsp::begin(arena, dsp::kSlotBytes));
  CHECK(dsp::begin(arena, (size_t)dsp::kSlots * dsp::kSlotBytes));

  // INFO: the library, a block's params
  send({0x32, 1, 0, 0, 0});
  auto a = answers();
  CHECK(a.size() == 1 && a[0][6] == 0x72 && a[0][7] == 1 && a[0][11] == 5 && !memcmp(&a[0][12], "DELAY", 5) && a[0][17] == 8);
  send({0x32, 2, 1, 0, 1});
  a = answers();
  CHECK(a.size() == 1 && a[0][11] == 8 && !memcmp(&a[0][12], "FEEDBACK", 8) && (a[0][20] << 7 | a[0][21]) == 6000);
  send({0x32, 3, 0, 99, 0});
  a = answers();
  CHECK(a.size() == 1 && a[0][11] == 0 && a[0][12] == 0);   // past the end: an empty name, 0 params

  // PLACE: refusals, then a DELAY anywhere (slot 0) and a FILTER in slot 5
  CHECK(place(1, 9, 0x7F, "NOPE") == -dsp::kUnknown);
  CHECK(place(2, 9, 16, "DELAY") == -dsp::kBad);
  CHECK(place(3, 9, 0x7F, "DELAY") == 0);
  CHECK(place(4, 9, 0, "FILTER") == -dsp::kFull);
  CHECK(place(5, 9, 5, "FILTER") == 5);
  CHECK(dsp::slot(0).state == dsp::kPlacedState && !strcmp(dsp::slot(0).block, "DELAY") && dsp::slot(5).owner == 9);
  CHECK(proc::slot(0) && !strcmp(proc::slot(0)->name(), "DELAY") && dsp::stats().used == 2);

  // a graph: SD stem (IN 3) -> DELAY (slot 0) -> FILTER (slot 5) -> MAIN FX L/R; the DELAY at 10 ms, no feedback; the
  // FILTER wide open. An impulse on SD comes out on MAIN FX 480 frames later.
  int v10 = (int)std::lround(16383.0 * std::log(10.0 / 0.05) / std::log(2000.0 / 0.05));
  base(9, 0, 0, {v10, 0});
  CHECK(rt(10, 9, 3, route::slot_chan(0, 0), 8192) >= 0);
  CHECK(rt(11, 9, 3, route::slot_chan(0, 1), 8192) >= 0);
  CHECK(rt(12, 9, route::slot_chan(0, 0), route::slot_chan(5, 0), 8192) >= 0);
  CHECK(rt(13, 9, route::slot_chan(0, 1), route::slot_chan(5, 1), 8192) >= 0);
  CHECK(rt(14, 9, route::slot_chan(5, 0), 2, 8192) >= 0);
  CHECK(rt(15, 9, route::slot_chan(5, 1), 3, 8192) >= 0);
  CHECK(rt(16, 9, route::slot_chan(5, 0), route::slot_chan(0, 0), 8192) == -route::kLoop);   // no loops in a graph
  quiet_in();
  for (int i = 0; i < 400; ++i) block(128);   // ramps in, the DELAY's time settles
  std::vector<float> fx;
  for (int i = 0; i < 10; ++i) {
    quiet_in();
    if (i == 0) g_in[3][0] = 1.0f;
    block(128);
    for (int f = 0; f < 128; ++f) { fx.push_back(g_o[2][f]); CHECK(g_o[3][f] == g_o[2][f] && g_o[0][f] == 0.0f); }
  }
  int at = 0;
  for (int i = 1; i < (int)fx.size(); ++i) if (std::fabs(fx[i]) > std::fabs(fx[at])) at = i;
  CHECK(std::abs(at - 480) <= 2 && fx[at] > 0.4f);
  // a lock on the DELAY's FEEDBACK (param 1) through the lock service on step 4 of track 1: played on the tick
  CHECK(send({0x12, 9, 4, 1, 8192 >> 7, 8192 & 0x7F}));
  locks::tick(1, 4, true);
  CHECK(locks::value(9, 1) == 8192);
  block(64);
  CHECK(static_cast<dsp::Block*>(proc::slot(0))->param(1) == 8192);

  // FREE slot 0 (owner 9): its points fade out (whoever owns them), then the block goes; the FILTER stays
  CHECK(send({0x31, 9, 0}));
  CHECK(dsp::slot(0).state == dsp::kFading);
  CHECK(place(20, 9, 0, "DELAY") == -dsp::kFull);   // not free until it is gone
  for (int i = 0; i < 4; ++i) { quiet_in(); block(128); answers(); }
  CHECK(dsp::slot(0).state == dsp::kEmpty && !proc::slot(0) && dsp::slot(5).state == dsp::kPlacedState);
  CHECK(route::point(0).open == false && route::point(4).open);   // the FILTER's return stays
  CHECK(place(21, 9, 0, "SHAPER") == 0);
  // another owner's FREE does nothing
  CHECK(!send({0x31, 8, 0x7F}));
  CHECK(dsp::slot(0).state == dsp::kPlacedState);

  // the HELLO frees every slot (and the route closes every point)
  send({0x00, 0, 0, 0, 0, 1});
  answers();
  for (int i = 0; i < 4; ++i) { block(128); answers(); }
  CHECK(dsp::stats().used == 0 && !proc::any() && !route::any());
  for (int s = 0; s < dsp::kSlots; ++s) CHECK(place(30 + s, 7, 0x7F, "WIDTH") == s);
  CHECK(place(50, 7, 0x7F, "WIDTH") == -dsp::kFull);
  send({0x31, 7, 0x7F});
  for (int i = 0; i < 4; ++i) { block(128); answers(); }
  CHECK(dsp::stats().used == 0);

  test_trig();
  test_mods();

  printf("test_dsp: OK (%lu placed, %lu freed, %lu refused)\n", (unsigned long)dsp::stats().placed,
         (unsigned long)dsp::stats().freed, (unsigned long)dsp::stats().refused);
  std::free(arena);
  return 0;
}

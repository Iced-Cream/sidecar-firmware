// core/tests/test_stream_cardq.cpp - DSP SAMPLE's stream with a card core (core/engines/stream_voice.h, 7z 2026-10-31): the
// head's read, the refills and the file's close are requests of the stream's own. The output is compared bit for bit with
// the same stream without a card core (each read on the calling core, as before): at unity and an octave up, 16-bit
// stereo, 24-bit mono, one shot and a loop, the card core 10 ms late all the time, away for 100 ms. No card time on the
// control core. Core 0 held up for good: the requests taken back with 10 ms of audio left, the output still the same. A
// re-trig and a loop back with a read under way, a new sample with a read under way (nothing of the old file heard), the
// streams' need reaching cardq::busy().
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "engines/stream_voice.h"
#include "msamp/cardq.h"
#include "platform.h"
#include "pool/card_index.h"
#include "voices.h"
#include "wg_util.h"

using namespace core;
using SV = rytm::StreamVoice;
constexpr int kB = 12;   // the engine's block
static card::Index idx;
static uint8_t scratch[256 * 1024];
static uint8_t head[SV::kHead], ring[SV::kRingBytes];
static SV sv;
static void lg(const char*, ...) {}

static int g_smp;   // the file's index in its folder (find)
static bool find(const char* name, int* fo, card::FileRec* r) {
  for (int f = 0; f < idx.folders(); ++f)
    for (int i = 0; idx.file(f, i, r); ++i)
      if (!strcmp(r->name, name)) return *fo = f, g_smp = i, true;
  return false;
}
struct Play { float semis; bool loop; float end; int blocks; };
static std::vector<float> render(const Play& p, int b) {
  float l[kB], r[kB];
  std::vector<float> o;
  (void)b;
  sv.render(p.semis, 0.0f, p.end, p.loop, 1.0f, l, kB, r);
  o.insert(o.end(), l, l + kB), o.insert(o.end(), r, r + kB);
  return o;
}
static std::vector<float> old_way(const char* name, const Play& p) {   // no card core: the reads here
  int fo;
  card::FileRec r;
  File f;
  CHECK(find(name, &fo, &r) && idx.open(fo, r, &f));
  CHECK(sv.prepare(r, f, fo, g_smp, 0.0f));
  sv.trigger();
  std::vector<float> o;
  for (int b = 0; b < p.blocks; ++b) {
    while (sv.refill()) {}
    auto x = render(p, b);
    o.insert(o.end(), x.begin(), x.end());
  }
  return o;
}
static bool serve() {
  host_card_core(0);
  bool r = cardq::service() != nullptr;
  host_card_core(3);
  return r;
}
static void prep(const char* name) {
  int fo;
  card::FileRec r;
  static File f;
  CHECK(find(name, &fo, &r) && idx.open(fo, r, &f));
  for (int i = 0; i < 100; ++i) {
    SV::Prep s = sv.prepare_q(r, f, fo, g_smp, 0.0f, false);
    if (s == SV::kPrepOk) return;
    CHECK(s == SV::kPrepWait);
    serve();
    sv.step(now_us());
  }
  CHECK(!"prepare_q never ended");
}
// With the card core `late` blocks behind (requests served every late blocks). Blocks [gap0, gap1): none served and the clock
// said to be past the rescue's wait (a request may be taken back there, and only there, on the control core)
static std::vector<float> queued(const char* name, const Play& p, int late, int gap0, int gap1) {
  prep(name);
  sv.trigger();
  std::vector<float> o;
  for (int b = 0; b < p.blocks; ++b) {
    uint64_t c = host_card_us(3);
    bool hold = b >= gap0 && b < gap1;
    sv.step(now_us() + (hold ? SV::kRescueWaitUs + 1000 : 0));
    while (sv.refill()) {}
    if (!hold) CHECK(host_card_us(3) == c);
    auto x = render(p, b);
    o.insert(o.end(), x.begin(), x.end());
    if (!hold && (b + 1) % late == 0) while (serve()) sv.step(now_us());
  }
  sv.clear();
  for (int i = 0; i < 6; ++i) {   // (its request's end, the close posted and run: before the worker goes off)
    sv.step(now_us());
    while (serve()) {}
  }
  return o;
}

int main() {
  char tmpl[] = "/tmp/core-streamq-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/Samples/S"));
  auto noise = [](uint32_t seed) {
    return [seed](int i, int c) {
      uint32_t x = ((uint32_t)i + (uint32_t)c * 977u + seed) * 2654435761u;
      x ^= x >> 13;
      return (float)(x * 2654435761u >> 8) / 16777216.0f - 0.5f;
    };
  };
  std::string card = tmpl;
  wg::write_file(card + "/Samples/S/st16.wav", wg::wav_bytes(5 * 48000, 2, 16, 48000, 1, "", noise(1)));
  wg::write_file(card + "/Samples/S/mo24.wav", wg::wav_bytes(5 * 48000, 1, 24, 48000, 1, "", noise(2)));
  wg::write_file(card + "/Samples/S/other.wav", wg::wav_bytes(3 * 48000, 2, 16, 48000, 1, "", noise(3)));
  CHECK(idx.begin(lg, scratch, sizeof scratch));
  sv.attach(head, ring);

  const Play plays[] = {{0.0f, false, 1.0f, 40000}, {12.0f, false, 1.0f, 40000}, {0.0f, true, 0.1f, 40000}, {12.0f, true, 0.03f, 40000}};
  host_card_model(true);
  for (const Play& p : plays)
    for (const char* n : {"st16.wav", "mo24.wav"}) {
      std::vector<float> want = old_way(n, p);
      CHECK(sv.underruns() == 0);
      cardq::set_worker(true);
      for (int late : {1, 8, 40}) {   // 0.25 ms .. 10 ms behind, all the time
        std::vector<float> got = queued(n, p, late, 0, 0);   // (no card time here: checked block by block)
        CHECK(got == want);
        CHECK(sv.underruns() == 0);
      }
      // ---- the card core away for 100 ms of audio: the ring carries it (a request taken back only if it runs low)
      uint32_t t0 = cardq::stats().taken_back;
      std::vector<float> got = queued(n, p, 8, 4000, 4400);
      CHECK(got == want && sv.underruns() == 0);
      if (p.semis == 0.0f && !p.loop) CHECK(cardq::stats().taken_back == t0);
      // ---- held up for good, from the start: every read taken back with 10 ms of audio left: still the same output
      t0 = cardq::stats().taken_back;
      got = queued(n, p, 1, 0, p.blocks);
      CHECK(got == want && sv.underruns() == 0);
      CHECK(cardq::stats().taken_back > t0 || p.loop);
      while (serve()) {}
      cardq::set_worker(false);
    }
  host_card_model(false);
  // ---- a re-trig with a read under way: its result dropped (not what follows the ring now), the output as a fresh start
  Play p{0.0f, false, 1.0f, 8000};
  std::vector<float> want = old_way("st16.wav", p);
  cardq::set_worker(true);
  prep("st16.wav");
  sv.trigger();
  for (int b = 0; b < 2000; ++b) {
    sv.refill();
    render(p, b);
    while (serve()) sv.step(now_us());
  }
  for (int i = 0; i < 2000 && cardq::pending() == 0; ++i) { render(p, i); sv.refill(); }
  CHECK(cardq::pending() == 1);
  sv.trigger();
  std::vector<float> got;
  for (int b = 0; b < p.blocks; ++b) {
    sv.step(now_us());
    sv.refill();
    auto x = render(p, b);
    got.insert(got.end(), x.begin(), x.end());
    while (serve()) sv.step(now_us());
  }
  CHECK(got == want && sv.underruns() == 0);
  // ---- a new sample with a read under way: the read ends, the file closes, nothing of it heard
  cardq::set_worker(false);
  std::vector<float> wother = old_way("other.wav", p);
  cardq::set_worker(true);
  prep("st16.wav");
  sv.trigger();
  for (int b = 0; b < 500; ++b) {
    sv.refill();
    render(p, b);
    while (serve()) sv.step(now_us());
  }
  for (int i = 0; i < 2000 && cardq::pending() == 0; ++i) { render(p, i); sv.refill(); }
  CHECK(cardq::pending() == 1);
  uint32_t cl = cardq::stats().ran[cardq::kCloseReq];
  prep("other.wav");
  CHECK(cardq::stats().ran[cardq::kCloseReq] == cl + 1);
  sv.trigger();
  got.clear();
  for (int b = 0; b < p.blocks; ++b) {
    sv.step(now_us());
    sv.refill();
    auto x = render(p, b);
    got.insert(got.end(), x.begin(), x.end());
    while (serve()) sv.step(now_us());
  }
  CHECK(got == wother && sv.underruns() == 0);
  sv.clear();
  sv.step(now_us());
  while (serve()) sv.step(now_us());
  // ---- a head the full queue refused: the new file not closed under it, the note plays on past its head
  {
    prep("st16.wav");
    int fo2;
    card::FileRec r2;
    static File f2;
    CHECK(find("other.wav", &fo2, &r2) && idx.open(fo2, r2, &f2));
    int smp2 = g_smp;
    CHECK(sv.prepare_q(r2, f2, fo2, smp2, 0.0f, false) == SV::kPrepWait);   // the old file's close posted
    while (serve()) {}
    sv.step(now_us());
    static cardq::Req dummy[cardq::kQueue];
    for (auto& d : dummy) {
      memset((void*)&d, 0, sizeof d);
      d.kind = cardq::kExistsReq;
      strcpy(d.path, "/none");
      CHECK(cardq::post(&d));
    }
    SV::Prep st = SV::kPrepWait;
    for (int i = 0; i < 300 && st == SV::kPrepWait; ++i) {
      st = sv.prepare_q(r2, f2, fo2, smp2, 0.0f, false);
      if (i >= 20) serve();
      sv.step(now_us());
    }
    CHECK(st == SV::kPrepOk);
    sv.trigger();
    got.clear();
    for (int b = 0; b < p.blocks; ++b) {
      sv.step(now_us());
      sv.refill();
      auto x = render(p, b);
      got.insert(got.end(), x.begin(), x.end());
      while (serve()) sv.step(now_us());
    }
    CHECK(got == wother && sv.underruns() == 0);
    sv.clear();
    for (int i = 0; i < 6; ++i) {
      sv.step(now_us());
      while (serve()) {}
    }
  }
  // ---- another sample with a head on its way: the head cancelled, it never runs
  {
    prep("st16.wav");
    int fa, fb;
    card::FileRec ra, rb;
    static File fx, fy;
    CHECK(find("other.wav", &fa, &ra) && idx.open(fa, ra, &fx));
    int sa = g_smp;
    CHECK(find("mo24.wav", &fb, &rb) && idx.open(fb, rb, &fy));
    int sb = g_smp;
    CHECK(sv.prepare_q(ra, fx, fa, sa, 0.0f, false) == SV::kPrepWait);
    while (serve()) {}
    sv.step(now_us());
    CHECK(sv.prepare_q(ra, fx, fa, sa, 0.0f, false) == SV::kPrepWait);   // the close done: other's head posted
    CHECK(cardq::pending() == 1);
    uint32_t c0 = cardq::stats().cancelled, r0 = cardq::stats().ran[cardq::kReadReq];
    CHECK(sv.prepare_q(rb, fy, fb, sb, 0.0f, false) == SV::kPrepWait);
    CHECK(cardq::stats().cancelled == c0 + 1);
    while (serve()) {}
    CHECK(cardq::stats().ran[cardq::kReadReq] == r0);
    SV::Prep st = SV::kPrepWait;
    for (int i = 0; i < 100 && st == SV::kPrepWait; ++i) {
      st = sv.prepare_q(rb, fy, fb, sb, 0.0f, false);
      serve();
      sv.step(now_us());
    }
    CHECK(st == SV::kPrepOk && !strcmp(sv.name(), "mo24.wav"));
    sv.clear();
    for (int i = 0; i < 6; ++i) {
      sv.step(now_us());
      while (serve()) {}
    }
  }
  // ---- a stale read is not taken back: a re-trig with one waiting, the clock past the wait, audio low
  {
    prep("st16.wav");
    sv.trigger();
    for (int b = 0; b < 3000; ++b) {
      sv.refill();
      render(p, b);
      while (serve()) sv.step(now_us());
    }
    for (int i = 0; i < 2000 && cardq::pending() == 0; ++i) { render(p, i); sv.refill(); }
    CHECK(cardq::pending() == 1);
    sv.trigger();                       // (the read is of a place the stream left)
    for (int b = 0; b < 3400 / 2; ++b) render(p, b);   // the head played out
    uint32_t t0 = cardq::stats().taken_back;
    sv.step(now_us() + SV::kRescueWaitUs + 1000);
    CHECK(cardq::stats().taken_back == t0);
    while (serve()) sv.step(now_us());
    sv.clear();
    for (int i = 0; i < 6; ++i) {
      sv.step(now_us());
      while (serve()) {}
    }
  }
  // ---- what the streams need reaches cardq::busy (voices.cpp streams_step): the recorder and uploads hold off
  CHECK(engine_begin());
  int fo;
  card::FileRec r;
  File f;
  v[0].dsps = true;
  SV& s0 = streams[0];
  CHECK(find("st16.wav", &fo, &r) && idx.open(fo, r, &f));
  while (s0.prepare_q(r, f, fo, g_smp, 0.0f, false) != SV::kPrepOk) { serve(); streams_step(); }
  s0.trigger();
  float l[kB], rr[kB];
  for (int b = 0; b < 400; ++b) {
    s0.render(0, 0, 1, false, 1.0f, l, kB, rr);
    while (s0.refill()) {}
    while (serve()) streams_step();
    streams_step();
  }
  CHECK(!cardq::busy());
  for (int b = 0; b < 3000; ++b) {   // the card core away: 750 ms of audio, no reads; the mark passed at half the ring
    s0.render(0, 0, 1, false, 1.0f, l, kB, rr);
    streams_step();
  }
  CHECK(cardq::busy());
  while (serve()) {}
  CHECK(!cardq::pending());
  s0.stop();
  streams_step();
  CHECK(!cardq::busy());
  // ---- kAppendReq: a part written says so; all written and the close failed says that
  {
    static cardq::Req q;
    memset((void*)&q, 0, sizeof q);
    cardq::set_worker(false);
    uint8_t txt[100];
    memset(txt, 'x', sizeof txt);
    host_short_write(30);
    CHECK(cardq::async(&q, cardq::kAppendReq, nullptr, "/Samples/app.txt", 0, txt, 100) && q.got == 30);
    host_fail_close("/Samples/app.txt", true);
    CHECK(cardq::async(&q, cardq::kAppendReq, nullptr, "/Samples/app.txt", 1, txt, 100) && q.got == cardq::kAppendUnsure);
    host_fail_close("/Samples/app.txt", false);
    cardq::set_worker(true);
  }
  // ---- eight streams held up, all low: one take-back a pass, the neediest's (a read on the control core is ~0.75 ms)
  for (int i = 0; i < 8; ++i) {
    v[i].dsps = true;
    CHECK(find("st16.wav", &fo, &r) && idx.open(fo, r, &f));
    while (streams[i].prepare_q(r, f, fo, g_smp, 0.0f, false) != SV::kPrepOk) { serve(); streams_step(); }
    streams[i].trigger();
  }
  for (int i = 0; i < 8; ++i) streams[i].refill();
  CHECK(cardq::pending() == 8);
  for (int b = 0; b < 3000 / 12 * 12; ++b)
    for (int i = 0; i < 8; ++i)
      if (streams[i].ahead(kB * 3) >= kB * 3) streams[i].render(i == 2 ? 0.0f : 0.0f, 0, 1, false, 1.0f, l, kB, rr);
  usleep(4000);
  uint32_t tb = cardq::stats().taken_back;
  streams_step();
  CHECK(cardq::stats().taken_back == tb + 1);
  while (serve()) {}
  streams_step();
  for (int i = 0; i < 8; ++i) streams[i].stop(), v[i].dsps = false;
  cardq::set_worker(false);
  printf("test_stream_cardq: PASS\n");
  return 0;
}

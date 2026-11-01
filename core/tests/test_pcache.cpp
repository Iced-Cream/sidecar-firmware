// core/tests/test_pcache.cpp - the pool RAM's shared sample cache (core/pool/pcache.h; the pools design 4.1, 7
// "Shared RAM"): a whole load through the card core (no control-core card time), int16 mono (stereo summed: poolmono=1), one entry
// per content (the same WAV at two paths: one copy, bytes counted once), a prefix (a machine's own fill) grown to whole
// with the old block kept while pinned, LRU eviction never of a pinned entry, the budget never exceeded, a prefetch
// never evicting, private blocks counted, forget.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "check.h"
#include "lib/library.h"
#include "msamp/cardq.h"
#include "platform.h"
#include "pool/pcache.h"
#include "wg_util.h"

using namespace core;

static std::string g_card;
static uint64_t g_us = 50000000;
static uint64_t g_card3 = 0;
static void run(int n) {
  for (int i = 0; i < n; ++i) {
    uint64_t c3 = host_card_us(3);
    pcache::step();
    g_card3 += host_card_us(3) - c3;
    host_card_core(0);
    for (int k = 0; k < 8 && cardq::service(); ++k) {}
    host_card_core(3);
    g_us += 1000;
    host_fake_time(true, g_us);
  }
}
static lib::Ref ref(const char* p) {
  lib::Ref r;
  CHECK(lib::ref_for_path(p, &r) && r.has_sha);
  return r;
}
static int load(const char* p) {
  lib::Ref r = ref(p);
  int e = pcache::want(r, p);
  CHECK(e >= 0);
  for (int i = 0; i < 4000 && pcache::state(e) == pcache::kLoading; ++i) run(1);
  return e;
}

int main() {
  char tmpl[] = "/tmp/pcache-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/A' '" + g_card + "/B'").c_str()) == 0);
  auto sine = [](double hz, double a) { return [hz, a](int i, int c) { return (c ? 0.5 : 1.0) * a * std::sin(2 * M_PI * hz * i / 48000); }; };
  // 1 s stereo 16-bit (L and half-level R in phase: mono = 0.75 x L), the same bytes at a second path; 3 s mono
  wg::write_file(g_card + "/A/st.wav", wg::wav_bytes(48000, 2, 16, 48000, 1, "", sine(440, 0.5)));
  wg::write_file(g_card + "/B/st copy.wav", wg::wav_bytes(48000, 2, 16, 48000, 1, "", sine(440, 0.5)));
  for (int i = 0; i < 6; ++i)
    wg::write_file(g_card + "/A/m" + std::to_string(i) + ".wav", wg::wav_bytes(4 * 48000, 1, 16, 48000, 1, "", sine(100 + 50 * i, 0.4)));
  wg::write_file(g_card + "/A/bad.wav", std::vector<uint8_t>(80, 3));
  host_set_card(g_card.c_str());
  host_fake_time(true, g_us);
  card_ok = true;
  lib::begin(true);
  for (int i = 0; i < 200000 && (i < 50 || lib::busy() || lib::stats().state != lib::kReady); ++i) lib::step();
  cardq::reset();
  cardq::set_worker(true);
  host_card_model(true);
  // 2 MB: each 4 s mono sample is 384 KB, the stereo one 96 KB (summed: poolmono=1, the layout before stereo samples;
  // native stereo entries are test_stereo's)
  pcache::set_mono(true);
  pcache::begin(2u << 20);
  pcache::begin(64u << 20);   // (a second call changes nothing)
  CHECK(pcache::ok() && pcache::stats().budget == 2u << 20);

  // ---- a whole load: int16 mono at the file's rate, the guard frame; the card core's work only
  int st = load("/A/st.wav");
  pcache::Info in;
  CHECK(pcache::state(st) == pcache::kReady && pcache::get(st, &in) && in.whole && !in.cut && in.frames == 48000 && in.rate == 48000);
  double err = 0;
  for (int i = 0; i < 48000; ++i) err = std::max(err, std::fabs(in.pcm[i] / 32768.0 - 0.75 * 0.5 * std::sin(2 * M_PI * 440 * i / 48000)));
  CHECK(err < 2e-3 && in.pcm[48000] == in.pcm[47999]);
  CHECK(g_card3 == 0);
  // ---- the same content at another path: the same entry, its bytes once
  uint64_t used = pcache::stats().used;
  lib::Ref copy = ref("/B/st copy.wav");
  CHECK(pcache::find(copy, pcache::kWhole) == st && pcache::want(copy, "/B/st copy.wav") == st);
  CHECK(pcache::stats().used == used && pcache::stats().entries == 1 && pcache::stats().shared_hits >= 1);
  // a file it cannot read: failed, not tried again until forgotten
  int bad = load("/A/bad.wav");
  CHECK(pcache::state(bad) == pcache::kFailed);

  // ---- a prefix filled by a machine (MULTISAMPLE's head), then a whole want grows it (the old block kept while pinned)
  lib::Ref m0 = ref("/A/m0.wav");
  int16_t* dst = nullptr;
  int have = -1;
  int p = pcache::fill_begin(m0, 4800, 4 * 48000, 48000, &dst, &have);
  CHECK(p >= 0 && dst && have < 0);
  for (int i = 0; i < 4800; ++i) dst[i] = (int16_t)(i & 0x7fff);
  pcache::fill_end(p, 4800, true);
  CHECK(pcache::get(p, &in) && !in.whole && in.frames == 4800 && in.total == 4 * 48000);
  CHECK(pcache::find(m0, 4800) == p && pcache::find(m0, pcache::kWhole) < 0);
  int16_t* d2 = nullptr;
  CHECK(pcache::fill_begin(m0, 2400, 4 * 48000, 48000, &d2, &have) < 0 && have == p);   // a shorter prefix: there already
  int oldb = in.blk;
  pcache::pin(oldb);   // a MULTISAMPLE note reads the head
  const int16_t* oldp = in.pcm;
  CHECK(pcache::want(m0, "/A/m0.wav") == p);
  for (int i = 0; i < 4000 && !(pcache::get(p, &in) && in.whole); ++i) run(1);
  CHECK(pcache::get(p, &in) && in.whole && in.frames == 4 * 48000 && in.blk != oldb && pcache::stats().grows == 1);
  CHECK(oldp[100] == 100);   // the old block still there for its reader
  uint64_t both = pcache::stats().used;
  pcache::unpin(oldb);       // the reader done: the old block freed
  CHECK(pcache::stats().used < both);
  CHECK(pcache::find(m0, pcache::kWhole) == p && pcache::find(m0, 4800) == p);   // whole satisfies a prefix

  // ---- the budget: loads past it evict the least recently used unpinned entries; a pinned one stays
  CHECK(pcache::get(st, &in));
  pcache::pin(in.blk);   // the stereo sample plays
  for (int i = 1; i < 6; ++i) {
    std::string path = "/A/m" + std::to_string(i) + ".wav";
    CHECK(pcache::state(load(path.c_str())) == pcache::kReady);
    CHECK(pcache::stats().used <= pcache::stats().budget);
  }
  CHECK(pcache::stats().evictions >= 1);
  CHECK(pcache::state(st) == pcache::kReady);   // pinned: kept
  CHECK(pcache::find(ref("/A/m5.wav"), pcache::kWhole) >= 0 && pcache::find(ref("/A/m4.wav"), pcache::kWhole) >= 0);
  printf("test_pcache: 6 x 384 KB + 96 KB into 2 MB: %u evictions, %llu KB used\n", pcache::stats().evictions,
         (unsigned long long)(pcache::stats().used >> 10));
  pcache::unpin(in.blk);
  // a prefetch never evicts: the arena full, an evicted file is refused for now (a want gets it back)
  int gone = -1;
  for (int i = 0; i < 6 && gone < 0; ++i) {
    std::string path = "/A/m" + std::to_string(i) + ".wav";
    if (pcache::find(ref(path.c_str()), pcache::kWhole) < 0) gone = i;
  }
  CHECK(gone >= 0);
  std::string gp = "/A/m" + std::to_string(gone) + ".wav";
  uint32_t ev0 = pcache::stats().evictions;
  int pf = pcache::want(ref(gp.c_str()), gp.c_str(), true);
  run(400);
  CHECK(pcache::stats().evictions == ev0 && pcache::stats().used <= pcache::stats().budget);
  CHECK(pf < 0 || pcache::state(pf) != pcache::kReady);
  int wv = pcache::want(ref(gp.c_str()), gp.c_str());   // a voice wants it: room made
  for (int i = 0; i < 4000 && pcache::state(wv) == pcache::kLoading; ++i) run(1);
  CHECK(pcache::state(wv) == pcache::kReady && pcache::stats().evictions > ev0);
  // a kept block gives way like an evictable one (BACKLOG 7ad): only when a window makes room, only its one pin on it; its
  // first frames move to the window's far end, the block and pin stay, the rest is room; no window: nothing happens
  {
    static int g_blk;
    static const int16_t* g_pcm;
    g_blk = -1;
    pcache::set_give_back([](int b, const int16_t* p) { g_blk = b, g_pcm = p; });
    pcache::Info w;
    CHECK(pcache::get(wv, &w) && w.whole && w.frames > 2000);
    std::vector<int16_t> first(w.pcm, w.pcm + 1000);
    pcache::pin(w.blk), pcache::pin(w.blk);
    pcache::mark_kept(w.blk, 1000);
    CHECK(pcache::stats().kept > 0);
    g_us += (pcache::kShrinkGuardMs + 1) * 1000;
    run(1);
    std::vector<int> priv;
    for (;;) {   // two pins: never given, whatever is asked for
      void* m = nullptr;
      int b = pcache::alloc_private(64 * 1024, &m);
      if (b < 0) break;
      priv.push_back(b);
    }
    CHECK(g_blk < 0 && !priv.empty());
    pcache::unpin(w.blk);
    void* m = nullptr;
    uint32_t sh = pcache::stats().shrinks;
    int b1 = pcache::alloc_private(64 * 1024, &m);   // (the block is all that can give room)
    CHECK(b1 >= 0);
    priv.push_back(b1);
    CHECK(g_blk == w.blk && pcache::stats().shrinks == sh + 1);
    pcache::Info p;
    CHECK(pcache::get(wv, &p) && !p.whole && p.frames == 1000 && p.pcm == g_pcm && p.blk == w.blk);
    CHECK(!memcmp(p.pcm, first.data(), 2000) && p.pcm[1000] == first[999]);   // (the guard frame)
    CHECK(pcache::find(ref(gp.c_str()), pcache::kWhole) < 0 && pcache::find(ref(gp.c_str()), 1000) == wv);
    CHECK(pcache::stats().kept == 0);
    for (int b : priv) pcache::release(b);
    pcache::unpin(w.blk);
    pcache::set_give_back(nullptr);
    int wg = pcache::want(ref(gp.c_str()), gp.c_str());   // whole again
    for (int i = 0; i < 4000 && pcache::busy(wg); ++i) run(1);
    CHECK(pcache::find(ref(gp.c_str()), pcache::kWhole) == wg);
  }
  // several kept blocks in one window, adjacent: the first keeps its frames where they are, the next's move up, each
  // intact, the guard frames right; the moves bounded (kMoveMax): a window that needs more is not taken; a minimum idle
  // time (a fill's) leaves a block used lately
  {
    static int g_n;
    static int g_bl[8];
    static const int16_t* g_pc[8];
    g_n = 0;
    pcache::set_give_back([](int b, const int16_t* p) { g_bl[g_n] = b, g_pc[g_n++] = p; });
    for (const char* n : {"/A/m0.wav", "/A/m1.wav", "/A/m2.wav", "/A/m3.wav", "/A/m4.wav", "/A/m5.wav"}) pcache::forget(ref(n));
    std::vector<int> ph;   // the rest of the arena private
    auto fill_up = [&ph]() {
      for (;;) {
        void* m = nullptr;
        int b = pcache::alloc_private(4 * 1024, &m);
        if (b < 0) break;
        ph.push_back(b);
      }
    };
    struct K { int e, blk; std::vector<int16_t> d; };
    std::vector<K> ks;
    const uint32_t kF = 90000, kHead = 20000;
    for (int i = 0; i < 3; ++i) {
      lib::Ref r{};
      snprintf(r.hint, sizeof r.hint, "/fake/%d", i);
      int16_t* dst = nullptr;
      int have = -1;
      int e = pcache::fill_begin(r, kF, kF, 48000, &dst, &have);
      CHECK(e >= 0);
      for (uint32_t k = 0; k < kF; ++k) dst[k] = (int16_t)(k * (i + 3));
      pcache::fill_end(e, kF, true);
      pcache::Info in;
      CHECK(pcache::get(e, &in) && in.whole);
      pcache::pin(in.blk);
      pcache::mark_kept(in.blk, kHead);
      ks.push_back({e, in.blk, std::vector<int16_t>(in.pcm, in.pcm + kHead)});
    }
    fill_up();
    g_us += (pcache::kShrinkGuardMs + 1) * 1000;
    run(1);
    void* m = nullptr;
    pcache::set_give_min_idle(3600 * 1000);   // none idle that long
    CHECK(pcache::alloc_private(200 * 1024, &m) < 0 && g_n == 0);
    pcache::set_give_min_idle(0);
    CHECK(pcache::alloc_private(1600 * 1024, &m) < 0 && g_n == 0);   // (more than the three can give)
    int big = pcache::alloc_private(250 * 1024, &m);   // over two of them: one head moved
    CHECK(big >= 0 && g_n == 2);
    for (int i = 0; i < g_n; ++i) {
      int k = -1;
      for (int j = 0; j < 3; ++j)
        if (ks[j].blk == g_bl[i]) k = j;
      CHECK(k >= 0);
      pcache::Info p;
      CHECK(pcache::get(ks[k].e, &p) && !p.whole && p.frames == kHead && p.pcm == g_pc[i]);
      CHECK(!memcmp(p.pcm, ks[k].d.data(), kHead * 2) && p.pcm[kHead] == ks[k].d[kHead - 1]);
    }
    CHECK(pcache::stats().used <= pcache::stats().budget);
    pcache::set_give_back(nullptr);
    pcache::release(big);
    for (int b : ph) pcache::release(b);
    for (K& k : ks) pcache::unpin(k.blk), pcache::unmark_kept(k.blk);
    for (int i = 0; i < 3; ++i) {
      lib::Ref r{};
      snprintf(r.hint, sizeof r.hint, "/fake/%d", i);
      pcache::forget(r);
    }
    int wg = pcache::want(ref(gp.c_str()), gp.c_str());   // (the rest of this test's file back)
    for (int i = 0; i < 4000 && pcache::busy(wg); ++i) run(1);
  }
  // private blocks: counted, never evicted
  void* mem = nullptr;
  uint64_t u0 = pcache::stats().used;
  int pb = pcache::alloc_private(100000, &mem);
  CHECK(pb >= 0 && mem && pcache::stats().priv >= 100000 && pcache::stats().used <= pcache::stats().budget);
  (void)u0;
  pcache::release(pb);
  CHECK(pcache::stats().priv == 0);
  // forget: an unloaded row's content goes (nothing pins it)
  CHECK(pcache::find(ref(gp.c_str()), pcache::kWhole) >= 0);
  pcache::forget(ref(gp.c_str()));
  CHECK(pcache::find(ref(gp.c_str()), 0) < 0);
  // ---- a fill and a whole load of the same content never run at once (the review's H2 / H3 / H4)
  {
    pcache::reset_for_tests();
    lib::Ref a = ref("/A/m1.wav"), b = ref("/A/m2.wav"), c = ref("/A/m3.wav");
    // a queued (not started) whole load: a fill takes it over, the whole load follows the fill
    int qa = pcache::want(b, "/A/m2.wav");   // (queued: not stepped yet)
    int qa2 = pcache::want(a, "/A/m1.wav");
    CHECK(qa >= 0 && qa2 >= 0 && pcache::busy(qa2));
    int16_t* d = nullptr;
    int have = -1;
    int f = pcache::fill_begin(a, 4800, 4 * 48000, 48000, &d, &have);
    CHECK(f == qa2 && d && pcache::busy(f));   // (its whole load after the fill)
    for (int i = 0; i < 4800; ++i) d[i] = (int16_t)i;
    run(200);   // the other load runs meanwhile; the filled entry is not loaded under the fill
    CHECK(pcache::state(f) == pcache::kLoading);
    pcache::fill_end(f, 4800, true);
    CHECK(pcache::get(f, &in) && !in.whole && in.frames == 4800 && pcache::busy(f));
    for (int i = 0; i < 4000 && !(pcache::get(f, &in) && in.whole); ++i) run(1);
    CHECK(pcache::get(f, &in) && in.whole && !pcache::busy(f));
    // a want while a fill runs: deferred to the fill's end
    int16_t* d2 = nullptr;
    int g = pcache::fill_begin(c, 2400, 4 * 48000, 48000, &d2, &have);
    CHECK(g >= 0);
    CHECK(pcache::want(c, "/A/m3.wav") == g && pcache::state(g) == pcache::kLoading);
    run(100);
    CHECK(pcache::state(g) == pcache::kLoading);   // nothing loaded under the fill
    memset(d2, 0, 2400 * 2);
    pcache::fill_end(g, 2400, true);
    for (int i = 0; i < 4000 && !(pcache::get(g, &in) && in.whole); ++i) run(1);
    CHECK(pcache::get(g, &in) && in.whole);
    // forgotten while it fills: dropped at the fill's end, nothing leaked
    lib::Ref m4 = ref("/A/m4.wav");
    int16_t* d3 = nullptr;
    int h = pcache::fill_begin(m4, 4800, 4 * 48000, 48000, &d3, &have);
    CHECK(h >= 0);
    pcache::forget(m4);
    CHECK(pcache::is(h, m4));   // (still there: the machine writes its block)
    pcache::fill_end(h, 4800, true);
    CHECK(!pcache::is(h, m4) && pcache::find(m4, 0) < 0);
    // everything forgotten: no block left behind
    pcache::forget(a), pcache::forget(b), pcache::forget(c);
    run(50);
    CHECK(pcache::stats().used == pcache::stats().priv);
  }
  CHECK(g_card3 == 0);   // every load on the card core
  host_card_model(false);
  cardq::reset();
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_pcache: PASS (%u loads, %u grows, %u shared)\n", pcache::stats().loads, pcache::stats().grows,
         pcache::stats().shared_hits);
  return 0;
}

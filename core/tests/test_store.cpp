// core/tests/test_store.cpp - the RAM store (core/store/store.h): put/get round trip, replace, mark frees, unknown path,
// the entry cap, and that clear() gives every byte back; the fill (want / fill_begin / fill_step / ready): present,
// absent and locked paths, every card operation on the card core, and a refill that abandons the one before. (The full
// engine's project load through the store - its LFO, insert and sequencer files - was cut in the strip-down.)
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "check.h"
#include "../fmt/fmt.h"
#include "../host/core_port.h"
#include "../msamp/cardq.h"
#include "../store/store.h"

using namespace core::store;
namespace cardq = core::cardq;

static std::string g_card;
static void card_file(const char* path, const char* bytes) {
  std::string p = g_card + path;
  CHECK(system(("mkdir -p \"$(dirname '" + p + "')\"").c_str()) == 0);
  FILE* f = fopen(p.c_str(), "wb");
  CHECK(f);
  fwrite(bytes, 1, strlen(bytes), f);
  fclose(f);
}
// one control-core idle pass, then (with a card core) the card core's turn: every posted request run on core 0
static bool pass() {
  bool done = fill_step();
  if (cardq::worker()) {
    core::host_card_core(0);
    while (cardq::service()) {
    }
    core::host_card_core(3);
  }
  return done;
}
static bool run_fill() {
  for (int i = 0; i < 10000; ++i)
    if (pass()) return true;
  return false;
}

static void fill_reads_present_absent_locked() {
  const uint8_t* d = nullptr;
  uint32_t n = 0;
  clear();
  core::host_fail_open("/.rytm/lfo/P/k003.txt", true);
  want("/.rytm/lfo/P/k002.txt");
  want("/.rytm/lfo/P/k003.txt");
  want("/.rytm/lfo/P/k004.txt");
  CHECK(!ready());
  fill_begin();
  CHECK(!ready());
  CHECK(run_fill());
  CHECK(ready());
  CHECK(get("/.rytm/lfo/P/k002.txt", &d, &n) == kPresent && n == 1 && d[0] == 'x');
  CHECK(get("/.rytm/lfo/P/k003.txt", &d, &n) == kLocked && n == 0);   // there, but the open failed
  CHECK(get("/.rytm/lfo/P/k004.txt", &d, &n) == kAbsent && n == 0);
  CHECK(fill_step());   // a finished fill stays finished
  core::host_fail_open("/.rytm/lfo/P/k003.txt", false);
  clear();
  CHECK(!ready() && entries() == 0 && bytes() == 0);
}

static void fill_charges_the_control_core_nothing() {
  const uint8_t* d = nullptr;
  uint32_t n = 0;
  clear();
  cardq::reset();
  cardq::set_worker(true);
  core::host_card_model(true);
  want("/.rytm/lfo/P/k002.txt");
  want("/.rytm/lfo/P/k004.txt");
  want("/.rytm/lfo/P/k005.txt");
  uint64_t ctl = core::host_card_us(3);
  fill_begin();
  CHECK(run_fill());
  CHECK(core::host_card_us(3) == ctl);   // the control core charged nothing: every open / read / exists on core 0
  CHECK(core::host_card_us(0) > 0);
  CHECK(get("/.rytm/lfo/P/k002.txt", &d, &n) == kPresent && n == 1);
  CHECK(get("/.rytm/lfo/P/k005.txt", &d, &n) == kPresent && n == 5 && !memcmp(d, "hello", 5));
  CHECK(get("/.rytm/lfo/P/k004.txt", &d, &n) == kAbsent);
  CHECK(cardq::stats().taken_back == 0);
  core::host_card_model(false);
  cardq::reset();
  clear();
}

static void refill_abandons_the_old_one() {
  const uint8_t* d = nullptr;
  uint32_t n = 0;
  for (int worker = 0; worker < 2; ++worker) {
    clear();
    cardq::reset();
    cardq::set_worker(worker != 0);
    want("/.rytm/lfo/P/k002.txt");
    want("/.rytm/lfo/P/k005.txt");
    fill_begin();
    CHECK(!pass());   // the folder's listing
    CHECK(!pass());   // the first path's open
    CHECK(!pass());   // its size, the buffer
    CHECK(!pass());   // its read: with a card core, done there but not yet taken
    fill_begin();     // the same paths again: the half-done fill abandoned, its file closed, then all of it anew
    CHECK(!ready());
    CHECK(run_fill() && ready());
    CHECK(bytes() == 6 && entries() == 2);
    CHECK(get("/.rytm/lfo/P/k005.txt", &d, &n) == kPresent && n == 5 && !memcmp(d, "hello", 5));

    fill_begin();
    CHECK(!pass());        // the listing
    CHECK(!fill_step());
    CHECK(!fill_step());   // the open posted: with a card core, still waiting there
    uint32_t cancelled = cardq::stats().cancelled;
    clear();
    CHECK(cardq::stats().cancelled == cancelled + (worker ? 1u : 0u));   // taken off the queue, never run          // a project switch: the old fill dropped with the old paths, then the new project's
    want("/.rytm/lfo/Q/k001.txt");
    want("/.rytm/lfo/Q/k009.txt");
    fill_begin();
    CHECK(!ready());
    CHECK(run_fill());
    CHECK(ready());
    CHECK(bytes() == 3 && entries() == 2);   // only the second fill's: "abc" and an absent path
    CHECK(get("/.rytm/lfo/Q/k001.txt", &d, &n) == kPresent && n == 3 && !memcmp(d, "abc", 3));
    CHECK(get("/.rytm/lfo/Q/k009.txt", &d, &n) == kAbsent);
    CHECK(get("/.rytm/lfo/P/k002.txt", &d, &n) == kUnknown);
    CHECK(get("/.rytm/lfo/P/k005.txt", &d, &n) == kUnknown);
  }
  cardq::reset();
  clear();
}

// ruling 2 (2026-10-26): the fill lists each wanted path's folder once (cardq kListReq, on the card core) instead of an
// open - and an exists for every path that is not there - per path: at a boot it took 7.3 s for 258 paths, nearly all
// of them absent per-kit files. An absent path costs no card operation; a present one its open, read and close; a
// listed one that will not open is locked (no exists: the listing said it is there); a folder that is not there makes
// all of its paths absent.
struct Ops {
  uint32_t ran[cardq::kKinds];
};
static Ops ops_now() {
  Ops o;
  memcpy(o.ran, cardq::stats().ran, sizeof o.ran);
  return o;
}
static uint32_t ops(const Ops& a, const Ops& b, cardq::Kind k) { return b.ran[k] - a.ran[k]; }
static uint32_t ops_all(const Ops& a, const Ops& b) {
  uint32_t n = 0;
  for (int k = 0; k < cardq::kKinds; ++k) n += b.ran[k] - a.ran[k];
  return n;
}
static void fill_lists_each_folder_once() {
  const uint8_t* d = nullptr;
  uint32_t n = 0;
  clear();
  cardq::reset();
  cardq::set_worker(true);
  card_file("/.rytm/lfo/L/k002.txt", "x");
  card_file("/.rytm/lfo/L/k003.txt", "locked");
  card_file("/.rytm/lfo/L.txt", "own");
  card_file("/.rytm/lfo/OTHER.txt", "another project's");
  core::host_fail_open("/.rytm/lfo/L/k003.txt", true);
  want("/.rytm/lfo/L.txt");
  for (int k = 1; k <= 128; ++k) {
    char p[40];
    sniprintf(p, sizeof p, "/.rytm/lfo/L/k%03d.txt", k);
    want(p);
  }
  for (int k = 0; k < 128; ++k) {   // a folder that is not there
    char p[40];
    sniprintf(p, sizeof p, "/.rytm/seq/NONE/p%03d.sqp", k);
    want(p);
  }
  want("/.rytm/lfo/L2.txt");   // the first folder again, after the others: still listed once
  Ops a = ops_now();
  fill_begin();
  CHECK(run_fill());
  Ops b = ops_now();
  printf("store: a fill of 258 paths in 3 folders (2 files, 1 locked): list %u, open %u, read %u, close %u, exists %u\n",
         ops(a, b, cardq::kListReq), ops(a, b, cardq::kOpenReq), ops(a, b, cardq::kReadReq), ops(a, b, cardq::kCloseReq),
         ops(a, b, cardq::kExistsReq));
  CHECK(ops(a, b, cardq::kListReq) == 3);    // /.rytm/lfo, /.rytm/lfo/L, /.rytm/seq/NONE: each once
  CHECK(ops(a, b, cardq::kOpenReq) == 3);    // L.txt, k002, k003 (the one that will not open)
  CHECK(ops(a, b, cardq::kReadReq) == 2 && ops(a, b, cardq::kCloseReq) == 2);
  CHECK(ops(a, b, cardq::kExistsReq) == 0);  // absent: the listing said so; locked: the listing said it is there
  CHECK(ops_all(a, b) == 10);
  CHECK(get("/.rytm/lfo/L.txt", &d, &n) == kPresent && n == 3);
  CHECK(get("/.rytm/lfo/L/k002.txt", &d, &n) == kPresent && n == 1 && d[0] == 'x');
  CHECK(get("/.rytm/lfo/L/k003.txt", &d, &n) == kLocked);
  CHECK(get("/.rytm/lfo/L/k004.txt", &d, &n) == kAbsent && get("/.rytm/lfo/L/k128.txt", &d, &n) == kAbsent);
  CHECK(get("/.rytm/seq/NONE/p000.sqp", &d, &n) == kAbsent && get("/.rytm/seq/NONE/p127.sqp", &d, &n) == kAbsent);
  CHECK(get("/.rytm/lfo/L2.txt", &d, &n) == kAbsent);
  CHECK(entries() == 258 && present() == 2);
  core::host_fail_open("/.rytm/lfo/L/k003.txt", false);
  cardq::reset();
  clear();
}

// a folder whose names do not fit the fill's listing buffer: its paths asked one by one as before (an open, then an
// exists for one that will not open) - never a name taken for absent because the list was cut short
static void fill_unlisted_folder_asks_per_path() {
  const uint8_t* d = nullptr;
  uint32_t n = 0;
  clear();
  cardq::reset();
  cardq::set_worker(true);
  std::string dir = g_card + "/.rytm/lfo/BIG";
  CHECK(system(("mkdir -p '" + dir + "'").c_str()) == 0);
  for (int i = 0; i < 700; ++i) {   // ~30 KB of names
    char p[80];
    sniprintf(p, sizeof p, "/.rytm/lfo/BIG/a_rather_long_file_name_%04d.txt", i);
    card_file(p, "z");
  }
  card_file("/.rytm/lfo/BIG/k001.txt", "one");
  want("/.rytm/lfo/BIG/k001.txt");
  want("/.rytm/lfo/BIG/k002.txt");
  Ops a = ops_now();
  fill_begin();
  CHECK(run_fill());
  Ops b = ops_now();
  CHECK(ops(a, b, cardq::kListReq) == 1 && ops(a, b, cardq::kOpenReq) == 2 && ops(a, b, cardq::kExistsReq) == 1);
  CHECK(get("/.rytm/lfo/BIG/k001.txt", &d, &n) == kPresent && n == 3);
  CHECK(get("/.rytm/lfo/BIG/k002.txt", &d, &n) == kAbsent);
  CHECK(system(("rm -rf '" + dir + "'").c_str()) == 0);
  printf("store: a folder too big to list: its paths asked one by one\n");
  cardq::reset();
  clear();
}

// a new file's write queued when its folder is listed, landing before the path's turn: the listing (without it) is
// never taken for its absence - the path is opened, the written bytes held
static void fill_lists_around_a_pending_write() {
  const uint8_t* d = nullptr;
  uint32_t n = 0;
  clear();
  cardq::reset();
  cardq::set_worker(true);
  card_file("/.rytm/lfo/N/k001.txt", "first");
  char* t = core::fmt::post_begin();   // the post slot busy: the new file's write queued behind it
  CHECK(t);
  memcpy(t, "s\n", 2);
  CHECK(core::fmt::post_end("/.rytm/q/slot.txt", 2));
  CHECK(core::fmt::post_queue("/.rytm/lfo/N/k002.txt", "new\n", 4));
  want("/.rytm/lfo/N/k001.txt");
  want("/.rytm/lfo/N/k002.txt");
  fill_begin();
  CHECK(!pass());   // the listing (k002 not there yet: its write still queued)
  for (int i = 0; i < 100 && core::fmt::pending("/.rytm/lfo/N/k002.txt"); ++i) core::fmt::post_step(), pass();
  CHECK(!core::fmt::pending("/.rytm/lfo/N/k002.txt"));   // landed before k002's turn
  CHECK(run_fill());
  CHECK(get("/.rytm/lfo/N/k001.txt", &d, &n) == kPresent && n == 5);
  CHECK(get("/.rytm/lfo/N/k002.txt", &d, &n) == kPresent && n == 4 && !memcmp(d, "new\n", 4));
  printf("store: a write pending at its folder's listing: the path opened once it landed, not taken for absent\n");
  cardq::reset();
  clear();
}

// final review (Minor): a fill waiting for a path's write the card core does not take (here: never stepped) says so -
// one line once it has waited 2 s, again at most every 10 s - then completes once the write lands
static std::vector<std::string> g_lines;
static void line_sink(const char* l, int n) { g_lines.push_back(std::string(l, (size_t)n)); }
static void fill_waiting_says_why() {
  const uint8_t* d = nullptr;
  uint32_t n = 0;
  clear();
  cardq::reset();
  cardq::set_worker(true);
  char* t = core::fmt::post_begin();   // the post slot busy: the path's write queued behind it, never stepped here
  CHECK(t);
  memcpy(t, "s\n", 2);
  CHECK(core::fmt::post_end("/.rytm/q/slot.txt", 2));
  CHECK(core::fmt::post_queue("/.rytm/lfo/W/k001.txt", "new\n", 4));
  want("/.rytm/lfo/W/k001.txt");
  g_lines.clear();
  core::host_set_log_sink(line_sink);
  uint64_t us = 5000000000ull;
  core::host_fake_time(true, us);
  fill_begin();
  auto said = [] {
    int k = 0;
    for (const std::string& l : g_lines) k += l.find("fill waiting for /.rytm/lfo/W/k001.txt's write") != std::string::npos;
    return k;
  };
  for (int i = 0; i < 100; ++i) { CHECK(!pass()); core::host_fake_time(true, us += 10000); }   // 1 s
  CHECK(said() == 0);   // (a short wait: nothing said)
  for (int i = 0; i < 200; ++i) { CHECK(!pass()); core::host_fake_time(true, us += 10000); }   // 3 s
  CHECK(said() == 1);
  for (int i = 0; i < 1100; ++i) { CHECK(!pass()); core::host_fake_time(true, us += 10000); }   // 14 s
  CHECK(said() == 2);   // (rate-limited)
  core::host_set_log_sink(nullptr);
  for (int i = 0; i < 100 && core::fmt::pending("/.rytm/lfo/W/k001.txt"); ++i) core::fmt::post_step(), pass();
  CHECK(run_fill() && get("/.rytm/lfo/W/k001.txt", &d, &n) == kPresent && n == 4);
  core::host_fake_time(false, 0);
  printf("store: a fill waiting for a write the card core has not taken says so (rate-limited)\n");
  cardq::reset();
  clear();
}

// fix round 1: a card error is never "the end of the folder" or "no such folder". A readdir that fails in the middle
// (host_fail_readdir) or a lookup the card cannot answer (host_fail_lookup) leaves the folder unlisted: its paths are
// asked one by one, and none is taken for absent because of the error (a save would write over a real file)
static void fill_listing_errors_ask_per_path() {
  const uint8_t* d = nullptr;
  uint32_t n = 0;
  clear();
  cardq::reset();
  cardq::set_worker(true);
  for (int k = 1; k <= 4; ++k) {
    char p[40];
    sniprintf(p, sizeof p, "/.rytm/lfo/RD/k%03d.txt", k);
    card_file(p, "here");
  }
  for (int k = 1; k <= 8; ++k) {
    char p[40];
    sniprintf(p, sizeof p, "/.rytm/lfo/RD/k%03d.txt", k);
    want(p);
  }
  want("/.rytm/seq/GONE/p000.sqp");   // a folder the card cannot tell about
  core::host_fail_readdir("/.rytm/lfo/RD", 2, true);
  core::host_fail_lookup("/.rytm/seq/GONE", true);
  Ops a = ops_now();
  fill_begin();
  CHECK(run_fill());
  Ops b = ops_now();
  core::host_fail_readdir("/.rytm/lfo/RD", 0, false);
  core::host_fail_lookup("/.rytm/seq/GONE", false);
  for (int k = 1; k <= 8; ++k) {
    char p[40];
    sniprintf(p, sizeof p, "/.rytm/lfo/RD/k%03d.txt", k);
    CHECK(get(p, &d, &n) == (k <= 4 ? kPresent : kAbsent));   // k003 and k004 were after the error: still there
  }
  CHECK(get("/.rytm/seq/GONE/p000.sqp", &d, &n) == kAbsent);   // asked itself: the card said no
  CHECK(ops(a, b, cardq::kListReq) == 2 && ops(a, b, cardq::kOpenReq) == 9 && ops(a, b, cardq::kExistsReq) == 5);
  printf("store: a readdir error mid-folder and an unanswered lookup: their paths asked one by one, none taken for absent\n");
  // a plain file where a folder is wanted: not listed (closed again), its path asked
  card_file("/.rytm/lfo/PF", "x");
  clear();
  want("/.rytm/lfo/PF/k001.txt");
  fill_begin();
  CHECK(run_fill() && get("/.rytm/lfo/PF/k001.txt", &d, &n) == kAbsent);
#ifdef __APPLE__   // (the host card is a folder: only the Mac's file system ignores case, as FAT does)
  // FAT names have no case: a P000.SQP copied onto the card by hand is p000.sqp's file
  card_file("/.rytm/seq/CASE/P000.SQP", "upper");
  clear();
  want("/.rytm/seq/CASE/p000.sqp");
  want("/.rytm/seq/CASE/p001.sqp");
  a = ops_now();
  fill_begin();
  CHECK(run_fill());
  b = ops_now();
  CHECK(get("/.rytm/seq/CASE/p000.sqp", &d, &n) == kPresent && n == 5 && get("/.rytm/seq/CASE/p001.sqp", &d, &n) == kAbsent);
  CHECK(ops(a, b, cardq::kListReq) == 1 && ops(a, b, cardq::kOpenReq) == 1 && ops(a, b, cardq::kExistsReq) == 0);
  printf("store: the listing matches names without case (P000.SQP)\n");
#endif
  cardq::reset();
  clear();
}

int main() {
  const uint8_t* d = nullptr;
  uint32_t n = 99;
  clear();
  CHECK(get("a/b.txt", &d, &n) == kUnknown);   // never seen: unknown (callers treat it as locked)

  const uint8_t one[5] = {1, 2, 3, 4, 5};
  CHECK(put("a/b.txt", one, 5));
  CHECK(get("a/b.txt", &d, &n) == kPresent && n == 5 && d && !memcmp(d, one, 5));
  CHECK(entries() == 1 && bytes() == 5);
  {
    uint8_t tmp[3] = {9, 9, 9};
    CHECK(put("c", tmp, 3));
    tmp[0] = 0;
    CHECK(get("c", &d, &n) == kPresent && d[0] == 9);
  }

  const uint8_t two[2] = {7, 8};
  CHECK(put("a/b.txt", two, 2));   // replaces
  CHECK(get("a/b.txt", &d, &n) == kPresent && n == 2 && !memcmp(d, two, 2));
  CHECK(entries() == 2 && bytes() == 5);

  mark("a/b.txt", kAbsent);   // frees the bytes
  d = one;
  n = 77;
  CHECK(get("a/b.txt", &d, &n) == kAbsent && n == 0);
  CHECK(bytes() == 3);
  mark("c", kLocked);
  CHECK(get("c", &d, &n) == kLocked && n == 0 && bytes() == 0);
  mark("never", kAbsent);   // a path not yet held becomes an entry
  CHECK(get("never", &d, &n) == kAbsent);
  CHECK(get("other", &d, &n) == kUnknown);

  clear();
  CHECK(entries() == 0 && bytes() == 0);
  CHECK(get("a/b.txt", &d, &n) == kUnknown);

  // 300 puts of 1..4096 bytes, each with its own pattern, then everything freed
  static uint8_t buf[4096];
  uint32_t want = 0;
  for (int i = 0; i < 300; ++i) {
    uint32_t len = 1 + (uint32_t)(i * 37) % 4096;
    memset(buf, i & 0xff, len);
    char p[40];
    sniprintf(p, sizeof p, "dir/f%03d.bin", i);
    CHECK(put(p, buf, len));
    want += len;
  }
  CHECK(entries() == 300 && bytes() == want);
  for (int i = 0; i < 300; ++i) {
    uint32_t len = 1 + (uint32_t)(i * 37) % 4096;
    char p[40];
    sniprintf(p, sizeof p, "dir/f%03d.bin", i);
    CHECK(get(p, &d, &n) == kPresent && n == len && d[0] == (i & 0xff) && d[len - 1] == (i & 0xff));
  }
  clear();
  CHECK(entries() == 0 && bytes() == 0);

  // the cap: entry kMaxEntries + 1 is refused and nothing is marked
  for (int i = 0; i < kMaxEntries; ++i) {
    char p[40];
    sniprintf(p, sizeof p, "cap/%d", i);
    CHECK(put(p, one, 1));
  }
  CHECK(!put("cap/over", one, 1));
  CHECK(get("cap/over", &d, &n) == kUnknown && entries() == kMaxEntries);
  CHECK(put("cap/0", two, 2));   // an existing path still replaces at the cap
  clear();

  char tmpl[] = "/tmp/store-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  card_file("/.rytm/lfo/P/k002.txt", "x");
  card_file("/.rytm/lfo/P/k003.txt", "locked");
  card_file("/.rytm/lfo/P/k005.txt", "hello");
  card_file("/.rytm/lfo/Q/k001.txt", "abc");
  core::host_set_card(g_card.c_str());
  fill_reads_present_absent_locked();
  fill_charges_the_control_core_nothing();
  refill_abandons_the_old_one();
  fill_lists_each_folder_once();
  fill_unlisted_folder_asks_per_path();
  fill_lists_around_a_pending_write();
  fill_waiting_says_why();
  fill_listing_errors_ask_per_path();
  system(("rm -rf '" + g_card + "'").c_str());

  printf("test_store: ok\n");
  return 0;
}

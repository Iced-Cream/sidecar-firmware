// core/tests/test_fmt.cpp - core/fmt's card writes: the atomic write (read back, power cut after every change: the file
// old or new, never half; in pieces; through the card queue), the LineReader and the post queue.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "fmt/fmt.h"
#include "msamp/cardq.h"
#include "platform.h"


using namespace core;

static std::string g_card;
static std::string golden_dir() {
  std::string f = __FILE__;
  return f.substr(0, f.rfind('/')) + "/golden";
}
static std::vector<uint8_t> slurp(const std::string& p) {
  std::vector<uint8_t> v;
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return v;
  uint8_t b[4096];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) v.insert(v.end(), b, b + n);
  fclose(f);
  return v;
}
static std::string rd(const std::string& p) {
  std::vector<uint8_t> v = slurp(g_card + p);
  return std::string(v.begin(), v.end());
}
static void put(const std::string& p, const std::string& s) {
  CHECK(system(("mkdir -p \"$(dirname '" + g_card + p + "')\"").c_str()) == 0);
  FILE* f = fopen((g_card + p).c_str(), "wb");
  CHECK(f);
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}
static bool exists(const std::string& p) {
  struct stat st;
  return stat((g_card + p).c_str(), &st) == 0;
}
static std::vector<std::string> list(const std::string& dir) {
  std::vector<std::string> v;
  DIR* d = opendir(dir.c_str());
  if (!d) return v;
  while (dirent* e = readdir(d))
    if (e->d_name[0] != '.') v.push_back(e->d_name);
  closedir(d);
  return v;
}

int main() {
  char tmpl[] = "/tmp/fmt-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  host_set_card(g_card.c_str());

  // ---- write_atomic: read back, the three renames; a power cut after every change leaves the file old or new
  {
    CHECK(fmt::write_atomic("/.rytm/demo/a.txt", "one\n", 4) && rd("/.rytm/demo/a.txt") == "one\n");
    CHECK(!exists("/.rytm/demo/a.txt.tmp") && !exists("/.rytm/demo/a.txt.prev"));
    CHECK(fmt::write_atomic("/.rytm/demo/a.txt", "two two\n", 8) && rd("/.rytm/demo/a.txt") == "two two\n");
    int cuts = 0;
    for (int k = 0; k < 40; ++k) {
      put("/.rytm/demo/b.txt", "old\n");
      host_power_cut(k);
      bool ok = fmt::write_atomic("/.rytm/demo/b.txt", "new contents\n", 13);
      host_power_cut(-1);   // the reboot
      fmt::recover("/.rytm/demo/b.txt");
      std::string s = rd("/.rytm/demo/b.txt");
      CHECK(s == "old\n" || s == "new contents\n");
      CHECK(!ok || s == "new contents\n");
      CHECK(!exists("/.rytm/demo/b.txt.tmp") && !exists("/.rytm/demo/b.txt.prev"));
      if (!ok) ++cuts;
      if (ok) break;
    }
    CHECK(cuts > 3);
    // in pieces: each step at most its piece of card I/O
    std::string big(300000, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = (char)('a' + i % 23);
    fmt::AtomicWriter w;
    w.begin("/.rytm/demo/big.bin", (const uint8_t*)big.data(), (uint32_t)big.size(), 65536);
    int steps = 0;
    while (w.step()) {
      CHECK(w.bytes_io() <= 65536);
      ++steps;
    }
    CHECK(w.ok() && rd("/.rytm/demo/big.bin") == big && steps >= 10);
    w.begin("/.rytm/demo/big.bin", (const uint8_t*)"short", 5);
    CHECK(w.step() && w.step());
    w.cancel();   // stopped half way: the old file stays, no .tmp
    CHECK(rd("/.rytm/demo/big.bin") == big && !exists("/.rytm/demo/big.bin.tmp"));
    // through the card queue from the control core (no card core in this test: at once)
    cardq::Req rq;
    memset(&rq, 0, sizeof rq);
    File f;
    static uint8_t q[] = "queued\n";
    CHECK(cardq::async(&rq, cardq::kAtomicReq, &f, "/.rytm/demo/q.txt", 0, q, 7) && rq.got == 7);
    CHECK(rd("/.rytm/demo/q.txt") == "queued\n");
  }


  // ---- LineReader (2026-10-26): the per-kit LFO / insert files' lines through a read-ahead buffer must be exactly the
  // byte-at-a-time reader's (its rules: \r dropped, a line cut at max - 1, a last line without \n still a line, -1 only
  // at the end), across the buffer's refills
  {
    auto bytewise = [](File& f, char* out, int max) {   // the old line(): one File::read() a byte
      int n = 0, c;
      bool any = false;
      while ((c = f.read()) >= 0) {
        any = true;
        if (c == '\n') break;
        if (c != '\r' && n < max - 1) out[n++] = (char)c;
      }
      out[n] = 0;
      return any ? n : -1;
    };
    std::string longline(700, 'x');
    std::string mid(250, 'm');   // ends near the 256-byte refill, so the next line's \r\n straddles it
    const std::string cases[] = {
        "", "\n", "a", "a\n", "a\r\nb\r\n", "\n\n\nlast", "# head\r\n1 VOICE 1 2 3\r\n\r\n2 FX 4 5 6",
        mid + "\r\nnext\r\n" + longline + "\nshort\n" + longline,
        std::string("\0led\nok\n", 8),   // a NUL-led line: its count, not 0 (0.3.0: never taken for a blank line)
    };
    int ncase = 0, nlines = 0;
    for (const std::string& c : cases)
      for (int max : {2, 8, 96, 1024}) {
        std::string fn = g_card + "/lines.txt";
        FILE* w = fopen(fn.c_str(), "wb");
        CHECK(w);
        fwrite(c.data(), 1, c.size(), w);
        fclose(w);
        File a, b;
        CHECK(a.open("/lines.txt", kRead) && b.open("/lines.txt", kRead));
        fmt::LineReader rd(b);
        char x[1024], y[1024], z[1024];
        uint32_t at = 0;   // fmt::next_line (2026-10-26, the RAM store): the same lines from the file's bytes in RAM
        for (;;) {
          int ka = bytewise(a, x, max), kb = rd.line(y, max);
          int kc = fmt::next_line((const uint8_t*)c.data(), (uint32_t)c.size(), &at, z, max);
          CHECK(ka == kb && !strcmp(x, y));
          CHECK(ka == kc && (ka < 0 || !memcmp(x, z, (size_t)ka + 1)));
          if (ka < 0) break;
          ++nlines;
        }
        CHECK(at == c.size());
        a.close(), b.close();
        ++ncase;
      }
    printf("test_fmt: LineReader = next_line = the byte reader on %d files (%d lines)\n", ncase, nlines);
  }
  // ---- post_queue (2026-10-26, the RAM store): a kit change's save with the post slot busy is queued (a copy), never
  // waited for; a newer text of a queued file replaces it; post_begin is null while one waits (so no newer save of that
  // file lands before it); post_step posts the next when the slot frees; post_wait(path) writes up to that path's
  {
    auto card_turn = [] {
      host_card_core(0);
      while (cardq::service()) {
      }
      host_card_core(3);
    };
    cardq::reset();
    cardq::set_worker(true);
    fmt::reset_for_tests();
    char* t = fmt::post_begin();
    CHECK(t);
    memcpy(t, "A1\n", 3);
    CHECK(fmt::post_end("/pq/a.txt", 3) && fmt::post_busy());
    CHECK(fmt::post_queue("/pq/b.txt", "B1\n", 3) && fmt::post_queue("/pq/c.txt", "C1\n", 3));
    CHECK(fmt::post_queue("/pq/b.txt", "B2\n", 3));   // newer: in its place
    CHECK(!fmt::post_begin() && fmt::post_busy());
    char big[8] = "x";
    CHECK(!fmt::post_queue("pq/rel.txt", big, 1) && !fmt::post_queue("/pq/big.txt", big, fmt::kQueueMax + 1));
    CHECK(!exists("/pq/a.txt") && !exists("/pq/b.txt"));   // nothing on the card before the card core's turn
    for (int i = 0; i < 16 && fmt::post_busy(); ++i) card_turn(), fmt::post_step();
    CHECK(!fmt::post_busy() && rd("/pq/a.txt") == "A1\n" && rd("/pq/b.txt") == "B2\n" && rd("/pq/c.txt") == "C1\n");
    CHECK(fmt::post_begin());
    // post_wait(path): the queue in its order up to that file (the rest left queued); no path: all
    t = fmt::post_begin();
    memcpy(t, "A2\n", 3);
    CHECK(fmt::post_end("/pq/a.txt", 3));
    CHECK(fmt::post_queue("/pq/b.txt", "B3\n", 3) && fmt::post_queue("/pq/c.txt", "C3\n", 3));
    fmt::post_wait("/pq/b.txt");
    CHECK(rd("/pq/a.txt") == "A2\n" && rd("/pq/b.txt") == "B3\n" && rd("/pq/c.txt") == "C1\n" && fmt::post_busy());
    fmt::post_wait("/pq/other.txt");   // not pending: no wait
    CHECK(rd("/pq/c.txt") == "C1\n");
    fmt::post_wait();
    CHECK(rd("/pq/c.txt") == "C3\n" && !fmt::post_busy());
    // no wait, no cap: 300 files queued behind one held-up write - every post_queue returns at once, the control core
    // charged no card time (a wait would write on it), nothing on the card before the card core's turns, none lost
    host_card_model(true);
    uint64_t ctl = host_card_us(3);
    t = fmt::post_begin();
    memcpy(t, "A4\n", 3);
    CHECK(fmt::post_end("/pq/a.txt", 3));
    std::vector<std::string> many;
    for (int i = 0; i < 300; ++i) many.push_back("/pq/m/q" + std::to_string(i) + ".txt");
    for (const std::string& p : many) CHECK(fmt::post_queue(p.c_str(), p.c_str(), (uint32_t)p.size()));
    for (const std::string& p : many) CHECK(fmt::post_queue(p.c_str(), p.c_str(), (uint32_t)p.size()));   // the same again: still 300
    CHECK(host_card_us(3) == ctl && !exists("/pq/m/q0.txt"));
    for (int i = 0; i < 700 && fmt::post_busy(); ++i) card_turn(), fmt::post_step();
    CHECK(!fmt::post_busy() && host_card_us(3) == ctl);
    for (const std::string& p : many) CHECK(rd(p) == p);
    host_card_model(false);
    // undo-replay review 2: a result per write (an id from post_queue), not one per path or "the last": two writes that
    // fail in one post_wait each keep their own failure, a good one its success, a replaced one is unknown
    {
      put("/pq/blocker", "a file where a folder should be\n");
      uint32_t ia = 0, ib = 0, ic = 0, id = 0, ix = 0;
      CHECK(fmt::post_queue("/pq/blocker/a.txt", "A\n", 2, &ia) && ia);
      CHECK(fmt::post_queue("/pq/blocker/b.txt", "B\n", 2, &ib) && ib && ib != ia);
      CHECK(fmt::post_queue("/pq/ok/c.txt", "C\n", 2, &ic) && ic);
      CHECK(fmt::post_queue("/pq/ok/d.txt", "D1\n", 3, &ix) && fmt::post_queue("/pq/ok/d.txt", "D2\n", 3, &id) && id != ix);
      CHECK(fmt::post_result(ia) == fmt::kPostPending && fmt::post_result(ic) == fmt::kPostPending);
      fmt::post_wait();   // (all of them in one call)
      CHECK(fmt::post_result(ia) == fmt::kPostFailed && fmt::post_result(ib) == fmt::kPostFailed);   // A not hidden by B
      CHECK(fmt::post_result(ic) == fmt::kPostOk && fmt::post_result(id) == fmt::kPostOk && rd("/pq/ok/d.txt") == "D2\n");
      CHECK(fmt::post_result(ix) == fmt::kPostUnknown && fmt::post_result(0) == fmt::kPostUnknown);   // replaced; none
      // the same path again, now writable: its new id succeeds, the old one's failure stays its own
      CHECK(system(("rm -f '" + g_card + "/pq/blocker'").c_str()) == 0);
      uint32_t ia2 = 0;
      CHECK(fmt::post_queue("/pq/blocker/a.txt", "A\n", 2, &ia2));
      fmt::post_wait();
      CHECK(fmt::post_result(ia2) == fmt::kPostOk && fmt::post_result(ia) == fmt::kPostFailed);
      printf("test_fmt: post_result per write: two failures in one post_wait each kept, a success, a replaced write unknown\n");
      // post_remove (undo-replay re-review): through the same queue, in its turn; a file not there is ok too
      uint32_t ir = 0, ir2 = 0, iw = 0;
      CHECK(fmt::post_queue("/pq/rm/x.txt", "X\n", 2, &iw));
      CHECK(fmt::post_remove("/pq/rm/x.txt", &ir) && ir && fmt::post_remove("/pq/rm/none.txt", &ir2));
      CHECK(fmt::post_result(ir) == fmt::kPostPending);
      fmt::post_wait();
      CHECK(fmt::post_result(ir) == fmt::kPostOk && fmt::post_result(ir2) == fmt::kPostOk && !exists("/pq/rm/x.txt"));
      CHECK(fmt::post_result(iw) == fmt::kPostOk);   // (the write in flight first, then the remove behind it)
      put("/pq/rm/y.txt", "Y\n");
      CHECK(fmt::post_remove("/pq/rm/y.txt", &ir));
      for (int i = 0; i < 8 && fmt::post_busy(); ++i) card_turn(), fmt::post_step();
      CHECK(fmt::post_result(ir) == fmt::kPostOk && !exists("/pq/rm/y.txt"));
      // re-review 2 finding 2: a remove the card refuses (a folder with a file in it), finished by post_wait: failed
      put("/pq/rm/full/a.txt", "A\n");
      CHECK(fmt::post_remove("/pq/rm/full", &ir));
      fmt::post_wait();
      CHECK(fmt::post_result(ir) == fmt::kPostFailed && exists("/pq/rm/full/a.txt"));
      printf("test_fmt: post_remove in the queue's order, a missing file ok\n");
    }
    // a newer text of the file in flight: queued, it lands after the one in flight (never before it)
    t = fmt::post_begin();
    memcpy(t, "A5\n", 3);
    CHECK(fmt::post_end("/pq/a.txt", 3));
    CHECK(fmt::post_queue("/pq/a.txt", "A6\n", 3));
    card_turn();
    CHECK(rd("/pq/a.txt") == "A5\n");
    for (int i = 0; i < 8 && fmt::post_busy(); ++i) fmt::post_step(), card_turn();
    CHECK(rd("/pq/a.txt") == "A6\n" && !fmt::post_busy());
    // fix round 1: a newer text of a queued file goes to the tail - it lands after the writes queued before it (a
    // RELOAD's .prv before the .sqp that replaces it, when an older .sqp save was queued ahead of both)
    {
      t = fmt::post_begin();
      memcpy(t, "S\n", 2);
      CHECK(fmt::post_end("/pq/o/slot.txt", 2));
      CHECK(fmt::post_queue("/pq/o/p000.sqp", "old\n", 4) && fmt::post_queue("/pq/o/p000.prv", "prv\n", 4));
      CHECK(fmt::post_queue("/pq/o/p000.sqp", "new\n", 4));   // newer: after the .prv now
      int sqp_at = -1, prv_at = -1;
      for (int i = 0; i < 16 && fmt::post_busy(); ++i) {
        card_turn(), fmt::post_step();
        if (prv_at < 0 && exists("/pq/o/p000.prv")) prv_at = i;
        if (sqp_at < 0 && exists("/pq/o/p000.sqp")) sqp_at = i;
      }
      card_turn(), fmt::post_step();
      if (sqp_at < 0 && exists("/pq/o/p000.sqp")) sqp_at = 99;
      CHECK(prv_at >= 0 && sqp_at > prv_at && rd("/pq/o/p000.sqp") == "new\n" && !fmt::post_busy());
      printf("test_fmt: a newer text of a queued file lands after the writes queued before it\n");
    }
    // a text over kPostMax (2026-10-26, the RAM store's phase 2: a sequencer pattern) is queued as its own copy, posted
    // at once when the slot is free (the slot writes from that copy), else in its turn behind the others
    {
      std::string b1(fmt::kPostMax + 1000, 'p'), b2(fmt::kPostMax * 3, 'q');
      b1[0] = '1', b2[0] = '2';
      CHECK(fmt::post_queue("/pq/big1.bin", b1.data(), (uint32_t)b1.size()) && fmt::pending("/pq/big1.bin"));
      CHECK(!exists("/pq/big1.bin"));   // posted: the card core's
      CHECK(fmt::post_queue("/pq/big2.bin", b2.data(), (uint32_t)b2.size()) && fmt::post_queue("/pq/a.txt", "A7\n", 3));
      b1[1] = b2[1] = 'x';   // the caller's buffers changed after: the copies are written
      for (int i = 0; i < 16 && fmt::post_busy(); ++i) card_turn(), fmt::post_step();
      CHECK(!fmt::post_busy() && rd("/pq/a.txt") == "A7\n");
      std::string r1 = rd("/pq/big1.bin"), r2 = rd("/pq/big2.bin");
      CHECK(r1.size() == b1.size() && r1[0] == '1' && r1[1] == 'p' && r2.size() == b2.size() && r2[0] == '2' && r2[1] == 'q');
      printf("test_fmt: post_queue over kPostMax: %u and %u bytes queued and written from their own copies\n",
             (unsigned)b1.size(), (unsigned)b2.size());
    }
    // the order between passes: one write per slot turn - after the first card turn only a is on the card, then a and b,
    // c still queued
    t = fmt::post_begin();
    memcpy(t, "A7\n", 3);
    CHECK(fmt::post_end("/pq/a.txt", 3));
    CHECK(fmt::post_queue("/pq/b.txt", "B7\n", 3) && fmt::post_queue("/pq/c.txt", "C7\n", 3));
    card_turn();
    CHECK(rd("/pq/a.txt") == "A7\n" && rd("/pq/b.txt") == "B3\n" && rd("/pq/c.txt") == "C3\n");
    CHECK(!fmt::post_step());   // a done here
    CHECK(fmt::post_step());    // b into the slot, posted
    CHECK(!fmt::post_step());   // ... and it only waits now: no step, the streams' idle step goes on (post_step's true is a write just posted)
    card_turn();
    CHECK(rd("/pq/b.txt") == "B7\n" && rd("/pq/c.txt") == "C3\n" && fmt::post_busy());
    for (int i = 0; i < 8 && fmt::post_busy(); ++i) fmt::post_step(), card_turn();
    CHECK(rd("/pq/c.txt") == "C7\n");
    // no take-back (phase 3, 2026-10-26): a queued write in the slot the card core does not take within
    // cardq::kTakeBackUs stays posted (counted once in post_held), never run here; it lands at the next card turn
    t = fmt::post_begin();
    memcpy(t, "A8\n", 3);
    CHECK(fmt::post_end("/pq/a.txt", 3));
    CHECK(fmt::post_queue("/pq/b.txt", "B8\n", 3));
    card_turn();
    CHECK(!fmt::post_step() && fmt::post_step());   // a done; b posted, the card core not running
    uint64_t now = now_us();
    host_fake_time(true, now + cardq::kTakeBackUs + 1000);
    uint32_t held0 = fmt::post_held(), took0 = cardq::stats().taken_back;
    for (int i = 0; i < 4; ++i) CHECK(!fmt::post_step() && fmt::post_busy());   // still posted: only waiting, no step
    CHECK(rd("/pq/b.txt") == "B7\n" && fmt::post_held() == held0 + 1 && cardq::stats().taken_back == took0);
    card_turn();
    CHECK(!fmt::post_step() && !fmt::post_busy() && rd("/pq/b.txt") == "B8\n");   // written by the card core
    host_fake_time(false, 0);
    cardq::set_worker(false);
    cardq::reset();
    fmt::reset_for_tests();
    // unwritten / the oldest age (/status "store"): one edit counts once at every stage with cardq::pending()
    {
      auto total = [&](uint32_t* o) { return (uint32_t)cardq::pending() + fmt::unwritten(o); };
      uint32_t o = 99;
      CHECK(total(&o) == 0 && o == 0);
      cardq::set_worker(true);
      t = fmt::post_begin();
      memcpy(t, "U1\n", 3);
      CHECK(fmt::post_end("/pq/u.txt", 3));   // posted into the ring
      CHECK(cardq::pending() == 1 && total(&o) == 1);
      CHECK(fmt::post_queue("/pq/u2.txt", "U2\n", 3) && total(&o) == 2);   // queued behind it
      uint64_t t0 = now_us();
      host_fake_time(true, t0 + 5000000);
      CHECK(total(&o) == 2 && o >= 4900 && o <= 5100);
      CHECK(fmt::post_queue("/pq/u2.txt", "U3\n", 3) && total(&o) == 2 && o >= 4900);   // replaced: keeps its age
      card_turn();   // the card core takes the slot's write
      CHECK(total(&o) == 2);
      while (fmt::post_busy()) fmt::post_step(), card_turn();
      CHECK(total(&o) == 0 && o == 0);
      // held: posted, the card core not running
      CHECK(fmt::post_queue("/pq/u4.txt", "U4\n", 3) && !fmt::post_step() && fmt::post_busy());
      host_fake_time(true, t0 + 6000000);
      CHECK(total(&o) == 1);
      host_fake_time(true, t0 + 6000000 + cardq::kTakeBackUs + 1000);
      CHECK(!fmt::post_step() && fmt::post_stalled() && total(&o) == 1);   // held: still once
      card_turn();
      while (fmt::post_busy()) fmt::post_step(), card_turn();
      CHECK(total(&o) == 0 && o == 0);
      host_fake_time(false, 0);
    }
    printf("test_fmt: post_queue: queued behind the slot, never a wait, newest text wins, in order, none lost\n");
  }
  CHECK(system(("rm -rf '" + g_card + "'").c_str()) == 0);   // (the temp card)
  printf("test_fmt: ok\n");
  return 0;
}

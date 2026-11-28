// core/tests/test_fmt.cpp - core/fmt: the format registry (S8m: every entry sniffed, read, refused when newer or damaged;
// the old-layout move as boxlayout's upgrade; an upgrade chain in memory; the header conventions; a quick fuzz) and the
// card writes: the atomic write (read back, power cut after every change: the file
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
#include "kv/kv.h"
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

static fmt::Status ld(const char* name, const std::string& s, fmt::Info* i, std::string* out = nullptr) {
  const fmt::Format* f = fmt::find(name);
  CHECK(f);
  static uint8_t buf[2 << 20];
  fmt::Buf b = {buf, 0, sizeof buf};
  fmt::Bytes in = {(const uint8_t*)s.data(), (uint32_t)s.size()};
  fmt::Status st = fmt::load(*f, in, &b, i);
  if (out) out->assign((const char*)buf, b.n);
  return st;
}

// ---- an upgrade chain (a test format: v1 "a=1" lines -> v2 with a header -> v3 upper-cased keys), pure
static bool up12(const fmt::Bytes& in, fmt::Buf* out, fmt::Ctx*) {
  int n = fmt::put_text_header((char*)out->p, (int)out->cap, "demo", 2, 2);
  if (n + in.n > out->cap) return false;
  memcpy(out->p + n, in.p, in.n);
  out->n = n + in.n;
  return true;
}
static bool up23(const fmt::Bytes& in, fmt::Buf* out, fmt::Ctx*) {
  fmt::Sniffed s;
  if (!fmt::text_header(in, "demo", &s) || s.version != 2) return false;
  int n = fmt::put_text_header((char*)out->p, (int)out->cap, "demo", 3, 2);
  for (uint32_t i = s.body; i < in.n; ++i) out->p[n++] = (uint8_t)(in.p[i] >= 'a' && in.p[i] <= 'z' ? in.p[i] - 32 : in.p[i]);
  out->n = (uint32_t)n;
  return true;
}
static bool demo_sniff(const fmt::Bytes& in, fmt::Sniffed* o) {
  if (fmt::text_header(in, "demo", o)) return true;
  o->version = o->min_reader = 1, o->body = 0;
  return true;
}
static bool demo_read(const fmt::Bytes& in, const fmt::Sniffed& s, char* why, int cap) {
  return s.version == 3 || (snprintf(why, cap, "not v3"), false);
}
static const fmt::Upgrade kDemoChain[] = {{1, up12}, {2, up23}};

int main() {
  char tmpl[] = "/tmp/fmt-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  host_set_card(g_card.c_str());

  // ---- the registry (S8m, 2026-11-28): the generic line's entries, at today's versions
  fmt::Info info;
  std::string out;
  {
    struct Want { const char* name; int cls; int current; int oldest; };
    const Want inv[] = {{"store", fmt::kProject, 1, 1}, {"boxlayout", fmt::kProject, 2, 1}, {"boxname", fmt::kSetting, 1, 1},
                        {"boxlast", fmt::kSetting, 1, 1}};
    CHECK(fmt::count() == (int)(sizeof inv / sizeof inv[0]));
    for (const Want& w : inv) {
      const fmt::Format* f = fmt::find(w.name);
      CHECK(f && f->cls == w.cls && f->current == w.current && f->oldest == w.oldest && f->sniff && f->read && f->where[0]);
      CHECK(f->nchain == f->native - f->oldest);   // a chain step for every version below the native one
    }
    CHECK(!fmt::find("pool") && !fmt::find(nullptr) && !fmt::at(-1) && !fmt::at(fmt::count()));
  }
  uint64_t ch0 = host_changes();

  // STORE records: a mod's bytes as they are (v1, no header); one past kv::kMaxRecord refused
  {
    std::string rec("\x00\x01\xff#sidecar store 9\n", 20);
    CHECK(ld("store", rec, &info, &out) == fmt::kOk && info.version == 1 && out == rec);
    CHECK(ld("store", "", &info, &out) == fmt::kOk && out.empty());
    CHECK(ld("store", std::string(kv::kMaxRecord, 'x'), &info) == fmt::kOk);
    CHECK(ld("store", std::string(kv::kMaxRecord + 1, 'x'), &info) == fmt::kUnreadable && info.locked);
  }

  // the per-box folder layout: v2 (no old entries) as it is; an empty or absent type folder is v2 too
  {
    CHECK(ld("boxlayout", "", &info, &out) == fmt::kOk && info.version == 2 && out.empty());
    const char* v2 = "3fa2c001/\nlast\n3fa2c001/store/\n3fa2c001/name.txt\n";
    CHECK(ld("boxlayout", v2, &info, &out) == fmt::kOk && info.version == 2 && out == v2);
    // what is old: not a UID folder (8 hex, a folder), not last; a file named like a UID, a 7-hex folder are old
    CHECK(!fmt::old_layout_entry("3FA2C001", 8, true) && !fmt::old_layout_entry("last", 4, false));
    CHECK(fmt::old_layout_entry("3fa2c001", 8, false) && fmt::old_layout_entry("3fa2c00", 7, true));
    CHECK(fmt::old_layout_entry("store", 5, true) && fmt::old_layout_entry("keys.txt", 8, false));
    CHECK(!fmt::old_layout_entry(".", 1, true) && !fmt::old_layout_entry("..", 2, true));
    // not a listing: a NUL, a control character, a '/' that is not "<uid>/", an empty name
    CHECK(ld("boxlayout", std::string("a\0b\n", 4), &info) == fmt::kUnreadable);
    CHECK(ld("boxlayout", "a\tb\n", &info) == fmt::kUnreadable);
    CHECK(ld("boxlayout", "store/x/\n", &info) == fmt::kUnreadable);
    CHECK(ld("boxlayout", "3fa2c001//\n", &info) == fmt::kUnreadable);
    CHECK(ld("boxlayout", "/\n", &info) == fmt::kUnreadable);
    CHECK(ld("boxlayout", "3fa2c001/a/b\n", &info) == fmt::kUnreadable);
  }
  // the old-layout move (v1 -> v2): a card from before the UID folders - STORE's folder, a box's files - upgraded in
  // memory for UID 3fa2c001: every old entry under <uid>/, last and another box's UID folder stay; CRLF read
  {
    const char* v1 = "store/\r\nkeys.txt\nlast\n0000beef/\n3FA2C001\n";
    fmt::Ctx c = {"3FA2C001", nullptr};
    static uint8_t lb[4096];
    fmt::Buf b = {lb, 0, sizeof lb};
    fmt::Bytes in = {(const uint8_t*)v1, (uint32_t)strlen(v1)};
    CHECK(fmt::load(*fmt::find("boxlayout"), in, &b, &info, &c) == fmt::kUpgraded && info.version == 1 && !info.locked);
    out.assign((const char*)lb, b.n);
    CHECK(out == "last\n0000beef/\n3fa2c001/\n3fa2c001/store/\n3fa2c001/keys.txt\n3fa2c001/3FA2C001\n");
    // the result is v2 and loads as it is (idempotent); the UID folder already there is listed once
    CHECK(ld("boxlayout", out, &info) == fmt::kOk && info.version == 2);
    const char* v1b = "3fa2c001/\nstore/\n";
    in = {(const uint8_t*)v1b, (uint32_t)strlen(v1b)};
    b = {lb, 0, sizeof lb};
    CHECK(fmt::load(*fmt::find("boxlayout"), in, &b, &info, &c) == fmt::kUpgraded);
    CHECK(std::string((const char*)lb, b.n) == "3fa2c001/\n3fa2c001/store/\n");
    // no UID (or a bad one): the step refuses, nothing upgraded (the layout stays locked until the box's UID is known)
    b = {lb, 0, sizeof lb};
    CHECK(fmt::load(*fmt::find("boxlayout"), in, &b, &info) == fmt::kUnreadable && info.locked);
    fmt::Ctx bad = {"3fa2c0g1", nullptr};
    CHECK(fmt::load(*fmt::find("boxlayout"), in, &b, &info, &bad) == fmt::kUnreadable);
    fmt::Ctx longer = {"3fa2c0011", nullptr};
    CHECK(fmt::load(*fmt::find("boxlayout"), in, &b, &info, &longer) == fmt::kUnreadable);
    // no buffer: an upgrade needs one
    CHECK(fmt::load(*fmt::find("boxlayout"), in, nullptr, &info, &c) == fmt::kUnreadable);
    // no room: refused, never cut
    fmt::Buf tiny = {lb, 0, 12};
    CHECK(fmt::load(*fmt::find("boxlayout"), in, &tiny, &info, &c) == fmt::kUnreadable);
    // a newer layout: refused, locked
    CHECK(ld("boxlayout", "#sidecar boxlayout 3\n", &info) == fmt::kUnreadable);   // (a listing has no header: not one)
  }
  // a box folder's name.txt and a type folder's last
  {
    CHECK(ld("boxname", "Rytm 3fa2", &info) == fmt::kOk && info.version == 1);
    CHECK(ld("boxname", "Studio Rytm #2\r\n", &info) == fmt::kOk);
    CHECK(ld("boxname", "", &info) == fmt::kUnreadable && info.locked);
    CHECK(ld("boxname", "a\nb\n", &info) == fmt::kUnreadable);
    CHECK(ld("boxname", std::string(24, 'x'), &info) == fmt::kUnreadable);
    CHECK(ld("boxname", "a\tb", &info) == fmt::kUnreadable);
    CHECK(ld("boxname", "#sidecar boxname 2\nRytm\n", &info) == fmt::kNewer && info.locked && !info.read_only);
    CHECK(ld("boxname", "#sidecar boxname 2 min=1\nRytm\n", &info) == fmt::kForeignNewer && info.read_only);
    CHECK(ld("boxname", "#sidecar boxlast 1\nRytm\n", &info) == fmt::kUnreadable);   // another format's header
    CHECK(ld("boxlast", "3fa2c001", &info) == fmt::kOk && ld("boxlast", "3FA2C001\n", &info) == fmt::kOk);
    CHECK(ld("boxlast", "3fa2c00", &info) == fmt::kUnreadable && ld("boxlast", "3fa2c001\n0000beef\n", &info) == fmt::kUnreadable);
    char line[200];
    CHECK(ld("boxlast", "#sidecar boxlast 4\n3fa2c001\n", &info) == fmt::kNewer);
    CHECK(fmt::refusal(*fmt::find("boxlast"), info, "/.rytm/last", line, sizeof line) > 0 && strstr(line, "boxlast v4"));
    CHECK(ld("boxlast", "zz", &info) == fmt::kUnreadable);
    CHECK(fmt::refusal(*fmt::find("boxlast"), info, "/.rytm/last", line, sizeof line) > 0 && strstr(line, "not a boxlast file"));
    CHECK(ld("boxlast", "3fa2c001", &info) == fmt::kOk && fmt::refusal(*fmt::find("boxlast"), info, "x", line, sizeof line) == 0);
  }
  CHECK(host_changes() == ch0);   // loading and refusing never write the card
  printf("test_fmt: registry: store, boxlayout (the old-layout move), boxname, boxlast\n");

  // ---- an upgrade chain in memory: v1 -> v2 -> v3, the reader sees only v3; out has the upgraded bytes
  {
    fmt::Format demo = {"demo", fmt::kProject, fmt::kText, 3, 1, 3, demo_sniff, demo_read, kDemoChain, 2, nullptr, "/demo", 0};
    static uint8_t buf[4096];
    fmt::Buf b = {buf, 0, sizeof buf};
    const char* v1 = "a=1\nb=2\n";
    fmt::Bytes in = {(const uint8_t*)v1, (uint32_t)strlen(v1)};
    CHECK(fmt::load(demo, in, &b, &info) == fmt::kUpgraded && info.version == 1 && !info.locked);
    CHECK(std::string((char*)buf, b.n) == "#sidecar demo 3 min=2\nA=1\nB=2\n");
    fmt::Bytes in3 = {buf, b.n};
    static uint8_t buf2[4096];
    fmt::Buf b2 = {buf2, 0, sizeof buf2};
    CHECK(fmt::load(demo, in3, &b2, &info) == fmt::kOk && b2.n == in3.n);   // idempotent
    fmt::Upgrade only12[] = {{1, up12}};
    demo.chain = only12, demo.nchain = 1;
    CHECK(fmt::load(demo, in, &b, &info) == fmt::kUnreadable && strstr(info.why, "no upgrade"));
    fmt::Buf tiny = {buf, 0, 6};
    demo.chain = kDemoChain, demo.nchain = 2;
    CHECK(fmt::load(demo, in, &tiny, &info) == fmt::kUnreadable);   // no room: refused, never cut
  }

  // ---- header conventions
  {
    char h[64];
    CHECK(fmt::put_text_header(h, sizeof h, "pool", 2, 1) > 0 && !strcmp(h, "#sidecar pool 2 min=1\n"));
    fmt::Sniffed s;
    fmt::Bytes b = {(const uint8_t*)h, (uint32_t)strlen(h)};
    CHECK(fmt::text_header(b, "pool", &s) && s.version == 2 && s.min_reader == 1 && s.body == strlen(h));
    CHECK(!fmt::text_header(b, "poo", &s) && !fmt::text_header(b, "pools", &s));
    CHECK(fmt::put_kv_header(h, sizeof h, "keys", 7) > 0 && !strcmp(h, "format=keys/7\n"));
    b = {(const uint8_t*)h, (uint32_t)strlen(h)};
    CHECK(fmt::kv_header(b, "keys", &s) && s.version == 7);
    const char* j = "{ \"format\": \"sidecar-project\", \"version\": 1, \"min_reader\": 1, \"formats\": {\"pool\": 2} }";
    b = {(const uint8_t*)j, (uint32_t)strlen(j)};
    CHECK(fmt::json_header(b, "project", &s) && s.version == 1 && s.min_reader == 1);
    CHECK(!fmt::json_header(b, "song", &s));
    uint8_t bin[32];
    fmt::put_bin_header(bin, "SLIB", 2, 1, 16, 0xDEADBEEF);
    uint32_t crc;
    b = {bin, sizeof bin};
    CHECK(fmt::bin_header(b, "SLIB", &s, &crc) && s.version == 2 && s.min_reader == 1 && s.body == 16 && crc == 0xDEADBEEF);
    CHECK(!fmt::bin_header(b, "SQP1", &s, &crc));
  }

  // ---- a quick fuzz: every format's sniffer and reader on mutated seeds and random bytes: no crash, never a card write
  {
    uint64_t c0 = host_changes();
    uint32_t seed = 12345;
    auto rnd = [&]() { return seed = seed * 1103515245u + 12345u, seed >> 8; };
    const std::vector<std::string> seeds = {"store/\nkeys.txt\nlast\n3fa2c001/\n", "3fa2c001/\n3fa2c001/store/\n",
                                            "Rytm 3fa2\n", "#sidecar boxname 2 min=1\nRytm\n", "3fa2c001\n",
                                            std::string("\x00\x01\x02 record", 10), "#sidecar demo 3\nA=1\n"};
    int runs = 0;
    for (int it = 0; it < 4000; ++it) {
      std::string s = seeds[rnd() % seeds.size()];
      int m = (int)(rnd() % 6);
      if (m == 0 && !s.empty()) {
        s.resize(rnd() % s.size());   // cut short
      } else if (m == 1) {
        for (int k = 0; k < 4 && !s.empty(); ++k) s[rnd() % s.size()] = (char)rnd();   // bytes changed
      } else if (m == 2) {
        s.insert(s.empty() ? 0 : rnd() % s.size(), 1, '\0');   // a NUL
      } else if (m == 3) {
        for (char& ch : s)
          if (ch == '\n' && rnd() % 2) ch = '\r';   // CR / LF mixed
      } else if (m == 4) {
        s = "";
        for (int k = (int)(rnd() % 200); k > 0; --k) s += (char)rnd();   // junk
      } else if (m == 5) {
        s += "/" + s;   // slashes in odd places
      }
      for (int i = 0; i < fmt::count(); ++i) {
        fmt::Info fi;
        ld(fmt::at(i)->name, s, &fi);
        static uint8_t fb[8192];
        fmt::Buf b = {fb, 0, sizeof fb};
        fmt::Ctx c = {"0000beef", nullptr};
        fmt::load(*fmt::at(i), fmt::Bytes{(const uint8_t*)s.data(), (uint32_t)s.size()}, &b, &fi, &c);
        runs += 2;
      }
    }
    CHECK(host_changes() == c0);
    printf("test_fmt: fuzz: %d loads, no crash, no card write\n", runs);
  }


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

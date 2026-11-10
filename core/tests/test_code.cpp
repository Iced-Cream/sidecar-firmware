// core/tests/test_code.cpp - shipped code (core/code): the SCX checker's refusals, the code ops 33..36 through the
// host link (OFFER, CHUNK, COMMIT, DROP; WANT, LOADED, REFUSED), the card cache (a temp folder as the card: the file by
// hash, the index, the cap's eviction, the crash mark), resuming a transfer, the name clash with a built-in.
//
// The blob's code runs only on AArch64 (make -C core test-aarch64: this suite under qemu-aarch64 user mode). There,
// with SCX_TGAIN=<tgain.scx> (sidecar-core blobs/), it also loads TGAIN through the whole path: transfer, card,
// relocation, constructors, the page protection (mprotect: W^X), the DSP library (INFO, PLACE by name), a graph
// stem -> TGAIN -> OUT with params through the lock service, DROP while placed, the HELLO, the card's fast path and the
// crash mark. Without SCX_TGAIN on AArch64 the test fails (a gate step that cannot run is a failure).
#include <sys/mman.h>
#include <sys/stat.h>

#include <cmath>
#include <cstddef>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "code/code.h"
#include "code/loader.h"
#include "dsp/dsp.h"
#include "fmt/fmt.h"
#include "hooks.h"
#include "kv/kv.h"
#include "kv/link.h"
#include "lib/sha256.h"
#include "locks/locks.h"
#include "platform.h"
#include "proc/proc.h"
#include "route/route.h"

using namespace core;
using Bytes = std::vector<uint8_t>;

static std::vector<Bytes> g_out;
static void capture(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static bool send(Bytes body) {
  Bytes m = {0xF0, 0x7D, 0x52, 0x54, 0x51, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return kv::link::from_host(m.data(), (int)m.size());
}
static std::vector<Bytes> answers() {
  g_out.clear();
  for (int i = 0; i < 8; ++i) kv::link::host_step(64);   // (no card core: host_step runs the card ops)
  return g_out;
}
static void put32(Bytes& b, uint32_t v) {
  b.push_back((uint8_t)(v >> 28 & 0x0F)), b.push_back((uint8_t)(v >> 21 & 0x7F)), b.push_back((uint8_t)(v >> 14 & 0x7F));
  b.push_back((uint8_t)(v >> 7 & 0x7F)), b.push_back((uint8_t)(v & 0x7F));
}
static uint32_t get32(const uint8_t* b) {
  return (uint32_t)b[0] << 28 | (uint32_t)b[1] << 21 | (uint32_t)b[2] << 14 | (uint32_t)b[3] << 7 | b[4];
}
static void name8(Bytes& b, const char* name) {
  for (int i = 0; i < 8; ++i) b.push_back(i < (int)strlen(name) ? (uint8_t)name[i] : 0);
}
static std::string str8(const uint8_t* b) {
  std::string s;
  for (int i = 0; i < 8 && b[i]; ++i) s += (char)b[i];
  return s;
}

// ---- the wire ----
struct Ans { int op = -1, seq = -1, owner = -1, code = -1; uint32_t offset = 0; std::string name; };
static Ans one(const std::vector<Bytes>& a) {
  CHECK(a.size() == 1);
  const Bytes& m = a[0];
  Ans r;
  r.op = m[6], r.seq = m[7], r.owner = m[8];
  if (r.op == code::kWant) { CHECK(m.size() == 15); r.offset = get32(&m[9]); }
  else if (r.op == code::kLoaded) { CHECK(m.size() == 18); r.name = str8(&m[9]); }
  else if (r.op == code::kRefused) { CHECK(m.size() == 11); r.code = m[9]; }
  else CHECK(false);
  return r;
}
static Ans offer(int seq, int owner, const Bytes& blob, const char* name, int size = -1) {
  uint8_t h[32], p[40];
  lib::sha256(blob.data(), blob.size(), h);
  Bytes b = {code::kOffer, (uint8_t)seq, (uint8_t)owner};
  int pn = kv::pack7(h, 32, p);
  CHECK(pn == 37);
  b.insert(b.end(), p, p + pn);
  put32(b, size < 0 ? (uint32_t)blob.size() : (uint32_t)size);
  name8(b, name);
  send(b);
  return one(answers());
}
// CHUNKs from `from` up to `to` (112 raw bytes each); skip: a chunk offset left out (a gap)
static std::vector<Bytes> chunks(int seq, int owner, const Bytes& blob, uint32_t from, uint32_t to, int64_t skip = -1) {
  std::vector<Bytes> all;
  for (uint32_t off = from; off < to; off += kv::kChunk) {
    if ((int64_t)off == skip) continue;
    uint32_t n = to - off < (uint32_t)kv::kChunk ? to - off : (uint32_t)kv::kChunk;
    uint8_t p[140];
    int pn = kv::pack7(blob.data() + off, (int)n, p);
    Bytes b = {code::kChunk, (uint8_t)seq, (uint8_t)owner};
    put32(b, off);
    b.insert(b.end(), p, p + pn);
    send(b);
    auto a = answers();
    all.insert(all.end(), a.begin(), a.end());
  }
  return all;
}
static Ans commit(int seq, int owner) {
  send({code::kCommit, (uint8_t)seq, (uint8_t)owner});
  return one(answers());
}
// the whole transfer of a blob the Pi does not hold: OFFER -> WANT 0 -> CHUNKs -> COMMIT
static Ans ship(int seq, int owner, const Bytes& blob, const char* name) {
  Ans w = offer(seq, owner, blob, name);
  if (w.op != code::kWant) return w;
  CHECK(w.offset == 0 && w.seq == seq && w.owner == owner);
  CHECK(chunks(seq, owner, blob, 0, (uint32_t)blob.size()).empty());
  return commit(seq, owner);
}

// ---- synthetic blobs: a valid header and tables around an image of zeros (never run) ----
struct Synth {
  const char* name = "SYN";
  uint32_t image = 256, bss = 0, entry = 0;
  uint16_t abi = SCX_ABI;
  std::vector<std::pair<uint32_t, uint64_t>> relocs;   // offset, addend
  std::vector<std::pair<uint32_t, uint32_t>> imports;  // hash, offset
  uint32_t init_off = 0, init_count = 0;
  int extra = 0;                                       // trailing bytes
  uint32_t fill = 0;                                   // image bytes: (i * 7 + fill) & 0xFF, so hashes differ
  uint16_t flags = 0;
  uint32_t data_off = 0;
};
static Bytes synth(const Synth& s) {
  scx_header h{};
  h.magic = SCX_MAGIC;
  h.abi = s.abi;
  strncpy(h.name, s.name, sizeof h.name);
  h.image_size = s.image, h.bss_size = s.bss, h.entry = s.entry;
  h.flags = s.flags, h.data_off = s.data_off;
  h.init_off = s.init_off, h.init_count = s.init_count;
  h.reloc_off = code::kHeader + s.image, h.reloc_count = (uint32_t)s.relocs.size();
  h.import_off = h.reloc_off + 4 * h.reloc_count, h.import_count = (uint32_t)s.imports.size();
  Bytes b((uint8_t*)&h, (uint8_t*)&h + sizeof h);
  Bytes img(s.image);
  for (uint32_t i = 0; i < s.image; ++i) img[i] = (uint8_t)(i * 7 + s.fill);
  for (auto& r : s.relocs) if (r.first + 8 <= s.image) memcpy(&img[r.first], &r.second, 8);
  for (auto& im : s.imports) if (im.second + 8 <= s.image) memset(&img[im.second], 0, 8);
  b.insert(b.end(), img.begin(), img.end());
  for (auto& r : s.relocs) b.insert(b.end(), (uint8_t*)&r.first, (uint8_t*)&r.first + 4);
  for (auto& im : s.imports) {
    scx_import x{im.first, im.second};
    b.insert(b.end(), (uint8_t*)&x, (uint8_t*)&x + sizeof x);
  }
  b.resize(b.size() + (size_t)s.extra, 0);
  return b;
}
static int chk(const Bytes& b) { return code::check(b.data(), (uint32_t)b.size(), nullptr); }

static void test_check() {
  Synth ok;
  ok.relocs = {{8, 16}};
  ok.imports = {{scx_hash("memcpy"), 24}};
  CHECK(chk(synth(ok)) == code::kOk);
  Bytes b = synth(ok);
  b[0] ^= 1;
  CHECK(chk(b) == code::kBad);                                    // magic
  { Synth s = ok; s.abi = 2; CHECK(chk(synth(s)) == code::kAbi); }
  { Synth s = ok; s.name = "lower"; CHECK(chk(synth(s)) == code::kBad); }
  { Synth s = ok; s.name = ""; CHECK(chk(synth(s)) == code::kBad); }
  { Synth s = ok; s.extra = 1; CHECK(chk(synth(s)) == code::kBad); }   // the tables end the file
  { Synth s = ok; s.relocs = {{252, 0}}; CHECK(chk(synth(s)) == code::kBad); }   // a slot past the image
  { Synth s = ok; s.relocs = {{8, 256}}; CHECK(chk(synth(s)) == code::kBad); }   // pointing past image + bss
  { Synth s = ok; s.bss = 64; s.relocs = {{8, 300}}; CHECK(chk(synth(s)) == code::kOk); }   // into the bss
  { Synth s = ok; s.imports = {{scx_hash("fopen"), 24}}; CHECK(chk(synth(s)) == code::kImport); }
  // the writable part (SCX_F_DATA): from a 64 KB boundary inside the image, above the entry; none without the flag
  { Synth s = ok; s.image = 0x10100; s.flags = SCX_F_DATA; s.data_off = 0x10000; CHECK(chk(synth(s)) == code::kOk); }
  { Synth s = ok; s.image = 0x10100; s.flags = SCX_F_DATA; s.data_off = 0x8000; CHECK(chk(synth(s)) == code::kBad); }
  { Synth s = ok; s.image = 0x10100; s.flags = SCX_F_DATA; s.data_off = 0x20000; CHECK(chk(synth(s)) == code::kBad); }
  { Synth s = ok; s.image = 0x10100; s.flags = SCX_F_DATA; s.data_off = 0; CHECK(chk(synth(s)) == code::kBad); }
  { Synth s = ok; s.image = 0x10100; s.data_off = 0x10000; CHECK(chk(synth(s)) == code::kBad); }
  { Synth s = ok; s.image = 0x10100; s.flags = SCX_F_DATA; s.data_off = 0x10000; s.entry = 0x10010; CHECK(chk(synth(s)) == code::kBad); }
  { Synth s = ok; s.flags = 2; CHECK(chk(synth(s)) == code::kBad); }
  { Synth s = ok; s.entry = 254; CHECK(chk(synth(s)) == code::kBad); }
  { Synth s = ok; s.init_off = 32, s.init_count = 1; CHECK(chk(synth(s)) == code::kBad); }   // not relocated
  { Synth s = ok; s.init_off = 8, s.init_count = 1; CHECK(chk(synth(s)) == code::kOk); }
  { Synth s = ok; s.image = 0; CHECK(chk(synth(s)) == code::kBad); }
  { Synth s = ok; s.image = code::kMaxBlob; CHECK(chk(synth(s)) == code::kFull); }
  b = synth(ok);
  CHECK(code::check(b.data(), 63, nullptr) == code::kBad);
  // place: copy, bss zeroed, relocation = base + addend, import = the export's address
  alignas(64) static uint8_t mem[512];
  memset(mem, 0xEE, sizeof mem);
  { Synth s = ok; s.bss = 64; b = synth(s); }
  CHECK(code::place(b.data(), (uint32_t)b.size(), mem, 256) == code::kFull);
  CHECK(code::place(b.data(), (uint32_t)b.size(), mem, sizeof mem) == code::kOk);
  uint64_t v;
  memcpy(&v, mem + 8, 8);
  CHECK(v == (uint64_t)(uintptr_t)mem + 16);
  memcpy(&v, mem + 24, 8);
  CHECK(v == (uint64_t)(uintptr_t)code::find_export(scx_hash("memcpy")) && v);
  CHECK(mem[256] == 0 && mem[319] == 0 && mem[320] == 0xEE && mem[1] == 7);
  // the export table: every name hashes to a distinct entry
  for (int i = 0; i < code::export_count(); ++i)
    for (int k = 0; k < i; ++k) CHECK(scx_hash(code::export_name(i)) != scx_hash(code::export_name(k)));
}

// ---- the board on the host: mprotect for real W^X (where it can run blob code) ----
static bool host_protect(void* p, size_t n, code::Prot prot) {
  int pr = prot == code::kProtRW ? PROT_READ | PROT_WRITE : prot == code::kProtRWX ? PROT_READ | PROT_WRITE | PROT_EXEC
                                                                                  : PROT_READ | PROT_EXEC;
  if (mprotect(p, n, pr)) return false;
  if (prot != code::kProtRW) __builtin___clear_cache((char*)p, (char*)p + n);
  return true;
}
static const code::Board kBoard = {host_protect};

static std::string g_card;
static bool exists(const std::string& rel) {
  struct stat st;
  return stat((g_card + rel).c_str(), &st) == 0;
}
static std::string hexhash(const Bytes& b) {
  uint8_t h[32];
  char hx[65];
  lib::sha256(b.data(), b.size(), h);
  lib::hex(h, 32, hx);
  return hx;
}
static std::string slurp(const std::string& rel) {
  std::string s;
  FILE* f = fopen((g_card + rel).c_str(), "rb");
  if (!f) return s;
  char buf[4096];
  size_t k;
  while ((k = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, k);
  fclose(f);
  return s;
}

static void test_wire() {
  // malformed and refused before any code runs
  Synth a;
  a.name = "DELAY";   // a built-in's name: stored, then refused by the library
  a.fill = 1;
  Bytes ba = synth(a);
  Ans r = ship(1, 7, ba, "DELAY");
  CHECK(r.op == code::kRefused && r.code == code::kClash && r.seq == 1 && r.owner == 7);
  CHECK(exists("/.rytm/code/" + hexhash(ba) + ".scx"));
  CHECK(slurp("/.rytm/code/index.txt").find(hexhash(ba) + " " + std::to_string(ba.size()) + " DELAY ") != std::string::npos);
  CHECK(code::loaded() == 0);
  // offered again: the card has it (no transfer), still a clash
  r = offer(2, 7, ba, "DELAY");
  CHECK(r.op == code::kRefused && r.code == code::kClash);
  CHECK(code::stats().fast >= 1);

  // the name offered must be the blob's
  Synth s2;
  s2.name = "SYNB";
  s2.fill = 2;
  Bytes b2 = synth(s2);
  CHECK(offer(3, 7, b2, "OTHER").op == code::kWant);
  CHECK(chunks(3, 7, b2, 0, (uint32_t)b2.size()).empty());
  r = commit(3, 7);
  CHECK(r.op == code::kRefused && r.code == code::kBad);

  // a hash that does not match the bytes: 10
  CHECK(offer(4, 7, b2, "SYNB").op == code::kWant);
  Bytes wrong = b2;
  wrong[100] ^= 0x55;
  CHECK(chunks(4, 7, wrong, 0, (uint32_t)wrong.size()).empty());
  r = commit(4, 7);
  CHECK(r.op == code::kRefused && r.code == code::kHashBad);
  CHECK(!exists("/.rytm/code/" + hexhash(b2) + ".scx") && !exists("/.rytm/code/" + hexhash(wrong) + ".scx"));

  // an import the engine does not export: 11, after the transfer
  Synth s3;
  s3.name = "SYNC";
  s3.imports = {{scx_hash("fopen"), 40}};
  Bytes b3 = synth(s3);
  r = ship(5, 7, b3, "SYNC");
  CHECK(r.op == code::kRefused && r.code == code::kImport);

  // too big for a blob: 3, at the offer
  r = offer(6, 7, b3, "SYNC", (int)code::kMaxBlob + 1);
  CHECK(r.op == code::kRefused && r.code == code::kFull);

  // a gap asks from where it stopped; a repeat is ignored; COMMIT before the end asks again; a resume after a new
  // OFFER of the same blob in the same session; then the end
  Synth s4;
  s4.name = "DELAY";
  s4.image = 2000;
  s4.fill = 4;
  Bytes b4 = synth(s4);
  CHECK(offer(7, 7, b4, "DELAY").op == code::kWant);
  auto ga = chunks(7, 7, b4, 0, 560, 336);   // 0, 112, 224, (336 left out), 448
  CHECK(ga.size() == 1);
  Bytes w = ga[0];
  CHECK(w[6] == code::kWant && get32(&w[9]) == 336);
  CHECK(chunks(7, 7, b4, 112, 224).empty());   // a repeat
  r = commit(7, 7);
  CHECK(r.op == code::kWant && r.offset == 336);
  r = offer(8, 7, b4, "DELAY");
  CHECK(r.op == code::kWant && r.offset == 336 && r.seq == 8);
  CHECK(chunks(8, 7, b4, 336, (uint32_t)b4.size()).empty());
  r = commit(8, 7);
  CHECK(r.op == code::kRefused && r.code == code::kClash);   // landed (and on the card); the name is a built-in's
  // a HELLO drops a transfer under way: CHUNKs then go nowhere, COMMIT is refused
  Synth s5;
  s5.name = "SYNE";
  s5.fill = 5;
  Bytes b5 = synth(s5);
  CHECK(offer(9, 7, b5, "SYNE").op == code::kWant);
  send({0x00, 0, 0, 0, 0, 1});
  answers();
  CHECK(chunks(9, 7, b5, 0, (uint32_t)b5.size()).empty());
  r = commit(9, 7);
  CHECK(r.op == code::kRefused && r.code == code::kBad);
  // malformed ops are dropped and counted
  uint32_t bad = code::stats().ops_bad;
  send({code::kOffer, 1, 7, 0});
  CHECK(answers().size() == 1);   // OFFER too short: refused 5
  send({code::kDrop, 7, 'N', 'O', 'P', 'E', 0, 0, 0, 0});
  CHECK(answers().empty());
  CHECK(code::stats().ops_bad > bad);

  // the cache's cap: the oldest offered goes first, never one offered this boot
  code::card_reboot_for_test();       // a new boot: everything on the card is from before it
  code::set_cache_cap(ba.size() + b4.size() + 64);   // room for the two DELAYs now on the card
  Synth s6;
  s6.name = "FILTER";   // refused by the library after it is stored: never run
  s6.fill = 6;
  Bytes b6 = synth(s6);
  CHECK(offer(10, 7, b4, "DELAY").code == code::kClash);   // b4 offered this boot: kept
  r = ship(11, 7, b6, "FILTER");
  CHECK(r.op == code::kRefused && r.code == code::kClash);
  CHECK(!exists("/.rytm/code/" + hexhash(ba) + ".scx"));    // the oldest-offered (ba) evicted
  CHECK(exists("/.rytm/code/" + hexhash(b4) + ".scx") && exists("/.rytm/code/" + hexhash(b6) + ".scx"));
  code::set_cache_cap(0);   // back to 16 MB
}

#if defined(__aarch64__)
// ---- the real blob: TGAIN (sidecar-core blobs/tgain) ----
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
static int place(int seq, int owner, int slot, const char* name) {
  Bytes b = {0x30, (uint8_t)seq, (uint8_t)owner, (uint8_t)slot};
  for (const char* p = name; *p; ++p) b.push_back((uint8_t)*p);
  send(b);
  auto a = answers();
  CHECK(a.size() == 1);
  return a[0][6] == 0x70 ? a[0][9] : -a[0][9];
}
static int rt(int seq, int owner, int src, int dst, int gain) {
  send({0x20, (uint8_t)seq, (uint8_t)owner, (uint8_t)src, (uint8_t)dst, (uint8_t)(gain >> 7), (uint8_t)(gain & 0x7F)});
  auto a = answers();
  CHECK(a.size() == 1);
  return a[0][6] == 0x60 ? a[0][9] : -a[0][9];
}
static void base(int set, int slot, int first, std::vector<int> v) {
  CHECK(send({0x10, (uint8_t)set, 1, (uint8_t)slot, (uint8_t)first, (uint8_t)v.size()}));
  Bytes b = {0x11, (uint8_t)set, 0, (uint8_t)v.size()};
  for (int x : v) b.push_back((uint8_t)(x >> 7)), b.push_back((uint8_t)(x & 0x7F));
  CHECK(send(b));
}
static void settle() { for (int i = 0; i < 40; ++i) { block(128); answers(); } }

static void test_tgain(const Bytes& tg) {
  CHECK(tg.size() > code::kHeader);
  const int lib0 = dsp::library_size();
  // the first time: a full transfer, then LOADED
  Ans r = ship(20, 9, tg, "TGAIN");
  CHECK(r.op == code::kLoaded && r.name == "TGAIN" && r.seq == 20 && r.owner == 9);
  CHECK(code::loaded() == 1 && dsp::library_size() == lib0 + 1);
  CHECK(exists("/.rytm/code/" + hexhash(tg) + ".scx"));
  CHECK(exists("/.rytm/code/.armed"));   // its constructors ran less than a second ago
  // INFO lists it like a built-in, with its params
  send({0x32, 1, 0, (uint8_t)lib0, 0});
  auto a = answers();
  CHECK(a.size() == 1 && a[0][11] == 5 && !memcmp(&a[0][12], "TGAIN", 5) && a[0][17] == 2);
  send({0x32, 2, 1, (uint8_t)lib0, 0});
  a = answers();
  CHECK(a.size() == 1 && !memcmp(&a[0][12], "GAIN", 4) && (a[0][16] << 7 | a[0][17]) == 8192);
  // only its offerer may PLACE it
  CHECK(place(21, 8, 0x7F, "TGAIN") == -dsp::kUnknown);
  int slot = place(22, 9, 0x7F, "TGAIN");
  CHECK(slot >= 0 && proc::slot(slot) && !strcmp(proc::slot(slot)->name(), "TGAIN"));
  code::BlobView v;
  CHECK(code::blob(0, &v) && v.users == 1 && v.owner == 9);
  // the blob's pages: read + execute after loading (a write would fault): checked through /proc/self/maps? qemu
  // user mode keeps its own map; the protect calls themselves are checked by host_protect returning true.
  // a graph: stem IN 3 -> TGAIN -> OUT 2 / 3
  CHECK(rt(23, 9, 3, route::slot_chan(slot, 0), 8192) >= 0);
  CHECK(rt(24, 9, 3, route::slot_chan(slot, 1), 8192) >= 0);
  CHECK(rt(25, 9, route::slot_chan(slot, 0), 2, 8192) >= 0);
  CHECK(rt(26, 9, route::slot_chan(slot, 1), 3, 8192) >= 0);
  auto run_const = [&](float x) {
    for (int i = 0; i < 30; ++i) {
      for (int f = 0; f < 128; ++f) g_in[3][f] = x;
      block(128);
      answers();
    }
  };
  run_const(0.5f);
  CHECK(std::fabs(g_o[2][127] - 0.5f) < 1e-3f && std::fabs(g_o[3][127] - 0.5f) < 1e-3f);   // unity at the default
  // GAIN to 16383 (~x2) and INVERT on, through the lock service's BASE values
  base(9, slot, 0, {16383, 16383});
  run_const(0.25f);
  CHECK(std::fabs(g_o[2][127] - 0.25f * 127.0f / 64.0f) < 0.01f && std::fabs(g_o[3][127] + 0.25f * 127.0f / 64.0f) < 0.01f);
  base(9, slot, 0, {0, 0});
  run_const(0.25f);
  CHECK(std::fabs(g_o[2][127]) < 1e-6f);
  // a second after the first PLACE the crash mark is gone
  struct timespec ts = {1, 100000000};
  nanosleep(&ts, nullptr);
  answers();
  answers();
  CHECK(!exists("/.rytm/code/.armed"));
  // offered again in the same session: answered at once, no transfer
  uint32_t fast = code::stats().fast;
  r = offer(27, 9, tg, "TGAIN");
  CHECK(r.op == code::kLoaded && code::stats().fast == fast + 1);
  // another owner offering the same blob: a clash
  r = offer(28, 5, tg, "TGAIN");
  CHECK(r.op == code::kRefused && r.code == code::kClash);
  // DROP while a slot holds it: unloaded only once the slot is freed
  send({code::kDrop, 9, 'T', 'G', 'A', 'I', 'N', 0, 0, 0});
  answers();
  CHECK(code::loaded() == 1);
  CHECK(place(29, 9, 0x7F, "TGAIN") == -dsp::kUnknown);   // nobody may PLACE a dropped blob
  CHECK(send({0x31, 9, 0x7F}));   // FREE every slot of owner 9
  settle();
  CHECK(code::loaded() == 0 && dsp::library_size() == lib0 && code::stats().unloaded >= 1);
  // offered again: the card holds it, loaded without a transfer
  uint32_t from_card = code::stats().from_card, chunks0 = code::stats().chunks;
  r = offer(30, 9, tg, "TGAIN");
  CHECK(r.op == code::kLoaded && code::stats().from_card == from_card + 1 && code::stats().chunks == chunks0);
  CHECK(place(31, 9, 0x7F, "TGAIN") >= 0);
  // the companion's HELLO: the slots freed, the owner forgotten (blob still loaded); PLACE needs a new OFFER
  send({0x00, 0, 0, 0, 0, 1});
  settle();
  CHECK(code::loaded() == 1 && place(32, 9, 0x7F, "TGAIN") == -dsp::kUnknown);
  CHECK(offer(33, 9, tg, "TGAIN").op == code::kLoaded && place(34, 9, 0x7F, "TGAIN") >= 0);
  // the crash mark: a boot that finds .armed with its hash refuses it with 13 until a new version
  CHECK(send({0x31, 9, 0x7F}));
  send({code::kDrop, 9, 'T', 'G', 'A', 'I', 'N', 0, 0, 0});
  settle();
  CHECK(code::loaded() == 0);
  {
    std::string p = g_card + "/.rytm/code/.armed";
    FILE* f = fopen(p.c_str(), "wb");
    CHECK(f);
    std::string line = hexhash(tg) + "\n";
    fwrite(line.data(), 1, line.size(), f);
    fclose(f);
  }
  code::card_reboot_for_test();
  r = offer(35, 9, tg, "TGAIN");
  CHECK(r.op == code::kRefused && r.code == code::kCrashed);
  CHECK(!exists("/.rytm/code/.armed"));
  CHECK(slurp("/.rytm/code/index.txt").find(" TGAIN ") != std::string::npos);
  // a new version (other bytes, the same name) is taken
  Bytes tg2 = tg;
  tg2[offsetof(scx_header, reserved)] ^= 0x01;   // a header byte nothing reads: other bytes, another hash
  r = ship(36, 9, tg2, "TGAIN");
  CHECK(r.op == code::kLoaded);
  CHECK(place(37, 9, 0x7F, "TGAIN") >= 0);
  printf("test_code: TGAIN loaded, placed and run (%zu bytes)\n", tg.size());
}

// ---- a voice blob (SCX_VOICE=<plaits.scx>, sidecar-mods' PLAITS voice): trigs and knobs through TRIG / PARAM ----
static float peak_of(int ch, int from, int to) {
  float m = 0.0f;
  for (int f = from; f < to; ++f) m = std::fmax(m, std::fabs(g_o[ch][f]));
  return m;
}
static void test_voice_blob(const Bytes& vb) {
  Ans r = ship(40, 0x44, vb, "PLAITS");
  CHECK(r.op == code::kLoaded && r.name == "PLAITS");
  int slot = place(41, 0x44, 0x7F, "PLAITS");
  CHECK(slot >= 0);
  if (slot < 0) return;
  CHECK(rt(42, 0x44, route::slot_chan(slot, 0), 4, 8192) >= 0);   // onto BD's Overbridge input
  settle();
  memset(g_in, 0, sizeof g_in);
  block(128);
  CHECK(peak_of(4, 0, 128) < 1e-4f);                            // silent before a trig
  // knobs: LEV 100, ENG zone 5 (VA), HRM TIM MOR 64, DEC 80, LPG 64, TUN 64 (as the Rytm's words >> 2: value x 64)
  CHECK(send({0x38, 0x44, (uint8_t)slot, 0, 8, 100 * 64 >> 7, 0, 40 * 64 >> 7, 0, 64 * 64 >> 7, 0, 64 * 64 >> 7, 0,
              64 * 64 >> 7, 0, 80 * 64 >> 7, 0, 64 * 64 >> 7, 0, 64 * 64 >> 7, 0}));
  CHECK(send({0x37, 0x44, (uint8_t)slot, 0, 48, 110, 0, 0, 0, 1}));
  float early = 0.0f, late = 0.0f;
  bool finite = true;
  for (int i = 0; i < 375; ++i) {                                 // 1 s at 48 kHz
    block(128);
    for (int f = 0; f < 128; ++f) finite = finite && std::isfinite(g_o[4][f]);
    if (i < 20) early = std::fmax(early, peak_of(4, 0, 128));
    if (i >= 360) late = std::fmax(late, peak_of(4, 0, 128));
    CHECK(peak_of(5, 0, 128) == 0.0f);                            // nothing on SD's input
  }
  CHECK(finite && early > 0.05f && early <= 1.5f);               // sounds on the trig
  // each zone of ENG renders without NaN, after a trig with its knobs as the tail
  for (int z = 0; z < 17; ++z) {
    int v = (z * 128 + 64) / 17;
    CHECK(send({0x37, 0x44, (uint8_t)slot, 0, 60, 100, 0, 0, 0, 2, 1, 1, (uint8_t)(v * 64 >> 7), 0}));
    bool ok = true;
    float pk = 0.0f;
    for (int i = 0; i < 40; ++i) {
      block(128);
      for (int f = 0; f < 128; ++f) ok = ok && std::isfinite(g_o[4][f]);
      pk = std::fmax(pk, peak_of(4, 0, 128));
    }
    CHECK(ok && pk < 4.0f);
    if (!ok || pk >= 4.0f) printf("test_code: PLAITS zone %d: finite %d peak %f\n", z, (int)ok, pk);
  }
  printf("test_code: PLAITS loaded, placed, trigged (peak %.3f, after 1 s %.3f; %zu bytes)\n", early, late, vb.size());
}
#endif

int main() {
  char tmpl[] = "/tmp/core-code-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin());
  hooks.to_rytm = capture;
  CHECK(kv::link::attach_generic());
  test_check();

  void* dsp_arena = std::aligned_alloc(64, (size_t)dsp::kSlots * dsp::kSlotBytes);
  CHECK(dsp::begin(dsp_arena, (size_t)dsp::kSlots * dsp::kSlotBytes));
  // the code arena: 2 MB, 64 KB aligned, from mmap so mprotect applies
  const size_t arena_n = 2u << 20;
  uint8_t* raw = (uint8_t*)mmap(nullptr, arena_n + code::kUnit, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(raw != MAP_FAILED);
  uint8_t* arena = (uint8_t*)(((uintptr_t)raw + code::kUnit - 1) & ~(uintptr_t)(code::kUnit - 1));
  static uint8_t buf_a[code::kMaxBlob], buf_b[code::kMaxBlob];
  CHECK(!code::begin(arena + 1, arena_n, buf_a, buf_b, &kBoard));
  CHECK(code::begin(arena, arena_n, buf_a, buf_b, &kBoard));
  send({0x00, 0, 0, 0, 0, 1});   // the companion's HELLO
  answers();
  test_wire();
#if defined(__aarch64__)
  const char* tp = getenv("SCX_TGAIN");
  if (!tp) { fprintf(stderr, "test_code: SCX_TGAIN=<tgain.scx> is needed on AArch64\n"); return 1; }
  FILE* f = fopen(tp, "rb");
  CHECK(f);
  Bytes tg;
  uint8_t buf[4096];
  size_t k;
  while ((k = fread(buf, 1, sizeof buf, f)) > 0) tg.insert(tg.end(), buf, buf + k);
  fclose(f);
  test_tgain(tg);
  if (const char* vp = getenv("SCX_VOICE")) {
    FILE* g = fopen(vp, "rb");
    CHECK(g);
    Bytes vb;
    while (g && (k = fread(buf, 1, sizeof buf, g)) > 0) vb.insert(vb.end(), buf, buf + k);
    if (g) fclose(g);
    test_voice_blob(vb);
  } else {
    printf("test_code: no voice blob (SCX_VOICE=<plaits.scx> runs one)\n");
  }
#else
  printf("test_code: blob code not run (not AArch64; make -C core test-aarch64 runs it)\n");
#endif
  return 0;
}

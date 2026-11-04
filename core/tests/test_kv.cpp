// core/tests/test_kv.cpp - the record store (core/kv) and its 51 STORE codec: write / read round trips over chunks,
// handles as versions, KEEP dropping the rest, a reopen rebuilding the index from the folder, a power cut mid-write
// leaving the old records, malformed messages refused without side effects.
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "check.h"
#include "kv/kv.h"

using namespace core::kv;

struct MemBackend : Backend {
  std::map<std::string, std::vector<uint8_t>> files;
  bool fail_writes = false;
  int writes = 0;
  bool write_atomic(const char* p, const uint8_t* d, uint32_t n) override {
    if (fail_writes) return false;   // the cut: write_atomic leaves the old file (or none) in place
    ++writes;
    files[p] = std::vector<uint8_t>(d, d + n);
    return true;
  }
  int read(const char* p, uint8_t* d, uint32_t cap) override {
    auto it = files.find(p);
    if (it == files.end()) return -1;
    uint32_t n = (uint32_t)it->second.size() < cap ? (uint32_t)it->second.size() : cap;
    if (n) memcpy(d, it->second.data(), n);
    return (int)n;
  }
  bool remove(const char* p) override { return files.erase(p) == 1; }
  bool list(const char* dir, void (*each)(const char*, void*), void* ctx) override {
    std::string pre = std::string(dir) + "/";
    bool any = false;
    for (auto& f : files)
      if (f.first.compare(0, pre.size(), pre) == 0 && f.first.find('/', pre.size()) == std::string::npos) {
        each(f.first.c_str() + pre.size(), ctx);
        any = true;
      }
    return any;
  }
};

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static std::vector<uint8_t> msg(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, kCmd, kVer};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
static void put_h(std::vector<uint8_t>& b, int h) { b.push_back(h >> 14 & 3); b.push_back(h >> 7 & 0x7F); b.push_back(h & 0x7F); }
static int get_h(const uint8_t* b) { return b[0] << 14 | b[1] << 7 | b[2]; }

// the companion's side: a record written in chunks; returns the new handle, or -status
static int write_rec(Service& sv, uint8_t seq, uint8_t scope, const std::vector<uint8_t>& data) {
  g_out.clear();
  uint32_t off = 0, total = (uint32_t)data.size();
  do {
    uint32_t c = total - off < (uint32_t)kChunk ? total - off : (uint32_t)kChunk;
    std::vector<uint8_t> b = {kOpWrite, seq, scope, (uint8_t)(total >> 7), (uint8_t)(total & 0x7F), (uint8_t)(off >> 7), (uint8_t)(off & 0x7F)};
    uint8_t p[kChunk + kChunk / 7 + 1];
    int pn = pack7(data.data() + off, (int)c, p);
    b.insert(b.end(), p, p + pn);
    auto m = msg(b);
    sv.sysex(m.data(), (int)m.size());
    off += c;
  } while (off < total && g_out.empty());   // a refused chunk ends the write, as the companion does
  CHECK(g_out.size() == 1);
  auto& r = g_out[0];
  if (r[6] == kOpStatus) return -r[8];
  CHECK(r[6] == kOpWritten && r[7] == seq && r[8] == scope);
  return get_h(&r[9]);
}

static int read_rec(Service& sv, uint8_t seq, uint8_t scope, int h, std::vector<uint8_t>* data) {
  g_out.clear();
  std::vector<uint8_t> b = {kOpRead, seq, scope};
  put_h(b, h);
  auto m = msg(b);
  sv.sysex(m.data(), (int)m.size());
  CHECK(!g_out.empty());
  if (g_out[0][6] == kOpStatus) return -g_out[0][8];
  data->clear();
  for (auto& r : g_out) {
    CHECK(r[6] == kOpData && r[7] == seq && r[8] == scope && get_h(&r[9]) == h);
    uint32_t total = (uint32_t)r[12] << 7 | r[13], off = (uint32_t)r[14] << 7 | r[15];
    CHECK(off == data->size());
    uint8_t p[kChunk];
    int n = unpack7(&r[16], (int)r.size() - 17, p);
    CHECK(n > 0 || total == 0);
    data->insert(data->end(), p, p + (n > 0 ? n : 0));
    CHECK(data->size() <= total);
  }
  return 0;
}

static void hello(Service& sv, uint32_t project) {
  g_out.clear();
  auto m = msg({kOpHello, (uint8_t)(project >> 28 & 0x0F), (uint8_t)(project >> 21 & 0x7F), (uint8_t)(project >> 14 & 0x7F),
                (uint8_t)(project >> 7 & 0x7F), (uint8_t)(project & 0x7F)});
  sv.sysex(m.data(), (int)m.size());
}

int main() {
  // pack7 / unpack7 round trip over every length up to 3 groups, every byte value
  for (int n = 0; n <= 21; ++n) {
    uint8_t in[21], p[32], back[21];
    for (int i = 0; i < n; ++i) in[i] = (uint8_t)(i * 37 + 0x80 * (i & 1));
    int pn = pack7(in, n, p);
    CHECK(pn == n + (n + 6) / 7);
    for (int i = 0; i < pn; ++i) CHECK(p[i] < 0x80);
    CHECK(unpack7(p, pn, back) == n);
    CHECK(memcmp(in, back, (size_t)n) == 0);
  }
  { uint8_t bad[2] = {0x02, 0x10}, o[8]; CHECK(unpack7(bad, 2, o) == -1); }   // a high bit for a missing byte

  MemBackend be;
  Store st(&be);
  Service sv(&st, out);

  // before a HELLO nothing is stored
  CHECK(write_rec(sv, 1, 1, {1, 2, 3}) == -kNoProject);
  CHECK(be.writes == 0);

  hello(sv, 0x8BADF00D);
  CHECK(g_out.size() == 1 && g_out[0][6] == kOpHelloAck && g_out[0][8] == 0 && g_out[0][9] == 0);

  // a kit record (send FX's 8 bytes) and a 1000-byte pattern record (9 chunks)
  std::vector<uint8_t> kit = {0, 127, 64, 1, 2, 3, 0x80, 0xFF};
  std::vector<uint8_t> pat(1000);
  for (int i = 0; i < 1000; ++i) pat[i] = (uint8_t)(i * 7);
  int hk = write_rec(sv, 2, 1, kit), hp = write_rec(sv, 3, 3, pat);
  CHECK(hk > 0 && hp > 0 && hk != hp);
  std::vector<uint8_t> back;
  CHECK(read_rec(sv, 4, 1, hk, &back) == 0 && back == kit);
  CHECK(read_rec(sv, 5, 3, hp, &back) == 0 && back == pat);
  CHECK(be.files.count("/.rytm/store/8badf00d/1-0001.bin") == 1);

  // handles are versions: a second write of the kit is a new handle, the old one still reads
  std::vector<uint8_t> kit2 = kit;
  kit2[0] = 99;
  int hk2 = write_rec(sv, 6, 1, kit2);
  CHECK(hk2 > hp);
  CHECK(read_rec(sv, 7, 1, hk, &back) == 0 && back == kit);
  CHECK(read_rec(sv, 8, 1, hk2, &back) == 0 && back == kit2);
  CHECK(read_rec(sv, 9, 1, 0x1234, &back) == -kMissing);
  CHECK(read_rec(sv, 9, 2, hk, &back) == -kMissing);   // a handle is its scope's

  // an empty record is a record
  int he = write_rec(sv, 10, 5, {});
  CHECK(he > 0 && read_rec(sv, 11, 5, he, &back) == 0 && back.empty());

  // a cut during the write: refused, the index unchanged, the old records still read
  be.fail_writes = true;
  CHECK(write_rec(sv, 12, 1, kit) == -kCardError);
  be.fail_writes = false;
  CHECK(st.records() == 4);
  CHECK(read_rec(sv, 13, 1, hk2, &back) == 0 && back == kit2);

  // a chunk out of order is refused and stores nothing
  {
    g_out.clear();
    auto m = msg({kOpWrite, 14, 1, 0, 20, 0, 7, 0, 1, 2, 3, 4, 5, 6, 7});
    sv.sysex(m.data(), (int)m.size());
    CHECK(g_out.size() == 1 && g_out[0][6] == kOpStatus && g_out[0][8] == kBad);
    CHECK(st.records() == 4);
  }
  // too big
  CHECK(write_rec(sv, 15, 1, std::vector<uint8_t>(kMaxRecord + 1)) == -kTooBig);

  // reopen (a Pi restart): the index comes back from the folder, new handles never reuse a live one
  {
    Store st2(&be);
    Service sv2(&st2, out);
    hello(sv2, 0x8BADF00D);
    CHECK(g_out[0][6] == kOpHelloAck && ((g_out[0][8] << 7) | g_out[0][9]) == 4);
    CHECK(read_rec(sv2, 16, 1, hk2, &back) == 0 && back == kit2);
    int h = write_rec(sv2, 17, 1, kit);
    CHECK(h > hk2 && h > he);
  }
  hello(sv, 0x8BADF00D);   // the first service sees the new record too
  CHECK(st.records() == 5);

  // a stray file in the folder (a .tmp, another name) is not a record
  be.files["/.rytm/store/8badf00d/1-0001.bin.tmp"] = {1};
  be.files["/.rytm/store/8badf00d/notes.txt"] = {1};
  hello(sv, 0x8BADF00D);
  CHECK(st.records() == 5);

  // KEEP in two messages: kit hk2 and the pattern stay, the rest go; the stray files are not touched
  {
    g_out.clear();
    std::vector<uint8_t> b1 = {kOpKeep, 18, 0, 1, 1};
    put_h(b1, hk2);
    auto m1 = msg(b1);
    sv.sysex(m1.data(), (int)m1.size());
    CHECK(g_out.empty());
    std::vector<uint8_t> b2 = {kOpKeep, 18, 1, 1, 3};
    put_h(b2, hp);
    auto m2 = msg(b2);
    sv.sysex(m2.data(), (int)m2.size());
    CHECK(g_out.size() == 1 && g_out[0][6] == kOpKept && ((g_out[0][8] << 7) | g_out[0][9]) == 3);
    CHECK(st.records() == 2);
    CHECK(read_rec(sv, 19, 1, hk, &back) == -kMissing);
    CHECK(read_rec(sv, 20, 1, hk2, &back) == 0 && back == kit2);
    CHECK(read_rec(sv, 21, 3, hp, &back) == 0 && back == pat);
    CHECK(be.files.count("/.rytm/store/8badf00d/notes.txt") == 1);
  }
  // a malformed KEEP list removes nothing
  {
    g_out.clear();
    std::vector<uint8_t> b = {kOpKeep, 22, 1, 1, 1, 4, 0, 0};   // handle's top byte 4: bad
    auto m = msg(b);
    sv.sysex(m.data(), (int)m.size());
    CHECK(g_out.size() == 1 && g_out[0][6] == kOpStatus && g_out[0][8] == kBad);
    CHECK(st.records() == 2);
  }

  // another project: its own folder, the first project's records unseen
  hello(sv, 7);
  CHECK(st.records() == 0);
  CHECK(read_rec(sv, 23, 1, hk2, &back) == -kMissing);
  int h7 = write_rec(sv, 24, 1, kit);
  CHECK(h7 == 1 && be.files.count("/.rytm/store/00000007/1-0001.bin") == 1);

  // wrong version / not ours / a status byte inside: refused or ignored, never stored
  {
    g_out.clear();
    std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, kCmd, 2, kOpHello, 0, 0, 0, 0, 1, 0xF7};
    sv.sysex(m.data(), (int)m.size());
    CHECK(g_out.size() == 1 && g_out[0][8] == kBad);
    g_out.clear();
    std::vector<uint8_t> o = {0xF0, 0x7D, 0x52, 0x54, 0x50, 1, 0, 0xF7};
    sv.sysex(o.data(), (int)o.size());
    CHECK(g_out.empty());
  }

  printf("test_kv: PASS\n");
  return 0;
}

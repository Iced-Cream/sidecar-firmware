// core/tests/test_store_link.cpp - 51 STORE through the engine: host_sysex on the control side, the record store on
// the card (core::File / Fs, fmt::write_atomic, a temp folder as the card), the answers through hooks.to_rytm; the lock
// ops on the control side; the companion's HELLO forgetting the lock sets and keeping the records (the Rytm's 35 leaving them).
#include <sys/stat.h>
#include <cstring>
#include <vector>

#include "check.h"
#include "host_link.h"
#include "hooks.h"
#include "kv/kv.h"
#include "kv/link.h"
#include "locks/locks.h"
#include "platform.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void send(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, kv::kCmd, kv::kVer};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  host_sysex(m.data(), (int)m.size());
  for (int i = 0; i < 64 && kv::link::host_step(8); ++i) {}   // (no card core here: host_step runs the card ops)
}
static int h3(const uint8_t* b) { return b[0] << 14 | b[1] << 7 | b[2]; }

int main() {
  char tmpl[] = "/tmp/core-store-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin());
  hooks.to_rytm = out;

  // HELLO project 0x0000BEEF
  send({kv::kOpHello, 0, 0, 2, 0x7D, 0x6F});
  CHECK(g_out.size() == 1 && g_out[0][6] == kv::kOpHelloAck);
  // WRITE an 8-byte kit record in one chunk
  g_out.clear();
  uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 0x81}, p[16];
  int pn = kv::pack7(data, 8, p);
  std::vector<uint8_t> w = {kv::kOpWrite, 1, 1, 0, 8, 0, 0};
  w.insert(w.end(), p, p + pn);
  send(w);
  CHECK(g_out.size() == 1 && g_out[0][6] == kv::kOpWritten);
  int h = h3(&g_out[0][9]);
  CHECK(h == 1);
  char path[200];
  snprintf(path, sizeof path, "%s/.rytm/store/0000beef/1-0001.bin", tmpl);
  struct stat st;
  CHECK(stat(path, &st) == 0 && st.st_size == 8);
  // READ it back
  g_out.clear();
  send({kv::kOpRead, 2, 1, 0, 0, 1});
  CHECK(g_out.size() == 1 && g_out[0][6] == kv::kOpData);
  uint8_t back[16];
  CHECK(kv::unpack7(&g_out[0][16], (int)g_out[0].size() - 17, back) == 8 && memcmp(back, data, 8) == 0);
  // a lock op is the control side's, answered by nobody
  g_out.clear();
  send({locks::kSet, 0, 12, 1, 0, 4});
  CHECK(g_out.empty() && locks::declared(0));
  // the Rytm's hello (35) leaves the sets; the companion's HELLO starts a session: the lock sets go, the records stay
  std::vector<uint8_t> hello35 = {0xF0, 0x7D, 0x52, 0x54, 0x35, 0x01, 0, 0, 0, 0xF7};
  host_sysex(hello35.data(), (int)hello35.size());
  CHECK(locks::declared(0));
  g_out.clear();
  send({kv::kOpHello, 0, 0, 2, 0x7D, 0x6F});
  CHECK(!locks::declared(0));
  CHECK(g_out.size() >= 1 && g_out[0][6] == kv::kOpHelloAck && ((g_out[0][8] << 7) | g_out[0][9]) == 1);
  // KEEP nothing: the record is removed from the card
  g_out.clear();
  send({kv::kOpKeep, 3, 1, 0});
  CHECK(g_out.size() == 1 && g_out[0][6] == kv::kOpKept && g_out[0][9] == 1);
  CHECK(stat(path, &st) != 0);
  // 04 NAME: kept on the control side (no answer), a listener told once per change; a malformed one changes nothing
  static int names = 0;
  CHECK(kv::link::on_name([](const char*, uint32_t) { ++names; }));
  g_out.clear();
  send({kv::kOpName, 0, 0, 2, 0x7D, 0x6F, 5, 'D', 'R', 'U', 'M', 'S'});
  send({kv::kOpName, 0, 0, 2, 0x7D, 0x6F, 5, 'D', 'R', 'U', 'M', 'S'});
  uint32_t pid = 0;
  CHECK(!strcmp(kv::link::project_name(&pid), "DRUMS") && pid == (2u << 14 | 0x7Du << 7 | 0x6F) && names == 1);
  CHECK(g_out.empty());
  send({kv::kOpName, 0, 0, 2, 0x7D, 0x6F, 6, 'X', 'Y'});   // len says 6, 2 bytes
  send({kv::kOpName, 0, 0, 2, 0x7D, 0x6F, 17, 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A'});
  CHECK(!strcmp(kv::link::project_name(), "DRUMS") && names == 1);
  send({kv::kOpName, 0, 0, 0, 0, 9, 16, 'L', 'O', 'N', 'G', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});   // the stock 16 bytes, NUL-ended
  CHECK(!strcmp(kv::link::project_name(&pid), "LONG") && pid == 9 && names == 2);
  printf("test_store_link: PASS\n");
  return 0;
}

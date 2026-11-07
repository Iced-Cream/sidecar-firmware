// core/tests/test_status_page.cpp - the generic engine's status page (core/screen/status_page): drawn as the screen's
// client when the Rytm opens its page, the mask (only NO caught), the clock's state appearing at the next refresh, the
// address, and NO closing the page.
#include <string.h>
#include <string>
#include <vector>

#include "check.h"
#include "../generic/generic.h"
#include "../hooks.h"
#include "../screen/screen.h"
#include "../screen/status_page.h"

namespace core { void host_fake_time(bool on, uint64_t us); }
using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void out_fn(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void in(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  gen::sysex(m.data(), (int)m.size());
}
static uint64_t g_us = 1000000;
static void run(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 5) {
    g_us += 5000, host_fake_time(true, g_us);
    screen::status_page::tick((uint32_t)(g_us / 1000));
    gen::tick((uint32_t)(g_us / 1000));
  }
}
// every 3D text sent so far (F0 7D 52 54 3D 01 seq slot x y font flags <chars> F7)
static bool texted(const char* s) {
  for (auto& m : g_out)
    if (m.size() > 13 && m[4] == 0x3D && std::string(m.begin() + 12, m.end() - 1).find(s) != std::string::npos) return true;
  return false;
}
static const std::vector<uint8_t>* last(uint8_t cmd) {
  for (auto it = g_out.rbegin(); it != g_out.rend(); ++it) if (it->size() > 4 && (*it)[4] == cmd) return &*it;
  return nullptr;
}

int main() {
  host_fake_time(true, g_us);
  hooks.to_rytm = out_fn;
  screen::set_client(screen::status_page::client());
  screen::status_page::set_address("192.168.1.217");
  in({0x35, 1, 0, 100 & 0x7F, 1});   // the Rytm's hello (caps 1)
  run(3500);                          // the splash comes and goes
  g_out.clear();
  in({0x30, 1, 0, 1, 0});             // TRK + YES on the Rytm
  CHECK(screen::open());
  run(60);
  CHECK(texted("SIDECAR STATUS") && texted("PROJECT") && texted("CLOCK") && texted("STORE") && texted("ROUTE"));
  CHECK(texted("192.168.1.217") && texted("STOP"));
  // the mask: no encoder, of the keys only NO (0x45: byte 3 + 9, bit 6)
  const std::vector<uint8_t>* m = last(0x37);
  CHECK(m && (*m)[6] == 0 && (*m)[7] == 0 && (*m)[8] == 0);
  CHECK(m && m->size() == 4 + 2 + 3 + 10 + 1 && (*m)[6 + 3 + 9] == (1 << 6));
  for (size_t i = 9; m && i < m->size() - 1; ++i) CHECK(i == 6 + 3 + 9 || (*m)[i] == 0);
  // PLAY on the Rytm: on the page at the next refresh
  g_out.clear();
  in({0x2D, 1, 1, 1, 0, 0, 0, 5});
  run(screen::status_page::kRefreshMs + 50);
  CHECK(texted("PLAY"));
  // NO: the page asks the Rytm to close (3A 0 0)
  g_out.clear();
  in({0x31, 1, 0, 0x45, 1});
  CHECK(last(0x3A) && (*last(0x3A))[6] == 0 && (*last(0x3A))[7] == 0);
  in({0x30, 1, 0, 0, 1});             // the Rytm closes it
  CHECK(!screen::open());
  g_out.clear();
  run(1000);
  CHECK(!last(0x3D));                 // closed: nothing drawn
  printf("test_status_page: ok\n");
  return 0;
}

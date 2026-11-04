// core/tests/test_generic.cpp - the stripped engine's core (core/generic, core/screen): the host link's
// routing (4F, 10, 35, 2C / 2D, 30..3F, 51..53, everything else dropped), the link reset's listeners, the remote
// screen's transport with and without a client: the connect splash at a hello, a page the Rytm opens closed again with
// no client, a client's frame, keys and encoders, the pairing modal over an open page.
#include <string.h>
#include <vector>

#include "check.h"
#include "../clock/clock.h"
#include "../generic/generic.h"
#include "../hooks.h"
#include "../screen/screen.h"

namespace core { void host_fake_time(bool on, uint64_t us); }
using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void out_fn(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static int sent(uint8_t cmd) {
  int k = 0;
  for (auto& m : g_out) k += m.size() > 4 && m[4] == cmd;
  return k;
}
static const std::vector<uint8_t>* last(uint8_t cmd) {
  for (auto it = g_out.rbegin(); it != g_out.rend(); ++it) if (it->size() > 4 && (*it)[4] == cmd) return &*it;
  return nullptr;
}
static void in(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  gen::sysex(m.data(), (int)m.size());
}
static uint64_t g_us = 1000000;
static void run(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 5) g_us += 5000, host_fake_time(true, g_us), gen::tick((uint32_t)(g_us / 1000));
}

static std::vector<const char*> g_projects;
static int g_links = 0;
static void proj_fn(const char* n) { g_projects.push_back(n); }
static void link_fn(const char*) { ++g_links; }
static int g_keys = 0, g_encs = 0, g_draws = 0, g_opens = 0;
static void c_draw(ui::Canvas& c, uint32_t) { ++g_draws; c.put_text(0, ui::kRSmall, 2, 1, "CLIENT"); c.hline(0, 127, 20); }
static void c_key(int, bool down) { g_keys += down; }
static void c_enc(int, int d, bool) { g_encs += d; }
static void c_open(bool on, int) { g_opens += on ? 1 : -1; }
static const screen::Client kClient = {"test", c_draw, c_key, c_enc, c_open, nullptr};
static bool g_store_seen = false;
static bool store_fn(const uint8_t* d, int) { g_store_seen = d[4] == 0x51; return true; }

int main() {
  host_fake_time(true, g_us);
  hooks.to_rytm = out_fn;
  CHECK(gen::on_project(proj_fn) && gen::on_link(link_fn));
  // 10: the project's name, once per change
  in({0x10, 'T', 'E', 'S', 'T'});
  in({0x10, 'T', 'E', 'S', 'T'});
  CHECK(!strcmp(gen::project(), "TEST") && g_projects.size() == 1);
  // a machine report, a send FX message: dropped, nothing sent back
  uint32_t dropped = gen::stats().dropped;
  in({0x04, 0, 60, 100});
  in({0x20, 1, 2});
  CHECK(gen::stats().dropped == dropped + 2 && g_out.empty());
  // 51 STORE: to the handler once one is set
  in({0x51, 1});
  CHECK(gen::stats().dropped == dropped + 3);
  gen::set_store(store_fn);
  in({0x51, 1});
  CHECK(g_store_seen && gen::stats().dropped == dropped + 3);
  // 2C: to the clock
  in({0x2C, 1, 0, 0, 0, 9, 1, 3, 4, 16, 2});
  int st, ln, sp;
  CHECK(clock::step_of(3, &st, &ln, &sp) && st == 4);
  // 35 hello (Rytm up 5 s, caps 1): the link reset (the clock forgets), the splash (36 64, a frame, then 36 0)
  in({0x35, 1, 0, 100 & 0x7F, 1});
  CHECK(g_links == 1 && !clock::step_of(3, &st, &ln, &sp) && screen::greeted() && screen::caps() == 1);
  run(50);
  CHECK(sent(0x39) >= 1 && last(0x36) && (*last(0x36))[6] == 64);
  run(3100);
  CHECK((*last(0x36))[6] == 0);
  // no client: a page the Rytm opens is closed again (3A 0 0)
  g_out.clear();
  in({0x30, 1, 0, 1, 0});
  CHECK(!screen::open() && last(0x3A) && (*last(0x3A))[7] == 0);
  // a client: opened, drawn, keys and encoders, keepalives, closed
  screen::set_client(&kClient);
  g_out.clear();
  in({0x30, 1, 0, 1, 0});
  CHECK(screen::open() && g_opens == 1);
  run(60);
  CHECK(g_draws >= 1 && sent(0x37) == 1 && sent(0x39) >= 1 && sent(0x3D) >= 1);
  in({0x31, 1, 0, 0x40, 1});
  in({0x32, 1, 2, 64 + 3, 0});
  CHECK(g_keys == 1 && g_encs == 3);
  run(600);
  CHECK(sent(0x3C) >= 1);
  // the pairing modal over the open page: drawn on top, a key takes it off and the client never sees that key
  CHECK(screen::popup("PAIR", "1234 5678", 30000, true));
  run(60);
  int keys = g_keys;
  in({0x31, 1, 0, 0x45, 1});
  in({0x31, 1, 0, 0x45, 0});
  run(60);
  CHECK(g_keys == keys && screen::popups() == 1);
  in({0x30, 1, 0, 0, 0});
  CHECK(!screen::open() && g_opens == 0);
  // a status popup with the page closed: 36 holds it, then 36 0
  g_out.clear();
  CHECK(screen::popup("HELLO", nullptr, 0, false));
  run(50);
  CHECK(last(0x36) && (*last(0x36))[6] > 0);
  run(1000);
  CHECK((*last(0x36))[6] == 0 && screen::status_popups() == 1);
  printf("test_generic: ok\n");
  return 0;
}

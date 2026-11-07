// core/screen/status_page.cpp - status_page.h. Text items only (the Rytm draws them in its own small font); one row
// every 8 px under the title bar, label at x 2, value at x 42 (kRSmall: 5 px a character at most, 17 characters there).
#include "status_page.h"

#include <string.h>

#include "../clock/clock.h"
#include "../dsp/dsp.h"
#include "../kv/link.h"
#include "../locks/locks.h"
#include "../platform.h"
#include "../route/route.h"
#include "../ui/ui.h"

namespace core {
namespace screen {
namespace status_page {
namespace {

using ui::Canvas;
using ui::kRSmall;

constexpr int kKeyNo = 0x45;
constexpr int kValueX = 42;
char g_ip[24];
uint32_t g_last;
uint8_t g_mask[kMaskBytes];

void row(Canvas& c, int slot, int y, const char* label, const char* value) {
  c.put_text(slot, kRSmall, 2, y, label);
  c.put_text(slot + 1, kRSmall, kValueX, y, value);
}

void draw(Canvas& c, uint32_t ms) {
  char v[40];
  c.put_text(0, kRSmall, 2, 1, "SIDECAR STATUS");
  c.hline(0, ui::kW - 1, 8);
  int y = 11, slot = 2;
  // the project, as the companion's store link names it (51 STORE 04 NAME)
  uint32_t id = 0;
  const char* name = kv::link::project_name(&id);
  if (name && name[0]) sniprintf(v, sizeof v, "%s", name);
  else sniprintf(v, sizeof v, "%s", id ? "(no name)" : "-");
  row(c, slot, y, "PROJECT", v), slot += 2, y += 8;
  // the step clock: transport, tempo, pattern as the Rytm names it (A01..H16)
  const uint8_t t = clock::transport();
  const int p = clock::pattern();
  const int b10 = (int)(clock::bpm() * 10.0 + 0.5);
  char pat[8];
  if (p >= 0) sniprintf(pat, sizeof pat, "%c%02d", 'A' + p / 16, p % 16 + 1);
  else sniprintf(pat, sizeof pat, "-");
  if (b10 > 0) sniprintf(v, sizeof v, "%s %d.%d %s", t == clock::kPlay ? "PLAY" : t == clock::kPause ? "PAUSE" : "STOP",
                         b10 / 10, b10 % 10, pat);
  else sniprintf(v, sizeof v, "%s %s", t == clock::kPlay ? "PLAY" : t == clock::kPause ? "PAUSE" : "STOP", pat);
  row(c, slot, y, "CLOCK", v), slot += 2, y += 8;
  // the record store and the Pi-side locks
  const kv::link::StoreStats ks = kv::link::store_stats();
  sniprintf(v, sizeof v, "%d REC %d LOCK SETS", ks.records, locks::stats().sets);
  row(c, slot, y, "STORE", v), slot += 2, y += 8;
  // the route's open points and the DSP library's used slots
  sniprintf(v, sizeof v, "%d POINTS %d DSP", route::stats().open, dsp::stats().used);
  row(c, slot, y, "ROUTE", v), slot += 2, y += 8;
  row(c, slot, y, "NET", g_ip[0] ? g_ip : "-"), slot += 2, y += 8;
  const uint32_t s = ms / 1000;
  sniprintf(v, sizeof v, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
  row(c, slot, y, "UPTIME", v);
}

void key(int code, bool down) {
  if (code == kKeyNo && down) request(false);   // NO: back to the Rytm's own screen
}

void opened(bool on, int) {
  if (on) g_last = 0;
}

const Client kClient = {"status", draw, key, nullptr, opened, g_mask};

}  // namespace

const Client* client() {
  memset(g_mask, 0, sizeof g_mask);   // no encoder; of the keys only NO
  g_mask[3 + kKeyNo / 7] = (uint8_t)(1u << (kKeyNo % 7));
  return &kClient;
}
void set_address(const char* ip) { sniprintf(g_ip, sizeof g_ip, "%s", ip ? ip : ""); }
void tick(uint32_t ms) {
  if (!open()) return;
  if (g_last == 0 || ms - g_last >= kRefreshMs) g_last = ms ? ms : 1, redraw();
}

}  // namespace status_page
}  // namespace screen
}  // namespace core

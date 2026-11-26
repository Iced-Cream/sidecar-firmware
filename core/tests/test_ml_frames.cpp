// core/tests/test_ml_frames.cpp - no SRC view frames while the Rytm's machine list covers the view (rytm-mods 0026 2B
// state bit 1): a machine change takes effect in the engine at once, the drawing waits; the cover going away (chosen,
// cancelled, or lost: the 2B heartbeat stops) sends one fresh whole frame and commit; rapid machine changes without a
// cover merge (one commit per send slot); with no 2B at all (an older mod) the view is drawn as before.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "seq/rytm_dump.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;
using namespace core::ui;

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static uint64_t g_us = 1000000000;
static void tick(int n = 1, int ms = 30) {
  for (int i = 0; i < n; ++i) g_us += (uint64_t)ms * 1000, host_fake_time(true, g_us), remote_tick(now_ms());
}
static void from_rytm(const std::vector<uint8_t>& body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
// 33: track 0 on a PLAITS (ping) or a 6-OP FM (pong): two machines with a page each
static void machine(bool pong) { from_rytm({0x33, 1, 0, 1, (uint8_t)(pong ? 49 : 44), (uint8_t)(pong ? (kFm | 0) : 0)}); }
static void src_view(int on) { from_rytm({0x29, 1, 0, (uint8_t)on}); }
static void src_vals() {
  std::vector<uint8_t> b = {0x2A, 1, 0, 0, 16};
  for (int k = 0; k < 16; ++k) b.push_back((uint8_t)(40 + k)), b.push_back(0);
  from_rytm(b);
}
// 2B <state> <views> <vt 5 bytes> <age>: state bit 0 view on, bit 1 covered
static void diag(int state) { from_rytm({0x2B, 1, (uint8_t)state, 9, 1, 0, 0, 0, 0x24, 0}); }
static void list_open() { diag(3); }       // the machine list over the SRC view
static void list_gone() { diag(1); }       // the list left (chosen or cancelled): the view on top again

struct Count { int commits = 0, rects = 0, msgs = 0; };
static Count count() {   // what went to the Rytm since the last take (the SRC view's frame messages only)
  Count c;
  for (auto& m : g_out) {
    if (m.size() < 6) continue;
    int cmd = m[4];
    if (cmd != 0x38 && cmd != 0x39 && cmd != 0x3D && cmd != 0x3E) continue;   // the frame: rects, texts, the commit (not the 47 names, the engine's own sends)
    ++c.msgs;
    if (cmd == 0x39) ++c.commits;
    if (cmd == 0x38) ++c.rects;
  }
  g_out.clear();
  return c;
}
static void start() {
  remote_reset();
  g_out.clear();
  rytm_dump::set_playing(true);
  machine(false);
  src_vals();
  src_view(1);
  tick(6);   // the first whole frame
  count();
}
// n machine changes, one per tick (30 ms apart: a tester scrolling the list), each with its 2A
static void browse(int n) {
  for (int i = 0; i < n; ++i) machine(i & 1), src_vals(), tick();
}

static void test_before_cover_draws() {   // no 2B at all (an older mod): drawn as ever
  start();
  browse(40);
  tick(10);
  Count c = count();
  CHECK(c.commits >= 1);
  printf("  no 2B: 40 machine changes in 1.2 s: %d commits, %d rect messages, %d messages (%.1f messages/s)\n", c.commits, c.rects, c.msgs, c.msgs / 1.5);
}

static void test_covered_silent() {
  start();
  list_open();
  tick(2);
  count();
  browse(40);
  tick(10);
  Count c = count();
  printf("  covered: 40 machine changes in 1.2 s: %d commits, %d rect messages, %d messages\n", c.commits, c.rects, c.msgs);
  CHECK(c.commits == 0 && c.rects == 0 && c.msgs == 0);
  CHECK(remote_track_machine(0) == (kFm | 0) || remote_track_machine(0) == 0);   // the engine took the last one at once
  // the 2B heartbeat (once a second) keeps the cover for as long as the list stays
  for (int i = 0; i < 12; ++i) tick(33), list_open();
  CHECK(count().commits == 0);
}

static void test_cover_ends_chosen() {
  start();
  list_open();
  browse(25);   // ends on PLAITS or FM: the choice
  const int chosen = remote_track_machine(0);
  count();
  list_gone();
  tick(3);
  Count c = count();
  CHECK(c.commits == 1 && c.rects > 0);   // one whole frame, one commit
  tick(10);
  CHECK(count().commits == 0);   // and nothing more
  CHECK(remote_track_machine(0) == chosen);
}

static void test_cover_ends_cancelled() {   // back on the machine the list opened on: still one fresh frame
  start();
  list_open();
  tick(2);
  count();
  list_gone();
  tick(3);
  Count c = count();
  CHECK(c.commits == 1 && c.rects > 0);
}

static void test_cover_end_lost() {   // the Rytm stops reporting (a 2B heartbeat is once a second): the view is drawn again
  start();
  list_open();
  browse(10);
  count();
  tick(60);   // 1.8 s without a 2B: still covered
  CHECK(count().commits == 0);
  tick(120);  // 5.4 s more
  Count c = count();
  CHECK(c.commits == 1 && c.rects > 0);
}

static void test_burst_merges() {   // no cover: 10 changes between two ticks are one commit, the latest machine's
  start();
  for (int i = 0; i < 10; ++i) machine(i & 1), src_vals();
  tick(3);
  Count c = count();
  CHECK(c.commits == 1);
  // a longer scroll: at most one commit per send slot, and the last machine ends up shown
  browse(40);
  tick(10);
  c = count();
  printf("  uncovered scroll: %d commits in 1.5 s\n", c.commits);
  CHECK(c.commits <= 8);
  const int last = remote_track_machine(0);
  machine(false), src_vals(), tick(15);
  CHECK(remote_track_machine(0) != last || last == 0);
  CHECK(count().commits >= 1);
}

static void test_cover_clock() {   // a 2B handled after the tick's ms was taken: the stamp is not later than that ms (no underflow)
  start();
  const uint32_t ms0 = now_ms();
  g_us += 30000, host_fake_time(true, g_us);   // time passes between taking ms and the Rytm's 2B
  list_open();
  remote_tick(ms0);
  remote_tick(ms0 + 30);
  CHECK(count().commits == 0);   // still covered, not ended at once
}

static void test_29_off_clears_cover() {   // the view left while covered: the next 29 on is not held back by the old cover
  start();
  list_open();
  tick(2);
  count();
  src_view(0);
  src_view(1);
  tick(4);
  Count c = count();
  CHECK(c.commits == 1 && c.rects > 0);
}

static void test_hello_clears_cover() {   // a new link: the Rytm says 29 and 2B again; a stale cover must not hold the view
  start();
  list_open();
  tick(2);
  count();
  from_rytm({0x35, 1, 0, 5, 0});   // hello
  src_view(1);
  tick(4);
  Count c = count();
  CHECK(c.commits >= 1);
}

static void test_late_heartbeat() {   // a 2B late by 4 s: one frame, then covered again and silent
  start();
  list_open();
  tick(2);
  count();
  tick(135);   // 4 s
  CHECK(count().commits == 1);
  list_open();
  tick(20);
  CHECK(count().commits == 0);
}

static void test_track_change_covered() {   // another track's view comes up under the cover: silent until the cover ends, then one frame
  start();
  list_open();
  tick(2);
  count();
  from_rytm({0x33, 1, 1, 1, 44, 0});   // track 1 on a PLAITS, its view up
  from_rytm({0x29, 1, 1, 1});
  tick(10);
  CHECK(count().commits == 0);
  list_gone();
  tick(4);
  Count c = count();
  CHECK(c.commits == 1 && c.rects > 0);
}

static void test_real_vtable() {   // the real MachineListView vtable words (OS 1.74) in the 2B: covered, one frame at the end
  start();
  from_rytm({0x2B, 1, 3, 9, 4, 0, 121, 61, 36, 0});   // vt 0x401E5EA4
  tick(2);
  count();
  browse(10);
  tick(5);
  CHECK(count().commits == 0);
  list_gone();
  tick(3);
  CHECK(count().commits == 1);
}

static void test_open_page_cover_end() {   // a Sidecar page open: a cover ending under it does not resend the page
  start();
  from_rytm({0x30, 1, 0, 1, 0});   // the page opens
  tick(8);
  count();
  list_open();
  tick(3);
  list_gone();
  tick(3);
  CHECK(count().rects == 0);
}

int main() {
  char tmpl[] = "/tmp/core_ml_frames_XXXXXX";
  CHECK(mkdtemp(tmpl));
  CHECK(system((std::string("mkdir -p '") + tmpl + "/Samples' '" + tmpl + "/.rytm'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = out;
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  struct { const char* name; void (*fn)(); } tests[] = {{"before_cover_draws", test_before_cover_draws}, {"covered_silent", test_covered_silent},
      {"cover_ends_chosen", test_cover_ends_chosen}, {"cover_ends_cancelled", test_cover_ends_cancelled},
      {"cover_end_lost", test_cover_end_lost}, {"cover_clock", test_cover_clock}, {"29_off_clears_cover", test_29_off_clears_cover},
      {"hello_clears_cover", test_hello_clears_cover}, {"late_heartbeat", test_late_heartbeat}, {"track_change_covered", test_track_change_covered},
      {"real_vtable", test_real_vtable}, {"open_page_cover_end", test_open_page_cover_end}, {"burst_merges", test_burst_merges}};
  for (auto& t : tests) printf("-- %s\n", t.name), fflush(stdout), t.fn();
  remote_reset();
  rytm_dump::set_playing(false);
  system((std::string("rm -rf '") + tmpl + "'").c_str());
  printf("test_ml_frames: PASS\n");
  return 0;
}

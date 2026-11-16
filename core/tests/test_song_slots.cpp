// core/tests/test_song_slots.cpp - SONG RECORD per slot (3(c) step 10c, core/song/song.h "Per slot"): the Rytm on slot 0
// and the A4 on slot 1 each record their own take from their own stream, at the same time, into their own folder
// (/.rytm/songs, /.a4/songs), with their own clock, transport, tempo, mutes, CC / NRPN and program changes. The A4's
// take has no stock snapshot and no replay yet (its dumps are not traced): nothing is asked of it, the replay refused.
// The Rytm's stays as it always was: its arm asks its dumps, its MIDI CONFIG is midi_config() whatever the slot in hand.
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/a4.h"
#include "device/device.h"
#include "engine.h"
#include "fmt/fmt.h"
#include "fx/send_fx.h"
#include "host_link.h"
#include "pool/pool.h"
#include "seq/seq.h"
#include "song/song.h"

using namespace core;


static std::vector<std::vector<uint8_t>> g_out, g_out1;
static void out0(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void out1(const uint8_t* m, int n) { g_out1.emplace_back(m, m + n); }

static int64_t g_frame = 0;
static uint64_t g_us = 1000000;
static int64_t frames() { return g_frame; }

static void put(const char* path, const char* text) {
  FILE* f = fopen(host_path(path).c_str(), "wb");
  CHECK(f);
  fputs(text, f);
  fclose(f);
}
static std::string get(const char* path) {
  FILE* f = fopen(host_path(path).c_str(), "rb");
  if (!f) return "";
  std::string s;
  char b[512];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static bool exists(const char* path) {
  struct stat sb;
  return stat(host_path(path).c_str(), &sb) == 0;
}
static void settle() {
  idle_flush(true);
  for (int i = 0; i < 4000; ++i) {
    device::step();
    idle_prepare();
    idle_save();
    idle_stream();
    bool loading = project_loading();
    for (int k = 1; k < kVoiceSlots; ++k) {
      device::Scope in(device::g_links[k]);
      loading = loading || project_loading();
    }
    if (i > 50 && idle_settled() && !loading) break;
  }
  idle_flush(false);
}

static std::vector<uint8_t> msg(uint8_t cmd, std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, cmd, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
static std::vector<uint8_t> blk4(int64_t frame) {
  uint32_t b = (uint32_t)(frame / 32) & ((1u << 28) - 1);
  return {(uint8_t)(b >> 21 & 0x7F), (uint8_t)(b >> 14 & 0x7F), (uint8_t)(b >> 7 & 0x7F), (uint8_t)(b & 0x7F)};
}
static std::vector<uint8_t> with_blk(std::vector<uint8_t> v) {
  auto b = blk4(g_frame);
  v.insert(v.end(), b.begin(), b.end());
  return v;
}
// a slot's stream, byte by byte, as the platform's tap hands it over (pi/engine/bridge.cpp link_byte: the slot's Scope)
static void feed(int slot, const std::vector<uint8_t>& m) {
  device::Scope in(device::g_links[slot]);
  for (uint8_t b : m) song::byte(b);
}
static std::string cmd(int slot, const char* c) {
  device::Scope in(device::g_links[slot]);
  char b[400];
  CHECK(song::command(c, b, sizeof b));
  return b;
}
static song::Status status(int slot) {
  device::Scope in(device::g_links[slot]);
  return song::status();
}
static void step(int ms) {   // time on, the song's loop and card jobs (every slot's), the posted writes
  for (int i = 0; i < ms; ++i) {
    g_us += 1000, g_frame += 48;
    host_fake_time(true, g_us);
    song::tick(now_ms());
    fmt::post_step();
    for (int k = 0; k < 16 && song::idle(); ++k) {}
  }
}
static int count(const std::string& s, const char* what) {
  int n = 0;
  for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) ++n;
  return n;
}

int main() {
  if (kVoiceSlots < 2) {
    printf("test_song_slots: PASS (an 8-voice engine: one slot, nothing to test)\n");
    return 0;
  }
  char tmpl[] = "/tmp/core-songslots-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/.rytm") && fs.mkdir("/.a4"));
  put("/.rytm/project", "RYP");
  put("/.a4/project", "A4P");
  hooks.to_rytm = out0;
  CHECK(engine_begin());
  device::set_out(1, out1);
  settle();
  // the A4 on the second transport, beside the Rytm: slot 1
  device::rytm_on_usb(true);
  {
    const uint8_t h[] = {0xf0, 0x7d, 0x52, 0x54, device::kCmdHello, 1, (uint8_t)(device::kIdA4 >> 7),
                         (uint8_t)(device::kIdA4 & 0x7f), 1, 0xf7};
    engine_sysex_link(1, h, (int)sizeof h);
  }
  settle();
  CHECK(device::g_links[1].box == &device::kA4 && device::g_base == &device::g_links[0]);
  CHECK(device::route_of(0) == 0 && device::route_of(1) == 1);   // (the platform's taps: each transport's slot)
  CHECK(!strcmp(prj(0).key, "RYP") && !strcmp(prj(1).key, "A4P"));

  song::test_frames(frames);
  song::reset();
  step(20);
  CHECK(status(0).state == song::kOff && status(1).state == song::kOff);

  // ---- arm both. The Rytm's arm asks its stock dumps (0x6D first: its GLOBAL); the A4's asks nothing of the A4
  g_out.clear(), g_out1.clear();
  feed(0, msg(0x2D, {7, 120 * 120 >> 14, (120 * 120 >> 7) & 127, 120 * 120 & 127}));   // the Rytm at 120 BPM
  feed(1, msg(0x2D, {7, 93 * 120 >> 14, (93 * 120 >> 7) & 127, 93 * 120 & 127}));      // the A4 at 93
  cmd(0, "song=arm");
  cmd(1, "song=arm");
  step(5);
  int req0 = 0, req1 = 0;
  for (auto& m : g_out) req0 += m.size() == 15 && m[1] == 0 && m[2] == 0x20 && m[3] == 0x3C;
  for (auto& m : g_out1) req1 += m.size() >= 7 && m[0] == 0xF0 && m[1] == 0 && m[2] == 0x20 && m[3] == 0x3C;
  printf("  armed: slot 0 %s (%d dump requests to the Rytm), slot 1 %s (%d to the A4): %s\n", song::state_name(status(0).state),
         req0, song::state_name(status(1).state), req1, status(1).msg);
  CHECK(req0 >= 1 && req1 == 0);
  CHECK(status(0).state == song::kArming && status(1).state == song::kArmed);
  CHECK(status(1).tempo == 93 * 120 && status(0).tempo == 120 * 120);   // (each box's own tempo)
  CHECK(status(1).problems == 0 && status(1).cfg_wrong == -1);          // (no MIDI CONFIG asked of the A4)
  CHECK(strstr(cmd(1, "song=check").c_str(), "no MIDI CONFIG"));
  {   // midi_config() is the Rytm's whatever the slot in hand (core/mod/rytm_out.cpp sends to the Rytm with it)
    device::Scope in(device::g_links[1]);
    CHECK(song::midi_config().track_ch[0] == 1 && song::midi_config().fx_ch == 13);
  }

  // ---- PLAY on both; each its own clock: the Rytm's 4-byte 2C records, the A4's 5-byte ones (a flags byte)
  feed(0, msg(0x2D, with_blk({1, 1})));
  feed(1, msg(0x2D, with_blk({1, 1})));
  CHECK(status(0).state == song::kRec && status(1).state == song::kRec);
  const int P0 = 6000, P1 = 7742;   // frames a step: 120 and 93 BPM
  int64_t next0 = 0, next1 = 0;
  int st0 = 0, st1 = 0;
  bool knob0 = false;
  for (int ms = 0; ms < 4000; ++ms) {
    if (g_frame >= next0 && st0 < 32) {
      feed(0, msg(0x2C, [&] { auto v = blk4(g_frame); v.push_back(1); v.insert(v.end(), {0, (uint8_t)(st0 % 16), 16, 2}); return v; }()));
      next0 += P0, ++st0;
    }
    if (g_frame >= next1 && st1 < 24) {
      feed(1, msg(0x2C, [&] { auto v = blk4(g_frame); v.push_back(1); v.insert(v.end(), {0, (uint8_t)(st1 % 16), 16, 2, 0}); return v; }()));
      next1 += P1, ++st1;
      if (st1 == 3) feed(1, {0xB0, 74, 33});                                 // the A4's track 1: a CC
      if (st1 == 5) feed(1, {0xB1, 99, 2, 0xB1, 98, 9, 0xB1, 6, 90, 0xB1, 38, 0});   // track 2: an NRPN
      if (st1 == 8) feed(1, msg(0x2D, {5, 2, 0}));                          // its mutes: track 2
      if (st1 == 12) feed(1, {0xC9, 5}), feed(1, msg(0x2D, {2, 0, 5}));      // a pattern queued
    }
    if (st0 == 6 && !knob0) knob0 = true, feed(0, {0xB2, 99, 1, 0xB2, 98, 3, 0xB2, 6, 64, 0xB2, 38, 0});   // the Rytm's knob
    step(1);
  }
  song::Status s0 = status(0), s1 = status(1);
  printf("  recording: slot 0 bar %u, %u events; slot 1 bar %u, %u events\n", (unsigned)(s0.t_now / song::kBar + 1),
         (unsigned)s0.events, (unsigned)(s1.t_now / song::kBar + 1), (unsigned)s1.events);
  CHECK(s0.state == song::kRec && s1.state == song::kRec);
  CHECK(s1.t_now >= 20 * 96 && s1.t_now < 25 * 96);   // (the A4's own steps: 24 by now, not the Rytm's 32)
  CHECK(s0.t_now >= 31 * 96);
  CHECK(exists("/.a4/songs/A4P/take-001.jsonl") && exists("/.rytm/songs/RYP/take-001.jsonl"));
  CHECK(!exists("/.rytm/songs/A4P") && !exists("/.a4/songs/RYP"));

  // ---- STOP: each its own file, in its own folder
  feed(1, msg(0x2D, with_blk({1, 0})));
  step(300);
  CHECK(status(1).state == song::kOff && status(0).state == song::kRec);   // (the Rytm still records)
  feed(0, msg(0x2D, with_blk({1, 0})));
  step(300);
  CHECK(status(0).state == song::kOff);
  const std::string a4 = get("/.a4/songs/A4P/take-001.json"), ry = get("/.rytm/songs/RYP/take-001.json");
  printf("  saved: /.a4/songs/A4P/take-001.json %d bytes, /.rytm/songs/RYP/take-001.json %d bytes\n", (int)a4.size(), (int)ry.size());
  CHECK(!a4.empty() && !ry.empty());
  CHECK(!exists("/.a4/songs/A4P/take-001.jsonl") && !exists("/.rytm/songs/RYP/take-001.jsonl"));
  CHECK(a4.find("\"box\":\"a4\"") != std::string::npos && ry.find("\"box\"") == std::string::npos);
  CHECK(a4.find("\"project\":\"A4P\"") != std::string::npos && ry.find("\"project\":\"RYP\"") != std::string::npos);
  CHECK(a4.find("\"tempo\":93") != std::string::npos && ry.find("\"tempo\":120") != std::string::npos);
  CHECK(a4.find("kit_x") == std::string::npos);   // (no stock snapshot of the A4)
  // the A4's events: its CC and NRPN by channel (0x40 | ch: no MIDI CONFIG names its tracks), its mutes, its PC
  printf("  A4 take: %d cc, %d p, %d mute, %d pc, %d play, %d stop\n", count(a4, "\"cc\""), count(a4, "\"p\""),
         count(a4, "\"mute\""), count(a4, "\"pc\""), count(a4, "\"play\""), count(a4, "\"stop\""));
  CHECK(count(a4, "\"cc\"") >= 1 && count(a4, "\"p\"") >= 1 && count(a4, "\"mute\"") >= 1 && count(a4, "\"pc\"") >= 1);
  CHECK(count(ry, "\"p\"") >= 1 && count(ry, "\"mute\"") == 0);   // (the A4's mutes and CC never in the Rytm's)
  {   // the take read back: the A4's box, its events
    song::Take t;
    char err[48];
    CHECK(song::read_json(a4.data(), (int)a4.size(), &t, err, sizeof err));
    CHECK(!strcmp(t.box, "a4") && !strcmp(t.project, "A4P") && t.n == (int)status(1).events);
    bool cc = false, nrpn = false;
    for (int i = 0; i < t.n; ++i) {
      cc |= t.ev[i].type == song::kCc && t.ev[i].a == 1 && t.ev[i].b == 74 && t.ev[i].v == 33;
      nrpn |= t.ev[i].type == song::kParam && t.ev[i].a == (0x40 | 1) && t.ev[i].b == (2 << 7 | 9) && t.ev[i].v == 90 << 7;
    }
    CHECK(cc && nrpn);
    t.free();
  }

  // ---- the A4's replay: refused (its dumps not traced), nothing sent to it
  g_out1.clear();
  cmd(1, "song=replay");
  printf("  slot 1 replay: %s\n", status(1).msg);
  CHECK(status(1).state == song::kOff && strstr(status(1).msg, "no replay"));
  step(50);
  CHECK(g_out1.empty() || [] { for (auto& m : g_out1) if (m[0] == 0xF0 && m[1] == 0) return false; return true; }());

  // ---- each slot's take list its own
  cmd(0, "song=list"), cmd(1, "song=list");
  step(50);
  CHECK(status(0).takes == 1 && status(1).takes == 1);
  put("/.a4/songs/A4P/take-007.json", a4.c_str());
  cmd(1, "song=list");
  step(50);
  CHECK(status(0).takes == 1 && status(1).takes == 2);

  printf("test_song_slots: PASS (each box its own take, clock, tempo and folder; the Rytm's as it was)\n");
  return 0;
}

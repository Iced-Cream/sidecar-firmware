// pi/engine/tests/test_generic_pi.cpp - the generic engine's Pi side (2026-11-04), its portable parts:
// bridge.cpp (the router behind bridge::sysex, 51 STORE's answers, the console's generic commands, /status's members, the popup and the
// address queues from core 0) and audio_return.cpp (the processor slots between Overbridge IN and OUT: nothing added
// while every slot is empty, a slot's output added and saturated, IN frames kept only while a slot is filled).
#include <string.h>
#include <vector>

#include "check.h"
#include "audio_return.h"
#include "bridge.h"
#include "clock/clock.h"
#include "generic/generic.h"
#include "proc/proc.h"
#include "screen/screen.h"

namespace core { void host_fake_time(bool on, uint64_t us); }

static std::vector<std::vector<uint8_t>> g_out;
static void out_fn(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static int sent(uint8_t cmd) {
  int k = 0;
  for (auto& m : g_out) k += m.size() > 4 && m[4] == cmd;
  return k;
}
static void in(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  bridge::sysex(m.data(), (int)m.size());
}
static uint64_t g_us = 1000000;
static void run(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 5) g_us += 5000, core::host_fake_time(true, g_us), bridge::tick();
}

// A processor that adds a fixed level to channel 0 and copies channel 2's input onto channel 3
struct Add : core::proc::Proc {
  float level = 0.25f;
  int params = 0, resets = 0;
  void process(const core::proc::Block& b) override {
    for (int f = 0; f < b.frames; ++f) b.out[0][f] = level, b.out[3][f] = b.in[2][f];
  }
  void set_param(int, int v) override { ++params, level = v / 16383.0f; }
  void link_reset() override { ++resets; }
  const char* name() const override { return "ADD"; }
};

int main() {
  core::host_fake_time(true, g_us);
  bridge::begin(out_fn);   // (no card on the host: the router runs without one)

  // ---- the router behind bridge::sysex: the project's name (51 STORE's 04 NAME), the clock's transport, a dropped
  // command
  in({0x51, 1, 0x04, 0, 0, 2, 0x7D, 0x6F, 3, 'K', 'I', 'T'});
  CHECK(!strcmp(bridge::project(), "KIT"));
  in({0x2D, 1, 1, 1, 0, 0, 0, 5});   // (version 1) transport PLAY at block 5
  CHECK(core::clock::transport() == core::clock::kPlay);
  in({0x01, 0, 0});        // a 0021 voice report: no Pi side in this engine
  char out[400];
  CHECK(bridge::generic_command("gen", out, sizeof out));
  CHECK(strstr(out, "project \"KIT\"") && strstr(out, "dropped 1 (last 01)"));
  // 51 STORE (core/kv, wired by bridge::begin): the companion's HELLO is answered at the next idle_save
  g_out.clear();
  in({0x51, 1, 0x00, 0, 0, 0, 1, 2});
  bridge::idle_save();
  CHECK(sent(0x51) == 1 && g_out.back()[6] == 0x40);
  in({0x51, 1, 0x10, 0, 0, 0, 0, 1});   // a lock set declared (51 10 SET: set 0, track 0, slot 0, param 0, 1 param)
  in({0x51, 1, 0x10, 40, 0, 0, 0, 1});  // refused: set 40 (kSets 32)
  CHECK(bridge::generic_command("kv", out, sizeof out));
  CHECK(strstr(out, "kv: project 00000082, 0 records, hellos 1 ") && strstr(out, "locks: 1 sets, ops 1 bad 1"));
  CHECK(bridge::generic_command("clock", out, sizeof out) && strstr(out, "transport 1"));
  CHECK(!bridge::generic_command("notemach=0:juno", out, sizeof out));

  // ---- /status's members: one JSON object's members, the status page the screen's client, no slot filled
  char js[4096];
  int n = bridge::generic_json(js, sizeof js);
  CHECK(n > 0 && n < (int)sizeof js);
  CHECK(strstr(js, "\"generic\":{\"project\":\"KIT\",\"projectId\":48879,"));
  CHECK(strstr(js, "\"proc\":{\"slots\":[null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null]"));
  CHECK(strstr(js, "\"client\":\"status\""));
  CHECK(strstr(js, ",\"kv\":{\"project\":130,\"records\":0,\"hellos\":1,\"reads\":0,\"writes\":0,"));
  CHECK(strstr(js, ",\"locks\":{\"sets\":1,\"ops\":1,\"bad\":1,\"values\":"));
  CHECK(strstr(js, ",\"queued\":-1,\"bpm10\":"));

  // ---- core 0's popup and address, handed to the screen at the control core's tick
  in({0x35, 1, 0, 80, 1});    // (version 1) the Rytm's hello: up 4 s (50 ms ticks), caps 1 (it understands 36)
  run(4000);               // (the connect splash runs and ends)
  uint32_t shown = core::screen::popups();
  bridge::screen_address("10.0.0.7");
  bridge::rui_popup("SOC 81 C", "WARM");
  run(100);
  CHECK(core::screen::popups() == shown + 1);
  CHECK(sent(0x36) >= 2);  // the splash and the popup
  CHECK(bridge::rytm_greeted() && bridge::rytm_hellos() == 1);

  // ---- the audio return: nothing while every slot is empty
  int32_t I[8][aret::kChannels], O[8][aret::kChannels];
  for (int f = 0; f < 8; ++f)
    for (int c = 0; c < aret::kChannels; ++c) I[f][c] = (c == 2 ? 1 << 30 : 0), O[f][c] = 1000 * c;
  CHECK(!aret::Wanted());
  aret::In(I, 8);
  aret::Fill(O, 8);
  for (int c = 0; c < aret::kChannels; ++c) CHECK(O[0][c] == 1000 * c);   // untouched

  // a slot filled: its output added; IN frames delivered in order
  static Add add;
  CHECK(core::proc::register_slot(1, &add));
  CHECK(aret::Wanted());
  aret::Reset();
  aret::In(I, 8);
  aret::Fill(O, 8);
  CHECK(O[0][0] == (int32_t)(0.25f * 2147483648.0f));
  CHECK(O[0][3] == 3000 + (1 << 30));
  CHECK(O[0][1] == 1000);
  CHECK(aret::InMissing() == 0);
  // saturation: a full-scale add onto a full-scale sample stays at full scale
  for (int f = 0; f < 8; ++f) O[f][0] = 2147483647;
  core::proc::set_param(1, 0, 16383);
  aret::In(I, 8);
  aret::Fill(O, 8);
  CHECK(add.params == 1 && O[0][0] == 2147483647);
  // the OUT fill ahead of the IN frames: zeros used, counted
  aret::Fill(O, 4);
  CHECK(aret::InMissing() == 4);
  // the link reset reaches the slot (the Rytm's USB MIDI gone; the hello's reset, queued before the slot was filled,
  // reached it at its first block)
  int resets = add.resets;
  bridge::rytm_gone();
  aret::In(I, 8);
  aret::Fill(O, 8);
  CHECK(add.resets == resets + 1);
  CHECK(bridge::generic_command("proc", out, sizeof out) && strstr(out, "1:ADD"));
  n = bridge::generic_json(js, sizeof js);
  CHECK(n > 0 && strstr(js, "\"proc\":{\"slots\":[null,\"ADD\",null,null,null,null,null,null,null,null,null,null,null,null,null,null]"));
  core::proc::clear_slot(1);
  CHECK(!aret::Wanted());

  // the route (core/route): 51 ROUTE BD stem (IN 2) -> MAIN L (OUT 0) at unity; answered 60 with point 0; after the ramp
  // OUT 0 carries BD's IN (1 << 30) on top of what was there; UNROUTE fades it out and nothing is kept again
  g_out.clear();
  in({0x51, 0x01, 0x20, 0x05, 0x06, 0x02, 0x00, 0x40, 0x00});
  bridge::idle_save();   // the answer goes out with the store's, at the control core's idle
  CHECK(sent(0x51) == 1);
  for (auto& m : g_out) if (m.size() > 4 && m[4] == 0x51) CHECK(m[6] == 0x60 && m[7] == 0x05 && m[8] == 0x06 && m[9] == 0x00);
  CHECK(aret::Wanted());
  aret::Reset();
  for (int k = 0; k < 40; ++k) {
    for (int f = 0; f < 8; ++f) for (int c = 0; c < aret::kChannels; ++c) O[f][c] = 1000 * c;
    aret::In(I, 8);
    aret::Fill(O, 8);
  }
  CHECK(O[7][0] == (1 << 30));
  for (int c = 1; c < aret::kChannels; ++c) CHECK(O[7][c] == 1000 * c);
  CHECK(bridge::generic_command("route", out, sizeof out) && strstr(out, "1 open") && strstr(out, "0 BD>MAIN L gain 8192 (owner 6)"));
  n = bridge::generic_json(js, sizeof js);
  CHECK(n > 0 && strstr(js, "\"route\":{\"open\":1,") && strstr(js, "\"points\":[[0,6,2,0,8192]]"));
  in({0x51, 0x01, 0x22, 0x06, 0x7F});
  for (int k = 0; k < 40; ++k) aret::In(I, 8), aret::Fill(O, 8);
  CHECK(!aret::Wanted());

  printf("test_generic_pi: ok\n");
  return 0;
}

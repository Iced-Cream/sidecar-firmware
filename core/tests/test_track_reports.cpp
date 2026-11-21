// core/tests/test_track_reports.cpp - the Rytm's 12 Sidecar voices (sim/protocol.py "Track reports", core/device/device.h
// kCmdTracks): the host asks a Rytm for track reports (5B 01 01) at its first report and after each hello; once it says so
// (5B 01 01 back, or a report with the track byte), a shared voice's second track (CP HT OH CB) plays a voice of its own,
// 8..11, beside the first one's, and a note-off with the track byte ends only that track's voice. A classic report after
// that (a Rytm that forgot) turns them off and asks again. Per-voice settings of a pair voice are its V's (box_voice).
// On a slot of 8 voices (-DCORE_VOICES=8) the host never asks: nothing to test.
#include <cmath>
#include <cstring>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/device.h"
#include "engine.h"
#include "host_link.h"
#include "pool/pool.h"
#include "ui/remote.h"
#include "voice_name.h"
#include "voices.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void out0(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void send(const std::vector<uint8_t>& m) { engine_sysex_link(0, m.data(), (int)m.size()); }
static std::vector<uint8_t> sidecar(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xf7);
  return m;
}
// a 0x04 trig on the Rytm's physical voice k with machine m (JUNO: LEN 127, held until its 0x11), the sound tag 0, and
// the track byte when track >= 0
static std::vector<uint8_t> trig(int m, int k, int note, int track) {
  static const uint8_t pl[16] = {0, 100, 0, 0, 0, 64, 64, 127, 64, 64, 0, 64, 64, 0, 0, 100};
  std::vector<uint8_t> t = {0x04, (uint8_t)(m | k), (uint8_t)note, 100};
  for (int i = 0; i < 16; ++i) t.push_back(pl[i]), t.push_back(0);
  t.push_back(0), t.push_back(0);   // the sound tag
  if (track >= 0) t.push_back((uint8_t)track);
  return sidecar(t);
}
static std::vector<uint8_t> note_off(int k, int track) {
  std::vector<uint8_t> b = {0x11, (uint8_t)k};
  if (track >= 0) b.push_back((uint8_t)track);
  return sidecar(b);
}
static int asks() {
  int n = 0;
  for (auto& m : g_out) n += m.size() == 8 && m[4] == device::kCmdTracks && m[5] == 1 && m[6] == 1;
  return n;
}

static constexpr int kRows = kBlock * 400;
static int16_t ring[kRows][kVoices];
static void render(int blocks = 400) {
  uint64_t mach[kMachines] = {0};
  for (int b = 0; b < blocks; ++b) render_voices(ring, kRows, b * kBlock, 0, kVoices, mach);
}
static double late(int vi) {   // the last quarter's rms
  double s = 0;
  for (int i = kRows * 3 / 4; i < kRows; ++i) s += (double)ring[i][vi] * ring[i][vi];
  return std::sqrt(s / (kRows / 4));
}

int main() {
  // the wire helpers first: they hold on every build
  {
    auto t = trig(kJuno, 7, 60, 3);
    CHECK(t.size() == 44 && device::track_byte(t.data(), (int)t.size()) == 3);
    auto c = trig(kJuno, 7, 60, -1);
    CHECK(c.size() == 43 && device::track_byte(c.data(), (int)c.size()) == -1);
    auto o = note_off(7, 3), o7 = note_off(7, -1);
    CHECK(device::track_byte(o.data(), 8) == 3 && device::track_byte(o7.data(), 7) == -1);
    auto bad = trig(kJuno, 7, 60, 12);
    CHECK(device::track_byte(bad.data(), (int)bad.size()) == -1);   // (no track 12)
  }
  if (!device::wants_tracks()) {
    printf("test_track_reports: PASS (8-voice slots: the Rytm's tracks share voices, nothing asked)\n");
    return 0;
  }
  char tmpl[] = "/tmp/core-tracks-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples"));
  hooks.to_rytm = out0;
  card_ok = cards.begin(log, shared, sizeof shared);
  CHECK(card_ok);
  junolib.begin(&cards.sd(), log);
  voices_init();
  device::rytm_on_usb(true);

  // an older Rytm's report: played as ever (physical voice 7, RS/CP's), and the host asks once
  CHECK(!device::g_links[0].tracks && ui::remote_track_voice(3) == 7 && ui::remote_track_voice(2) == 7);
  send(trig(kJuno, 7, 60, -1));
  CHECK(v[7].juno && !v[8].juno && asks() == 1);
  send(trig(kJuno, 7, 62, -1));
  CHECK(asks() == 1);   // once a hello
  printf("  an older Rytm: voice 7 plays, one 5B 01 01 asked\n");

  // the Rytm's answer: CP HT OH CB on voices of their own, their names the tracks', their V the shared voice's
  send(sidecar({device::kCmdTracks, 1, 1}));
  CHECK(device::g_links[0].tracks);
  const int want[12] = {0, 2, 7, 8, 1, 3, 5, 9, 4, 10, 6, 11};
  for (int t = 0; t < 12; ++t) CHECK(ui::remote_track_voice(t) == want[t]);
  CHECK(!strcmp(voice_name(8), "CP") && !strcmp(voice_name(9), "HT") && !strcmp(voice_name(7), "RS/CP"));
  CHECK(box_voice(8) == 7 && box_voice(11) == 6 && device::rytm().voices == 12);

  // MT and HT both held on JUNO: two voices, 5 and 9, both sounding (one Rytm voice, two Sidecar voices)
  send(trig(kJuno, 5, 60, 6));   // MT
  send(trig(kJuno, 5, 67, 7));   // HT takes the Rytm's voice: on the Sidecar its own
  CHECK(v[5].juno && v[9].juno && v[5].live && v[9].live);
  render();
  const double mt = late(5), ht = late(9);
  printf("  MT %.1f and HT %.1f held together (voices 5, 9)\n", mt, ht);
  CHECK(mt > 100 && ht > 100);
  // HT's note-off (its track byte): voice 9 releases, MT's voice 5 holds on
  send(note_off(5, 7));
  render();
  printf("  after HT's 0x11: MT %.1f, HT %.1f\n", late(5), late(9));
  CHECK(late(5) > 0.6 * mt && late(9) < 0.4 * ht);   // (A11's release: a few hundred ms)
  // MT's note-off ends voice 5
  send(note_off(5, 6));
  render();
  printf("  after MT's 0x11: MT %.1f\n", late(5));
  CHECK(late(5) < 0.4 * mt);

  // a classic report while on: the Rytm forgot (its port came up again): back to 8, asked again
  const int a0 = asks();
  send(trig(kJuno, 7, 60, -1));
  CHECK(!device::g_links[0].tracks && asks() == a0 + 1 && ui::remote_track_voice(3) == 7);
  // a track report says it again (an answer lost)
  send(trig(kJuno, 4, 60, 9));   // OH (track 9) on CH/OH's voice 4
  CHECK(device::g_links[0].tracks && v[10].juno && ui::remote_track_voice(9) == 10);
  // a hello (35): asked again, off until the answer
  const int a1 = asks();
  send(sidecar({0x35, 1, 0, 10, 1}));
  CHECK(!device::g_links[0].tracks && asks() == a1 + 1);
  send(sidecar({device::kCmdTracks, 1, 0}));   // a Rytm that says no
  CHECK(!device::g_links[0].tracks && ui::remote_track_voice(9) == 4);
  printf("test_track_reports: PASS\n");
  return 0;
}

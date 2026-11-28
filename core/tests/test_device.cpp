// core/tests/test_device.cpp - the device profiles and the hello (core/device/device.h) in the generic engine: the Rytm
// the default; an unknown hello or another protocol told so; a known box taken at its hello (its output filtered, its
// reports translated into the engine's 0x04 / 0x03 / 0x11 by its table, its other messages dropped); the Rytm's 35
// giving the link back; a box with a state folder of its own answered kRestart and the choice saved before one restart;
// the Digitakt's profile accepted by add().
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/device.h"
#include "device/audio.h"
#include "device/a4.h"
#include "device/digitakt.h"
#include "keys.h"
#include "fmt/fmt.h"
#include "generic/generic.h"
#include "hooks.h"
#include "platform.h"

using namespace core;
using ui::extra_for_box;

static int g_restarts = 0;
static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void send(const std::vector<uint8_t>& m) { gen::sysex(m.data(), (int)m.size()); }
static std::vector<uint8_t> sidecar(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xf7);
  return m;
}
static constexpr uint16_t kBoxId = 0x3F00;   // the range for private tests (docs/device-profiles.md)
// Two tracks, three parameters: P1 fixed 12, P2 from its parameter 2, P3 from 0, P4 fixed 33
static const device::Profile kBox = {
    kBoxId, "Test box", 0x1234, 0x0001,
    4, 2, 3,
    {0x7D, 'T', 'B'},
    0x61, 0x62, 0x63,
    0,
    0x00, 0,   // the voice byte's machine (0: the old engine's PLAITS)
    {-1, 2, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {12, 0, 0, 33, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100},
    {1u << 0x15},   // of the engine's messages it takes only 15
    nullptr};
static std::vector<uint8_t> box(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 'T', 'B'};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xf7);
  return m;
}
static std::vector<uint8_t> hello(int id, int proto = 1) {
  return sidecar({device::kCmdHello, 1, (uint8_t)(id >> 7), (uint8_t)(id & 0x7f), (uint8_t)proto, 'T', 'B'});
}

int main() {
  char tmpl[] = "/tmp/core-device-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  hooks.to_rytm = ::out;
  CHECK(gen::begin());

  // the default: the Rytm, nothing in between
  CHECK(!device::foreign() && device::active().id == device::kIdRytm && hooks.to_rytm == ::out);
  CHECK(device::rytm().tracks == 12 && device::rytm().usb_vid == 0x1935 && device::rytm().usb_pid == 0x0b48);
  CHECK(device::rytm().audio && device::rytm().audio->channels == 12);

  // a hello nobody knows, or of another protocol: told so, the Rytm stays
  send(hello(0x1234));
  CHECK(g_out.size() == 1 && g_out[0][4] == device::kCmdHello && g_out[0].size() == 10 && g_out[0][8] == device::kUnknown);
  CHECK(!device::foreign());
  CHECK(device::add(&kBox) && !device::add(&kBox));   // the id is taken the second time
  g_out.clear();
  send(hello(kBoxId, 9));
  CHECK(g_out.size() == 1 && g_out[0][8] == device::kBadProto && !device::foreign());

  // its hello: answered, and it is the box from here on
  g_out.clear();
  send(hello(kBoxId));
  CHECK(g_out.size() == 1 && g_out[0][6] == kBoxId >> 7 && g_out[0][7] == (kBoxId & 0x7f) && g_out[0][8] == device::kOk);
  CHECK(device::foreign() && device::active().id == kBoxId && hooks.to_rytm != ::out);

  // its trig through the table: the engine's 0x04 on voice 1, P1 fixed, P2 its c, P3 its a, P4 fixed
  {
    std::vector<uint8_t> t = box({0x61, 1, 48, 127, 10, 0, 20, 64, 30, 0});
    const uint8_t* d = t.data();
    int len = (int)t.size();
    uint8_t m[device::kMaxMessage];
    CHECK(device::translate(device::g_link, d, len, m) && d == m && len == 43);
    CHECK(m[4] == 0x04 && m[5] == 1 && m[6] == 48 && m[7] == 127);
    CHECK(m[8] == 12 && m[10] == 30 && m[11] == 0 && m[12] == 10 && m[14] == 33);
    std::vector<uint8_t> off = box({0x63, 1});
    d = off.data(), len = (int)off.size();
    CHECK(device::translate(device::g_link, d, len, m) && len == 7 && m[4] == 0x11 && m[5] == 1);
    std::vector<uint8_t> past = box({0x61, 2, 48, 100, 1, 0, 2, 0, 3, 0});   // a track past its voices
    d = past.data(), len = (int)past.size();
    uint32_t d0 = device::dropped();
    CHECK(!device::translate(device::g_link, d, len, m) && device::dropped() == d0 + 1);
  }
  // the engine's messages to it: only what its profile takes
  g_out.clear();
  const uint8_t juno[8] = {0xf0, 0x7d, 0x52, 0x54, 0x15, 0, 0, 0xf7}, other[8] = {0xf0, 0x7d, 0x52, 0x54, 0x14, 1, 0, 0xf7};
  uint32_t d1 = device::dropped();
  to_rytm(juno, 8);
  to_rytm(other, 8);
  CHECK(g_out.size() == 1 && g_out[0][4] == 0x15 && device::dropped() == d1 + 1);
  // a Rytm plugged in: its hello 35 gives it back; the output is the platform's again
  send(sidecar({0x35, 1, 0, 100, 3}));
  CHECK(!device::foreign() && hooks.to_rytm == ::out && device::active().id == device::kIdRytm);

  // reset() from a box
  send(hello(kBoxId));
  CHECK(device::foreign());
  device::reset();
  CHECK(!device::foreign() && hooks.to_rytm == ::out);

  // a box with a state folder of its own: answered "restart", the choice kept in /.rytm, one restart after the saves
  static const device::Profile kBox2 = [] { device::Profile p = kBox; p.id = kBoxId + 1; p.state_dir = "/.boxtest"; return p; }();
  CHECK(device::add(&kBox2));
  mkdir((std::string(tmpl) + "/.rytm").c_str(), 0755);
  g_out.clear();
  send(hello(kBoxId + 1));   // no restart hook on this board: told it has no such profile, nothing changes
  CHECK(g_out.size() == 1 && g_out[0][8] == device::kUnknown && !device::foreign());
  hooks.device_restart = [] { ++g_restarts; };
  g_out.clear();
  send(hello(kBoxId + 1));
  CHECK(g_out.size() == 1 && g_out[0][8] == device::kRestart && !device::foreign() && !device::g_link.state);
  for (int k = 0; k < 200 && !g_restarts; ++k) gen::idle_save();
  CHECK(g_restarts == 1);
  {
    File f;
    char b[16] = {0};
    CHECK(f.open("/.rytm/device", kRead) && f.read(b, sizeof b - 1) > 0);
    CHECK(atoi(b) == kBoxId + 1);
  }

  // the Digitakt's profile (docs/device-profiles.md): registered by the engine, as the A4's
  CHECK(device::by_id(device::kIdDigitakt) == &device::kDigitakt && !device::add(&device::kDigitakt));
  CHECK(device::by_id(device::kIdA4) == &device::kA4 && device::kA4.audio && device::kA4.audio->in_channels == device::kA4InChannels);
  CHECK(device::kDigitakt.audio && device::kDigitakt.audio->ret == out::kStereoMix);

  // The voices' own-channel trim is the profile's: the A4's insert point sits low, the Rytm and the Digitakt stay at 0, bit for bit
  {
    device::reset();
    CHECK(out::control.trk10 == 0);
    CHECK(device::rytm().trk10 == 0 && device::kDigitakt.trk10 == 0 && device::kA4.trk10 == 180);
    CHECK(device::kA4.audio->loop_frames == 197 && device::kDigitakt.audio->loop_frames == 0);   // LIVE IN's latency, the chop's
    device::g_link.state = "/.a4";                     // the folder chosen at boot: this hello is accepted
    send(hello(device::kA4.id));
    CHECK(device::foreign() && device::active().id == device::kIdA4);
    CHECK(out::control.trk10 == 180);
    out::set_trim(2, 60);                              // a live stereo=trim:trk
    send(hello(device::kA4.id));                       // a repeated hello is no change of box
    CHECK(out::control.trk10 == 60);
    static int s_user = -1;                            // U1..U5: the A4 reports them as the Rytm does, the voice byte's flag
    hooks.user_report = [](int, int s, int, int, int, int) { s_user = s; };
    std::vector<uint8_t> t = sidecar({0x04, 0x50 + 1, 60, 100});
    t.pop_back();
    for (int k = 0; k < 16; ++k) t.push_back(64), t.push_back(0);
    t.push_back(0), t.push_back(0), t.push_back(0xf7);
    usleep((device::kEchoMs + 5) * 1000);              // (60 went to the box on this voice above: past its echo)
    send(t);
    CHECK(s_user == 0);
    hooks.user_report = nullptr;
    device::reset();                                   // back to the Rytm
    CHECK(!device::foreign() && out::control.trk10 == 0);
    {   // the usual way in: the boot after its hello's restart picks the box, and its hello then changes nothing
      Fs f;
      File w;
      CHECK(w.open("/.rytm/device", kWrite));
      CHECK(w.write((const uint8_t*)"3\n", 2) == 2);
      w.close();
      device::boot();
      CHECK(device::active().id == device::kIdA4 && out::control.trk10 == 180);
      send(hello(device::kA4.id));
      CHECK(out::control.trk10 == 180);
      device::reset();
      CHECK(f.remove("/.rytm/device"));
    }
  }

  // A poly track's chord on the A4's voices: one 04 a note, on the voice's machine; a lone trig is what it always was
  {
    auto trig = [](int vb, int note, int vel = 100) {
      std::vector<uint8_t> t = {0x04, (uint8_t)vb, (uint8_t)note, (uint8_t)vel};
      for (int k = 0; k < 2 * 16 + 2; ++k) t.push_back(k % 2 ? 0 : 64);
      return sidecar(t);
    };
    auto off = [](int vi) { return sidecar({0x11, (uint8_t)vi}); };
    auto notes = [](int vi) { return inst::voice_notes(vi) + poly::voice_notes(vi); };
    auto quiet = [&](int vi) { send(off(vi)); voice_silence(vi); usleep(25000); };
    device::g_link.state = "/.a4";
    send(hello(device::kA4.id));
    CHECK(device::kA4.chords && !device::rytm().chords && !device::kDigitakt.chords);

    for (int m : {kPlaits, kJuno, kDaisy}) {   // three notes: the voice's own machine, two beside it, in order
      inst::reset();
      send(trig(m | 1, 60));
      CHECK(notes(1) == 0);                    // a lone trig: no instance, as ever
      send(trig(m | 1, 64));
      send(trig(m | 1, 67));
      CHECK(inst::voice_notes(1) == 2);
      send(trig(m | 1, 64));                   // twice in a chord is once
      CHECK(inst::voice_notes(1) == 2);
      send(off(1));                            // 0x11 lets all of them go
      CHECK(inst::stat(1).sounding <= 2);
      quiet(1);
      CHECK(notes(1) == 0);
    }
    {   // 6-OP FM: its pool, released with the voice
      send(trig(kFm | 2, 60));
      CHECK(poly::voice_notes(2) == 0);
      send(trig(kFm | 2, 64));
      send(trig(kFm | 2, 67));
      send(trig(kFm | 2, 71));
      CHECK(poly::voice_notes(2) == 3 && poly::held_notes(2) == 3);
      send(off(2));
      CHECK(poly::held_notes(2) == 0);
      quiet(2);
      poly::silence(2);
    }
    {   // MD plays no second note: a chord is its first hit
      static int hits;
      auto seen = hooks.trig_seen;
      hooks.trig_seen = [] { ++hits; };
      hits = 0;
      send(trig(kMd | 3, 60));
      send(trig(kMd | 3, 64));
      send(trig(kMd | 3, 67));
      CHECK(hits == 1);                        // one hit: a chord's other notes are no flam
      quiet(3);
      hits = 0;
      send(trig(kMd | 3, 60));
      send(off(3));                            // the mod's mono line: an 11 between, two hits
      send(trig(kMd | 3, 64));
      CHECK(hits == 2);
      quiet(3);
      hits = 0;
      send(trig(kMd | 3, 60));
      usleep((device::kChordMs + 10) * 1000);
      send(trig(kMd | 3, 64));                 // too far apart: two hits
      CHECK(hits == 2);
      hooks.trig_seen = seen;
      quiet(3);
      send(trig(kMd | 3, 60));
      send(trig(kMd | 3, 64));
      CHECK(notes(3) == 0);
      quiet(3);
    }
    {   // too far apart, or an 11 between (the mod's mono track): notes of a line, not a chord
      send(trig(kPlaits | 0, 60));
      usleep((device::kChordMs + 5) * 1000);
      send(trig(kPlaits | 0, 64));
      CHECK(notes(0) == 0);
      usleep((device::kChordMs + 5) * 1000);
      send(trig(kPlaits | 0, 60));
      send(off(0));
      send(trig(kPlaits | 0, 64));
      CHECK(notes(0) == 0);
      send(trig(kPlaits | 0, 60));             // the same note at once: a retrig
      CHECK(notes(0) == 1);                    // (60 and 64: this one a chord's second note)
      quiet(0);
    }
    {   // a new chord with no off, in the old one's tail: the old notes go
      send(trig(kPlaits | 0, 60));
      send(trig(kPlaits | 0, 64));
      usleep((device::kChordMs + 5) * 1000);
      send(trig(kPlaits | 0, 62));
      CHECK(inst::stat(0).sounding <= 1);
      quiet(0);
    }
    {   // a machine change inside the window is no chord
      send(trig(kPlaits | 0, 60));
      send(trig(kJuno | 0, 64));
      CHECK(notes(0) == 0);
      quiet(0);
    }
    {   // the sequencer's chord to the box: each note's echo is dropped, the voice plays it once
      uint32_t before = inst::voice_notes(0);
      static std::vector<std::vector<uint8_t>> sent;
      keys::set_forward([](uint8_t st, uint8_t a, uint8_t b) { sent.push_back({st, a, b}); });
      device::box_note(0, 60, 100);
      device::box_note(0, 64, 100);
      device::box_note(0, 67, 100);
      const uint8_t* d;
      uint8_t buf[device::kMaxMessage];
      for (int n : {60, 64, 67}) {
        auto m = trig(kPlaits | 0, n);
        d = m.data();
        int len = (int)m.size();
        CHECK(!device::translate(device::g_link, d, len, buf));
      }
      auto m = trig(kPlaits | 0, 67);          // once only
      d = m.data();
      int len = (int)m.size();
      CHECK(device::translate(device::g_link, d, len, buf));
      device::box_note(0, -1, 0);
      keys::set_forward(nullptr);
      CHECK(inst::voice_notes(0) == before);
      quiet(0);
    }
    {   // a MIDI channel bound and in use (the A4's note send on 6-OP FM): its notes are the voice's, no second copy from the trigs
      send(trig(kFm | 2, 60));
      poly::midi(0x92, 60, 100);
      send(off(2));
      poly::midi(0x82, 60, 0);
      usleep(40000);
      for (int n = 0; n < 2; ++n) {
        poly::midi(0x92, 60, 100);
        send(trig(kFm | 2, 60));
        poly::midi(0x82, 60, 0);
        usleep(40000);
      }
      CHECK(poly::live_now(2));
      poly::midi(0x92, 60, 100);
      poly::midi(0x92, 64, 100);
      send(trig(kFm | 2, 60));
      send(trig(kFm | 2, 64));
      CHECK(poly::held_notes(2) == 2);
      poly::midi(0x82, 60, 0);
      poly::midi(0x82, 64, 0);
      quiet(2);
      poly::reset();
    }
    {   // what is heard, tonal settings through the real report: each note of the chord on the voice's one channel
      auto tuned = [](int vb, int note) {
        static const uint8_t pl[16] = {93, 64, 0, 0, 127, 0, 64, 127, 64, 64, 0, 64, 64, 0, 0, 100};   // a tonal PLAITS engine, a long DEC, TUN and FM centred, LEV up, GLD 0, VOL 100
        std::vector<uint8_t> t = {0x04, (uint8_t)vb, (uint8_t)note, 100};
        for (int k = 0; k < 16; ++k) t.push_back(pl[k]), t.push_back(0);
        t.push_back(0), t.push_back(0);
        return sidecar(t);
      };
      auto tone = [](int vi, int note) {          // Goertzel, one ring: 0.1 s
        double f = 440 * std::pow(2.0, (note - 69) / 12.0), k = 2 * std::cos(2 * M_PI * f / 48000), a = 0, b = 0;
        for (int i = 0; i < kBlock * 400; ++i) {
          double c = ring[i][vi] / 32768.0 + k * a - b;
          b = a, a = c;
        }
        return a * a + b * b - k * a * b;
      };
      auto heard = [&](double* o) {               // power over 0.8 s: a chord beats, one window says little
        double p[3] = {0, 0, 0};
        for (int w = 0; w < 8; ++w) {
          render();
          p[0] += tone(1, 60), p[1] += tone(1, 64), p[2] += tone(1, 66);
        }
        for (int k = 0; k < 3; ++k) o[k] = 10 * std::log10(p[k] / 8 + 1e-12);
      };
      for (int m : {kPlaits, kJuno}) {
        send(tuned(m | 1, 60));                   // the engine in place
        render();
        double alone[3], other[3], ch[3];
        const int ns[3] = {60, 64, 66};
        for (int k = 0; k < 3; ++k) {             // each note by itself on the voice's machine
          quiet(1);
          render();
          send(tuned(m | 1, ns[k]));
          double p[3];
          heard(p);
          alone[k] = p[k];
          if (k == 0) other[1] = p[1], other[2] = p[2];
        }
        quiet(1);
        render();
        for (int n : ns) send(tuned(m | 1, n));
        CHECK(inst::voice_notes(1) == 2);
        heard(ch);
        printf("  chord, machine %02x: dB on 60 64 66, each alone %.1f %.1f %.1f, 60's own on the others %.1f %.1f; in the chord %.1f %.1f %.1f\n", m, alone[0], alone[1], alone[2], other[1], other[2], ch[0], ch[1], ch[2]);
        for (int k = 1; k < 3; ++k) CHECK(ch[k] > other[k] + 12 && ch[k] > alone[k] - 6);   // there, about 3 dB under alone: the headroom gain
        quiet(1);
        render();
      }
    }
    {   // the Rytm: the same bytes are what they always were, no chord
      device::reset();
      device::g_link.state = nullptr;
      send(trig(kPlaits | 1, 60));
      send(trig(kPlaits | 1, 64));
      send(trig(kPlaits | 1, 67));
      CHECK(notes(1) == 0);
      send(off(1));
      voice_silence(1);
    }
  }

  {  // the A4's screen: the pages' messages alone, their text in the pixels, its hello never the Rytm's 35, the Rytm's pages hidden
    device::reset();
    device::g_link.state = "/.a4";
    send(hello(device::kA4.id));
    CHECK(device::foreign() && device::active().id == device::kIdA4);
    ui::remote_reset();
    g_out.clear();
    send(sidecar({0x36, 1, 0, 100, 1}));
    CHECK(ui::remote_caps() == 1 && device::active().id == device::kIdA4);
    CHECK(!strcmp(voice_name(0), "T1") && !strcmp(voice_name(3), "T4"));
    g_out.clear();
    send(sidecar({0x30, 1, 0, 1, 0}));
    CHECK(ui::remote_open());
    uint32_t ms = 1000;
    ui::remote_tick(ms);
    CHECK(count(0x37) == 1 && count(0x38) >= 8 && count(0x39) == 1);
    for (auto& m : g_out) CHECK(m[4] == 0x37 || m[4] == 0x38 || m[4] == 0x39 || m[4] == 0x3C);
    uint32_t dropped0 = device::dropped();
    int on = 0;
    const uint8_t* fb = ui::remote_frame();
    for (int x = 0; x < 128; ++x) on += __builtin_popcount(fb[7 + 8 * x]);   // the title bar's page
    CHECK(on > 100);
    for (int k = 0; k < 24; ++k) {
      send(sidecar({0x31, 1, 0, 0x41, 1}));
      send(sidecar({0x31, 1, 0, 0x41, 0}));
      ui::remote_tick(ms += 50);
      const char* t = ui::remote_page_title();
      for (const char* no : {"SEQ", "SONG", "POOL", "SAMPLE", "AUTO", "INSERT", "MASTER", "PROJECT", "KEYS", "ARP", "POLY"}) CHECK(!t || strcmp(t, no));
      CHECK(!t || extra_for_box(t));
    }
    CHECK(device::dropped() == dropped0);          // nothing it asked for was filtered
    send(sidecar({0x30, 1, 0, 0, 3}));
    CHECK(!ui::remote_open());
    ui::remote_reset();
    device::reset();
    CHECK(device::box_screen() == nullptr && !strcmp(voice_name(0), "BD"));
    CHECK(extra_for_box("SEQ"));
  }

  {  // the Digitakt's screen and slots (mod 0.3): the pages as the A4's, eight tracks T1..T8; a slot's 19 answered with its 1A, a 1D let
     // through; the Rytm's other messages still filtered
    device::reset();
    device::g_link.state = "/.digitakt";
    send(hello(device::kIdDigitakt));
    CHECK(device::foreign() && device::active().id == device::kIdDigitakt);
    CHECK((device::kDigitakt.features & (device::kRemoteUi | device::kUserSlots)) == (device::kRemoteUi | device::kUserSlots));
    CHECK(!(device::kDigitakt.features & (device::kProject | device::kKits | device::kSendFx | device::kPools | device::kRytmKeys)));
    CHECK(!strcmp(device::kDigitakt.screen->keys, "UP + YES: PAGES"));
    CHECK(device::box_screen() == device::kDigitakt.screen && !strcmp(voice_name(0), "T1") && !strcmp(voice_name(7), "T8"));
    g_out.clear();
    send(sidecar({0x19, 1, 2, 0, 0, 1}));            // ASK META, slot 2, generation 0
    CHECK(count(0x1A) >= 1);
    for (auto& m : g_out) CHECK(m[4] == 0x1A);
    g_out.clear();
    std::vector<uint8_t> slots = sidecar({0x1D, 1, 1, 1, 1, 1, 1});
    to_rytm(slots.data(), (int)slots.size());
    CHECK(count(0x1D) == 1);
    uint32_t d0 = device::dropped();
    g_out.clear();
    for (const std::vector<uint8_t>& m : {sidecar({0x3D, 1, 0}), sidecar({0x14, 0}), sidecar({0x1B, 1, 0, 0x21, 1, 1, 0})}) to_rytm(m.data(), (int)m.size());
    CHECK(g_out.empty() && device::dropped() == d0 + 3);   // the Rytm's text (drawn into the pixels for a box), a pool list, the installed list
    ui::remote_reset();
    send(sidecar({0x36, 1, 0, 100, 1}));
    CHECK(ui::remote_caps() == 1 && device::active().id == device::kIdDigitakt);
    g_out.clear();
    send(sidecar({0x30, 1, 0, 1, 0}));
    CHECK(ui::remote_open());
    uint32_t ms = 1000;
    ui::remote_tick(ms);
    CHECK(count(0x37) == 1 && count(0x38) >= 8 && count(0x39) == 1);
    for (auto& m : g_out) CHECK(m[4] == 0x37 || m[4] == 0x38 || m[4] == 0x39 || m[4] == 0x3C);
    uint32_t dropped0 = device::dropped();
    for (int k = 0; k < 24; ++k) {
      send(sidecar({0x31, 1, 0, 0x41, 1}));
      send(sidecar({0x31, 1, 0, 0x41, 0}));
      ui::remote_tick(ms += 50);
      const char* t = ui::remote_page_title();
      for (const char* no : {"SEQ", "SONG", "POOL", "SAMPLE", "AUTO", "INSERT", "MASTER", "PROJECT", "KEYS", "ARP", "POLY"}) CHECK(!t || strcmp(t, no));
      CHECK(!t || extra_for_box(t));
    }
    CHECK(device::dropped() == dropped0);
    send(sidecar({0x30, 1, 0, 0, 3}));
    CHECK(!ui::remote_open());
    ui::remote_reset();
    device::reset();
    CHECK(device::box_screen() == nullptr && !strcmp(voice_name(0), "BD"));
  }

  printf("test_device: ok\n");
  return 0;
}

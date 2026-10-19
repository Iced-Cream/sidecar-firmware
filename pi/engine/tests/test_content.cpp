// pi/engine/tests/test_content.cpp - Task 6's self-test content on a folder standing in for the Pi's card (the core's
// Mac port): bridge::selftest_content writes /Samples/pi-test/sine440.wav and /.rytm/pools/_default.txt, touches
// nothing that exists, then the core loads the pool (slot 1 resolved) and DSP SAMPLE on note 48 plays the sample an
// octave down (220 Hz: pi_run.py's EXPECT). Built against the core (make -C core engines first): Makefile's content.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../bridge.h"
#include "notes.h"
#include "check.h"
#include "voices.h"

static std::string slurp(const char* p) {
  FILE* f = fopen(core::host_path(p).c_str(), "rb");
  if (!f) return "";
  std::string s;
  char b[4096];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
  fclose(f);
  return s;
}
static uint32_t le32(const std::string& s, size_t o) {
  return (uint8_t)s[o] | (uint8_t)s[o + 1] << 8 | (uint8_t)s[o + 2] << 16 | (uint32_t)(uint8_t)s[o + 3] << 24;
}
static void noop(const uint8_t*, int) {}

int main() {
  char tmpl[] = "/tmp/pi-content-XXXXXX";
  CHECK(mkdtemp(tmpl));
  core::host_set_card(tmpl);

  // A fresh card: both files written.
  CHECK(bridge::selftest_content());
  std::string wav = slurp("/Samples/pi-test/sine440.wav");
  CHECK(wav.size() == 44 + 96000 * 2);
  CHECK(!memcmp(wav.data(), "RIFF", 4) && le32(wav, 4) == 36 + 96000 * 2 && !memcmp(wav.data() + 8, "WAVEfmt ", 8));
  CHECK(le32(wav, 16) == 16 && le32(wav, 24) == 48000 && le32(wav, 28) == 96000 && !memcmp(wav.data() + 36, "data", 4));
  CHECK(le32(wav, 40) == 96000 * 2);
  const int16_t* pcm = (const int16_t*)(wav.data() + 44);
  int peak = 0, cross = 0;
  for (int i = 0; i < 96000; ++i) {
    peak = std::max(peak, std::abs((int)pcm[i]));
    if (i && pcm[i - 1] < 0 && pcm[i] >= 0) ++cross;
  }
  CHECK(peak > 11900 && peak <= 12000 && cross >= 879 && cross <= 881);   // 440 Hz for 2 s
  CHECK(slurp("/.rytm/pools/_default.txt") == "pi-test/sine440.wav\n");

  // Nothing that exists is touched: a changed pool and WAV stay as they are.
  FILE* f = fopen(core::host_path("/.rytm/pools/_default.txt").c_str(), "wb");
  fputs("pi-test/sine440.wav\nother.wav\n", f);
  fclose(f);
  CHECK(truncate(core::host_path("/Samples/pi-test/sine440.wav").c_str(), 44 + 96000 * 2 - 2) == 0);
  CHECK(bridge::selftest_content());
  CHECK(slurp("/.rytm/pools/_default.txt") == "pi-test/sine440.wav\nother.wav\n");
  CHECK(slurp("/Samples/pi-test/sine440.wav").size() == 44 + 96000 * 2 - 2);
  // A project named on the card: its pool is the project's, no _default written.
  char tmpl2[] = "/tmp/pi-content-XXXXXX";
  CHECK(mkdtemp(tmpl2));
  core::host_set_card(tmpl2);
  CHECK(mkdir(core::host_path("/.rytm").c_str(), 0755) == 0);
  f = fopen(core::host_path("/.rytm/project").c_str(), "wb");
  fputs("PROJ", f);
  fclose(f);
  CHECK(bridge::selftest_content());
  CHECK(slurp("/Samples/pi-test/sine440.wav").size() == 44 + 96000 * 2);
  struct stat st;
  CHECK(stat(core::host_path("/.rytm/pools/_default.txt").c_str(), &st) != 0);

  // A write cut short by a power loss: the leftover .tmp files go at the next boot, the files are written whole.
  char tmpl4[] = "/tmp/pi-content-XXXXXX";
  CHECK(mkdtemp(tmpl4));
  core::host_set_card(tmpl4);
  CHECK(mkdir(core::host_path("/Samples").c_str(), 0755) == 0 && mkdir(core::host_path("/Samples/pi-test").c_str(), 0755) == 0);
  CHECK(mkdir(core::host_path("/.rytm").c_str(), 0755) == 0 && mkdir(core::host_path("/.rytm/pools").c_str(), 0755) == 0);
  f = fopen(core::host_path("/Samples/pi-test/sine440.wav.tmp").c_str(), "wb");
  fputs("RIFF", f);
  fclose(f);
  f = fopen(core::host_path("/.rytm/pools/_default.txt.tmp").c_str(), "wb");
  fputs("pi-te", f);
  fclose(f);
  CHECK(bridge::selftest_content());
  CHECK(slurp("/Samples/pi-test/sine440.wav") == wav && slurp("/.rytm/pools/_default.txt") == "pi-test/sine440.wav\n");
  CHECK(stat(core::host_path("/Samples/pi-test/sine440.wav.tmp").c_str(), &st) != 0);
  CHECK(stat(core::host_path("/.rytm/pools/_default.txt.tmp").c_str(), &st) != 0);
  // A write that fails (the .tmp cannot be opened: a folder in its place): false, no file left behind.
  char tmpl5[] = "/tmp/pi-content-XXXXXX";
  CHECK(mkdtemp(tmpl5));
  core::host_set_card(tmpl5);
  CHECK(mkdir(core::host_path("/Samples").c_str(), 0755) == 0 && mkdir(core::host_path("/Samples/pi-test").c_str(), 0755) == 0);
  CHECK(mkdir(core::host_path("/Samples/pi-test/sine440.wav.tmp").c_str(), 0755) == 0);
  CHECK(mkdir(core::host_path("/Samples/pi-test/sine440.wav.tmp/x").c_str(), 0755) == 0);   // not removable
  CHECK(!bridge::selftest_content());
  CHECK(stat(core::host_path("/Samples/pi-test/sine440.wav").c_str(), &st) != 0);

  // The fresh card through the core: slot 1 resolved, DSP SAMPLE on note 48 (channel 1) plays 220 Hz.
  char tmpl3[] = "/tmp/pi-content-XXXXXX";
  CHECK(mkdtemp(tmpl3));
  core::host_set_card(tmpl3);
  CHECK(bridge::selftest_content());
  CHECK(bridge::begin(noop));
  CHECK(bridge::pool_slots() == 1 && core::pool[0].folder >= 0 && !strcmp(core::pool[0].path, "pi-test/sine440.wav"));
  core::NoteAdapter notes(bridge::sysex);
  CHECK(notes.SetMachine(0, "dsps"));
  notes.Channel(0x90, 48, 100);
  CHECK(!strcmp(bridge::machine_name(0), "DSPS"));
  int16_t block[bridge::kBlock][bridge::kVoices];
  uint64_t mach[bridge::kMachines] = {};
  int n = 0, rises = 0, first = -1, last = -1, pk = 0;
  int16_t prev = 0;
  for (int b = 0; b < 48000 / bridge::kBlock; ++b) {   // 1 s
    while (bridge::idle_prepare()) {}
    bridge::idle_stream();
    bridge::render(block, 0, bridge::kVoices, mach);
    for (int s = 0; s < bridge::kBlock; ++s, ++n) {
      int16_t x = block[s][0];
      pk = std::max(pk, std::abs((int)x));
      if (n > 4800 && prev < -64 && x >= 0) {   // after 100 ms; a small hysteresis
        if (first < 0) first = n;
        last = n;
        ++rises;
      }
      if (x < -64 || x >= 0) prev = x;
    }
  }
  CHECK(pk > 1000);
  CHECK(rises > 10);
  double hz = (rises - 1) * 48000.0 / (last - first);
  printf("test_content: DSP SAMPLE note 48 on the pool's sine440.wav: %.1f Hz, peak %d\n", hz, pk);
  CHECK(std::fabs(hz - 220.0) < 220.0 * 0.03);

  // Task 7's boot bench on the same card (the Mac's clock: ns): every machine renders, the longest block within the
  // total, and the voices silent again afterwards (voices_init).
  for (const char* m : {"plaits", "juno", "fm", "daisy", "dsps", "dd"}) {
    uint32_t worst = 0;
    int pk = 0;
    uint64_t t = bridge::bench(m, &worst, &pk);
    printf("test_content: bench %s: %.2f %% of a Mac core per voice, worst block %.1f %%, peak %d\n", m,
           t * 100.0 / 1e9, worst * 100.0 / (1e9 * bridge::kBlock / 48000), pk);
    CHECK(t > 0 && worst > 0 && worst <= t);
    CHECK(pk > 1000);   // the machine sounds
  }
  for (int vi = 0; vi < bridge::kVoices; ++vi) CHECK(!strcmp(bridge::machine_name(vi), "PLAITS"));   // not left on DD
  int after = 0;
  for (int b = 0; b < 400; ++b) {
    bridge::render(block, 0, bridge::kVoices, mach);
    for (int s = 0; s < bridge::kBlock; ++s)
      for (int v = 0; v < bridge::kVoices; ++v) after = std::max(after, std::abs((int)block[s][v]));
  }
  CHECK(after == 0);

  // Every machine sounds through the note adapter as the self-test plays it (voice 0, channel 1, note 48: a note-off
  // and a note-on back to back every 0.5 s, no block between them), switching machines as pi_run.py does (plaits
  // juno fm daisy dsps dd); the 4th note (1.5..2 s) must sound too: each note-on retriggers, not only the first.
  for (const char* m : {"plaits", "juno", "fm", "daisy", "dsps", "dd"}) {
    CHECK(notes.SetMachine(0, m));
    int peak = 0, last = 0;
    const int half = 24000 / bridge::kBlock;
    for (int b = 0; b < 4 * half; ++b) {   // 2 s
      if (b % half == 0) {
        notes.Channel(0x80, 48, 0);
        notes.Channel(0x90, 48, 100);
      }
      while (bridge::idle_prepare()) {}
      bridge::idle_stream();
      bridge::render(block, 0, bridge::kVoices, mach);
      for (int s = 0; s < bridge::kBlock; ++s) {
        int a = std::abs((int)block[s][0]);
        peak = std::max(peak, a);
        if (b >= 3 * half) last = std::max(last, a);
      }
    }
    printf("test_content: %s on notes: peak %d, 4th note %d\n", m, peak, last);
    CHECK(peak > 1000 && last > 1000);
  }
  printf("test_content: PASS\n");
  return 0;
}

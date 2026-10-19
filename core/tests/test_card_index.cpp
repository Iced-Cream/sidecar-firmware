// core/tests/test_card_index.cpp - the card index (core/pool/card_index.h) on a folder standing in for the card:
// folders breadth first, files in name order with their WAV data, float WAVs and hidden files left out, the
// stored index reused while the card is unchanged and rebuilt when it changes.
#include <cstring>
#include <string>
#include <unistd.h>   // mkdtemp

#include "check.h"
#include "pool/card_index.h"
#include "wav.h"

static std::string g_log;   // every line since the last clear
static void sink(const char* l, int n) { g_log.append(l, n).append("\n"); }

int main() {
  char tmpl[] = "/tmp/core-idx-XXXXXX";
  CHECK(mkdtemp(tmpl));
  core::host_set_card(tmpl);
  core::host_set_log_sink(sink);
  core::Fs fs;
  CHECK(fs.begin());
  fs.mkdir("/Samples");
  fs.mkdir("/Samples/Drums");
  fs.mkdir("/Samples/Drums/Hits");
  fs.mkdir("/Samples/Bass");
  fs.mkdir("/Samples/Empty");
  auto ramp = [](int i, int) { return (int32_t)(i * 7); };
  write_wav("/Samples/Drums/snare.wav", 100, 1, 16, 44100, ramp);
  write_wav("/Samples/Drums/kick.wav", 50, 2, 24, 48000, ramp);
  write_wav("/Samples/Drums/float.wav", 10, 1, 16, 48000, ramp, 3);
  write_wav("/Samples/Drums/._kick.wav", 10, 1, 16, 48000, ramp);
  write_wav("/Samples/Drums/Hits/hat.wav", 20, 1, 16, 48000, ramp);
  write_wav("/Samples/Bass/sub.wav", 30, 1, 16, 48000, ramp);
  static uint8_t scratch[64 * 1024];
  {
    card::Index idx;
    CHECK(idx.begin(core::log, scratch, sizeof scratch));
    CHECK(idx.folders() == 5);   // /Samples, then Bass Drums Empty (name order here), then Hits
    CHECK(strcmp(idx.folder(0).name, "") == 0 && idx.folder(0).nfiles == 0);
    int drums = -1;
    for (int f = 1; f < idx.folders(); ++f) if (!strcmp(idx.folder(f).name, "Drums")) drums = f;
    CHECK(drums > 0 && idx.folder(drums).nfiles == 2 && idx.folder(drums).nsub == 1);
    uint16_t sub[8];
    CHECK(idx.subfolders(0, sub, 8) == 2);   // Empty has no .wav below: not listed
    CHECK(!strcmp(idx.folder(sub[0]).name, "Bass") && !strcmp(idx.folder(sub[1]).name, "Drums"));
    card::FileRec r;
    CHECK(idx.file(drums, 0, &r) && !strcmp(r.name, "kick.wav") && r.channels == 2 && r.bits == 24);
    CHECK(r.rate == 48000 && r.data_offset == 44 && r.data_len == 50 * 2 * 3);
    CHECK(idx.file(drums, 1, &r) && !strcmp(r.name, "snare.wav") && r.rate == 44100 && r.data_len == 200);
    core::File f;
    CHECK(idx.open(drums, r, &f));
    uint8_t b[4];
    CHECK(f.seekSet(r.data_offset + 2) && f.read(b, 2) == 2 && b[0] == 7 && b[1] == 0);   // sample 1 = 7
    CHECK(!idx.file(drums, 2, &r));
  }
  {   // unchanged card: the stored index is used
    card::Index idx;
    g_log.clear();
    CHECK(idx.begin(core::log, scratch, sizeof scratch));
    CHECK(g_log.find("index: unchanged, loaded") != std::string::npos);
    CHECK(idx.folders() == 5);
  }
  write_wav("/Samples/Bass/sub2.wav", 30, 1, 16, 48000, ramp);
  {   // a new file: rebuilt
    card::Index idx;
    g_log.clear();
    CHECK(idx.begin(core::log, scratch, sizeof scratch));
    CHECK(g_log.find("index: card changed") != std::string::npos);
    int bass = -1;
    for (int f = 1; f < idx.folders(); ++f) if (!strcmp(idx.folder(f).name, "Bass")) bass = f;
    CHECK(bass > 0 && idx.folder(bass).nfiles == 2);
  }
  printf("test_card_index: PASS\n");
  return 0;
}

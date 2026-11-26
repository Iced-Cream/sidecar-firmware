// core/tests/test_library_depth.cpp - the library's walk on a card with folders nested deeper than lib's kDepth (12):
// the walk must not index its level table past its end (an out-of-bounds close at the deepest level, caught by
// UBSan/ASan builds), must index every sample down to the deepest walked folder and must skip what lies deeper (the
// walk counts the first too-deep folder of each chain, Stats::deep_skipped, and logs the count once).
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "check.h"
#include "lib/library.h"
#include "platform.h"

using namespace core;

static std::string g_card;
static uint64_t g_us = 1000000;
static void rules(lib::IoRule* r) { *r = {lib::kPiece, lib::kPiece, 0, ""}; }
static bool no() { return false; }
static int no_refs(const char**, int) { return 0; }
static std::string wav(int seed) {   // a small WAV-looking file, a different one per seed
  std::string s(1000, '\0');
  for (size_t i = 0; i < s.size(); ++i) s[i] = (char)(seed * 31 + (int)i * 7);
  memcpy(&s[0], "RIFF", 4), memcpy(&s[8], "WAVEfmt ", 8);
  uint32_t sz = 16, rate = 48000;
  uint16_t pcm = 1, ch = 1;
  memcpy(&s[16], &sz, 4), memcpy(&s[20], &pcm, 2), memcpy(&s[22], &ch, 2), memcpy(&s[24], &rate, 4);
  return s;
}
static void put(const std::string& p, const std::string& s) {
  std::string cmd = "mkdir -p \"$(dirname '" + g_card + p + "')\"";
  CHECK(system(cmd.c_str()) == 0);
  FILE* f = fopen((g_card + p).c_str(), "wb");
  CHECK(f);
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}
int main() {
  char tmpl[] = "/tmp/lib-depth-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  host_set_card(g_card.c_str());
  host_fake_time(true, g_us);
  // Samples/d1/.../d15, a sample in every folder
  std::string dir = "/Samples";
  put(dir + "/top.wav", wav(0));
  const int kLevels = 15;
  for (int i = 1; i <= kLevels; ++i) {
    dir += "/d" + std::to_string(i);
    put(dir + "/s" + std::to_string(i) + ".wav", wav(i));
  }
  lib::reset_for_tests();
  lib::Env e{};
  e.rules = rules, e.card_busy = no, e.other_job = no, e.current_refs = no_refs;
  lib::set_env(e);
  lib::begin(true);
  int n = 0;
  for (; n < 200000; ++n) {
    g_us += 2500;
    host_fake_time(true, g_us);
    if (!lib::step() && !lib::busy()) break;
  }
  CHECK(n < 200000);
  lib::Stats st = lib::stats();
  CHECK(st.state == lib::kReady);
  // the 11 and 12 here and below are lib's kDepth (12, private to library.cpp): a change of it fails this test loudly
  // top.wav + the samples of d1..d11 (Samples is the 1st folder down, d11 the 12th: the deepest walked)
  CHECK(st.files == 1 + 11);
  CHECK(st.deep_skipped == 1);   // d12 is the one chain's first too-deep folder (its d13.. are never listed)
  std::string path = "/Samples";
  for (int i = 1; i <= kLevels; ++i) {
    path += "/d" + std::to_string(i);
    lib::Ref r;
    bool known = lib::ref_for_path((path + "/s" + std::to_string(i) + ".wav").c_str(), &r) && r.has_sha;
    CHECK(known == (i <= 11));   // indexed (hashed) down to the deepest walked folder, not below
  }
  CHECK(system(("rm -rf '" + g_card + "'").c_str()) == 0);
  printf("test_library_depth: ok\n");
  return 0;
}

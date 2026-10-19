// core/tests/test_sample_ram.cpp - the sample RAM (core/pool/sample_ram_pool.h): off without memory from the
// board; with 16 MB, a wanted sample loads in steps and reads back as int16 mono at its rate, pinned ones stay.
#include <cstdlib>
#include <cstring>
#include <unistd.h>   // mkdtemp

#include "check.h"
#include "pool/sample_ram_pool.h"
#include "wav.h"

int main() {
  char tmpl[] = "/tmp/core-sram-XXXXXX";
  CHECK(mkdtemp(tmpl));
  core::host_set_card(tmpl);
  core::Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples/A"));
  write_wav("/Samples/A/ramp.wav", 30000, 1, 16, 44100, [](int i, int) { return (int32_t)((i * 3) % 30000 - 15000); });
  static uint8_t scratch[64 * 1024];
  static card::Index idx;
  CHECK(idx.begin(core::log, scratch, sizeof scratch));
  unsetenv("CORE_SAMPLE_RAM_MB");
  CHECK(rytm::SampleRamPool::create(&idx, core::log) == nullptr);   // no memory from the board: off
  setenv("CORE_SAMPLE_RAM_MB", "16", 1);
  rytm::SampleRamPool* p = rytm::SampleRamPool::create(&idx, core::log);
  CHECK(p && p->ok() && p->arena_bytes() > (8u << 20) && p->pluck_line(0) != nullptr);
  int f = 1;   // /Samples/A
  const int16_t* pcm;
  uint32_t frames;
  float rate;
  bool cut;
  CHECK(!p->get(f, 0, &pcm, &frames, &rate, &cut));
  p->want(f, 0, false);
  int steps = 0;
  while (p->step()) ++steps;
  CHECK(steps >= 2);   // an open, then 8 KB reads
  CHECK(p->get(f, 0, &pcm, &frames, &rate, &cut) && frames == 30000 && rate == 44100.0f && !cut);
  for (int i = 0; i < 30000; i += 997) CHECK(pcm[i] == (int16_t)((i * 3) % 30000 - 15000));
  CHECK(p->loads() == 1 && p->used_bytes() >= 60000);
  p->pin(f, 0);
  p->unpin(f, 0);
  printf("test_sample_ram: PASS\n");
  return 0;
}

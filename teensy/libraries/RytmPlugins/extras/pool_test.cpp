// extras/pool_test.cpp - the per-voice pool reader (head random access, ring streamed, seek), on the Mac.
// build: c++ -std=c++17 -O1 -Wall -I../src pool_test.cpp ../src/rmp_pool.cpp -o /tmp/pool_test && /tmp/pool_test
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "rmp_pool.h"

static std::vector<uint8_t> file;   // a WAV-like file: 44 header bytes, then PCM
static int reads; static bool fail;
static bool rd(void *, uint32_t off, uint8_t *buf, uint32_t n) {
  if (fail || (uint64_t)off + n > file.size()) return false;
  ++reads; memcpy(buf, file.data() + off, n); return true;
}
static int16_t s16(uint32_t i) { return (int16_t)((int)(i % 1000) * 60 - 30000); }
static uint8_t head[8192], ring[16384];

int main() {
  const uint32_t frames = 20000;
  file.assign(44, 0);
  for (uint32_t i = 0; i < frames; ++i) { int16_t v = s16(i); file.push_back((uint8_t)v); file.push_back((uint8_t)(v >> 8)); }
  rmp_pool_src src = {44, frames * 2, 44100.0f, 1, 16};
  rmp_pool_reader r;
  rmp_pool_attach(&r, head, sizeof head, ring, sizeof ring, 8192);
  rmp_pool_info inf;
  // not prepared: asked for, the prepare queued
  CHECK(rmp_pool_info_get(&r, 7, &inf) == RMP_POOL_NOT_READY && r.want == 7);
  CHECK(rmp_pool_info_get(&r, 0, &inf) == RMP_POOL_EMPTY && rmp_pool_info_get(&r, 128, &inf) == RMP_POOL_EMPTY);
  float b[64];
  CHECK(rmp_pool_read(&r, 7, 0, b, 12) == 0);
  // the job prepares it: one read, the head
  reads = 0;
  CHECK(rmp_pool_prepare(&r, 7, &src, rd, nullptr) && reads == 1 && r.want == 0);
  CHECK(rmp_pool_info_get(&r, 7, &inf) == RMP_POOL_OK);
  CHECK(inf.frames == frames && inf.channels == 1 && inf.rate == 44100.0f && inf.where == RMP_POOL_HEAD && inf.head_frames == 4096);
  // random access inside the head, in any order
  CHECK(rmp_pool_read(&r, 7, 4000, b, 12) == 12 && fabsf(b[0] - s16(4000) / 32768.0f) < 1e-6f);
  CHECK(rmp_pool_read(&r, 7, 100, b, 4) == 4 && fabsf(b[3] - s16(103) / 32768.0f) < 1e-6f);
  // past the head: streamed - nothing until the ring has it, no seek
  CHECK(rmp_pool_read(&r, 7, 4096, b, 12) == 0 && r.seek == RMP_POOL_NONE);
  CHECK(rmp_pool_read(&r, 7, 4090, b, 12) == 6);   // the head's last 6, then the wait
  CHECK(rmp_pool_refill(&r) == 8192 && rmp_pool_read(&r, 7, 4096, b, 12) == 12 && fabsf(b[11] - s16(4107) / 32768.0f) < 1e-6f);
  // the whole sample in order, 12 frames a call, a refill whenever less than 4096 frames are ahead: complete, exact
  rmp_pool_clear(&r);
  CHECK(rmp_pool_prepare(&r, 7, &src, rd, nullptr));
  uint32_t pos = 0, missed = 0;
  while (pos < frames) {
    if (rmp_pool_ahead(&r) < 4096) rmp_pool_refill(&r);
    int32_t got = rmp_pool_read(&r, 7, pos, b, 12);
    if (got == 0) { ++missed; rmp_pool_refill(&r); continue; }
    for (int32_t i = 0; i < got; ++i) CHECK(fabsf(b[i] - s16(pos + i) / 32768.0f) < 1e-6f);
    pos += (uint32_t)got;
  }
  CHECK(missed == 0);
  // a jump outside head and ring: silence, the stream restarts there at the next refill
  rmp_pool_clear(&r);
  CHECK(rmp_pool_prepare(&r, 7, &src, rd, nullptr));
  CHECK(rmp_pool_read(&r, 7, 15000, b, 12) == 0 && r.seek == 15000);
  CHECK(rmp_pool_ahead(&r) == 0);   // a pending seek is the most urgent refill
  CHECK(rmp_pool_refill(&r) > 0 && r.seek == RMP_POOL_NONE);
  CHECK(rmp_pool_read(&r, 7, 15000, b, 12) == 12 && fabsf(b[0] - s16(15000) / 32768.0f) < 1e-6f);
  CHECK(rmp_pool_read(&r, 7, 100, b, 1) == 1);   // the head is still there
  // the end: fewer frames
  CHECK(rmp_pool_read(&r, 7, frames - 3, b, 12) <= 3);
  // another slot: nothing, its prepare asked for
  CHECK(rmp_pool_read(&r, 9, 0, b, 12) == 0 && r.want == 9);
  // 24-bit stereo, 3 channels stored: the first two delivered, interleaved
  file.assign(44, 0);
  for (uint32_t i = 0; i < 1000; ++i)
    for (int c = 0; c < 3; ++c) { int32_t v = (int32_t)(i * 1000) * (c ? -1 : 1); file.push_back((uint8_t)v); file.push_back((uint8_t)(v >> 8)); file.push_back((uint8_t)(v >> 16)); }
  rmp_pool_src s3 = {44, 1000 * 9, 48000.0f, 3, 24};
  rmp_pool_clear(&r);
  CHECK(rmp_pool_prepare(&r, 2, &s3, rd, nullptr) && rmp_pool_info_get(&r, 2, &inf) == RMP_POOL_OK && inf.channels == 2);
  CHECK(inf.head_frames == 8192 / 9);
  CHECK(rmp_pool_read(&r, 2, 10, b, 2) == 2);
  CHECK(fabsf(b[0] - 10000 / 8388608.0f) < 1e-6f && fabsf(b[1] + 10000 / 8388608.0f) < 1e-6f && fabsf(b[2] - 11000 / 8388608.0f) < 1e-6f);
  // a sample that cannot be read: EMPTY from then on (no prepare loop)
  rmp_pool_clear(&r);
  fail = true;
  CHECK(rmp_pool_info_get(&r, 4, &inf) == RMP_POOL_NOT_READY && !rmp_pool_prepare(&r, 4, &src, rd, nullptr));
  CHECK(rmp_pool_info_get(&r, 4, &inf) == RMP_POOL_EMPTY && r.want == 0);
  fail = false;
  rmp_pool_src bad = {44, 100, 48000.0f, 1, 12};   // 12-bit: not a format
  CHECK(!rmp_pool_prepare(&r, 5, &bad, rd, nullptr) && rmp_pool_info_get(&r, 5, &inf) == RMP_POOL_EMPTY);
  // a sample whole in RAM (PSRAM): decoded straight, any position
  file.assign(44, 0);
  for (uint32_t i = 0; i < frames; ++i) { int16_t v = s16(i); file.push_back((uint8_t)v); file.push_back((uint8_t)(v >> 8)); }
  CHECK(rmp_pool_decode(file.data() + 44, frames, 1, 16, 19990, b, 20) == 10 && fabsf(b[9] - s16(19999) / 32768.0f) < 1e-6f);
  CHECK(rmp_pool_decode(file.data() + 44, frames, 1, 16, 7, b, 1) == 1 && fabsf(b[0] - s16(7) / 32768.0f) < 1e-6f);
  // 32-bit integer PCM, stereo: streamed and whole
  file.assign(44, 0);
  for (uint32_t i = 0; i < 3000; ++i)
    for (int c = 0; c < 2; ++c) { uint32_t v = (uint32_t)((int32_t)i * 700000 * (c ? -1 : 1)); for (int k = 0; k < 4; ++k) file.push_back((uint8_t)(v >> (8 * k))); }
  rmp_pool_src s32 = {44, 3000 * 8, 48000.0f, 2, 32};
  CHECK(rmp_pool_prepare(&r, 3, &s32, rd, nullptr) && rmp_pool_info_get(&r, 3, &inf) == RMP_POOL_OK && inf.head_frames == 1024);
  CHECK(rmp_pool_read(&r, 3, 1000, b, 2) == 2 && fabsf(b[0] - (float)(1000 * 700000 / 2147483648.0)) < 1e-6f && fabsf(b[3] + (float)(1001 * 700000 / 2147483648.0)) < 1e-6f);
  CHECK(rmp_pool_decode(file.data() + 44, 3000, 2, 32, 2999, b, 4) == 1 && fabsf(b[1] + (float)(2999 * 700000 / 2147483648.0)) < 1e-6f);
  // a sample that fits the head whole: all random access (head_frames == frames), no stream
  rmp_pool_src small = {44, 1000 * 8, 48000.0f, 2, 32};
  CHECK(rmp_pool_prepare(&r, 3, &small, rd, nullptr) && rmp_pool_info_get(&r, 3, &inf) == RMP_POOL_OK);
  CHECK(inf.where == RMP_POOL_HEAD && inf.head_frames == 1000 && inf.frames == 1000 && rmp_pool_ahead(&r) == 0xFFFFFFFFu);
  CHECK(rmp_pool_read(&r, 3, 990, b, 12) == 10 && rmp_pool_read(&r, 3, 5, b, 12) == 12 && r.seek == RMP_POOL_NONE);

  // a long sample (40000 frames, 16-bit mono) for the streaming cases below; one refill between requests
  // when fewer than 4096 frames are ahead (the sketch's loop job)
  const uint32_t lf = 40000;
  file.assign(44, 0);
  for (uint32_t i = 0; i < lf; ++i) { int16_t v = s16(i); file.push_back((uint8_t)v); file.push_back((uint8_t)(v >> 8)); }
  rmp_pool_src ls = {44, lf * 2, 48000.0f, 1, 16};
  auto exact = [&](uint32_t p, int32_t got) {
    for (int32_t k = 0; k < got; ++k)
      if (fabsf(b[k] - s16(p + (uint32_t)k) / 32768.0f) > 1e-6f) return false;
    return true;
  };
  // an interpolating reader (4-point: each block reads 15 frames from 1 before it, overlapping the last by 3)
  CHECK(rmp_pool_prepare(&r, 7, &ls, rd, nullptr));
  uint32_t shorts = 0;
  for (uint32_t k = 1; 12 * k + 14 < lf; ++k) {
    if (rmp_pool_ahead(&r) < 4096) rmp_pool_refill(&r);
    int32_t got = rmp_pool_read(&r, 7, 12 * k - 1, b, 15);
    CHECK(exact(12 * k - 1, got));
    if (got < 15) ++shorts;
  }
  CHECK(shorts == 0);
  // retriggers: the sample from frame 0 three times (back into the head re-arms the ring at its end)
  CHECK(rmp_pool_prepare(&r, 7, &ls, rd, nullptr));
  shorts = 0;
  for (int t = 0; t < 3; ++t)
    for (uint32_t p = 0; p + 12 <= 20000; p += 12) {
      if (rmp_pool_ahead(&r) < 4096) rmp_pool_refill(&r);
      int32_t got = rmp_pool_read(&r, 7, p, b, 12);
      CHECK(exact(p, got));
      if (got < 12) ++shorts;
    }
  CHECK(shorts == 0);
  // a forward skip past the ring's data and held there: urgent, and served within two refills
  for (int variant = 0; variant < 2; ++variant) {
    CHECK(rmp_pool_prepare(&r, 7, &ls, rd, nullptr));
    if (!variant) CHECK(rmp_pool_refill(&r) == 8192);   // the ring holds 4096..8191, half full
    for (uint32_t p = 0; p < 6000;) {
      if (variant && rmp_pool_ahead(&r) < 4096) rmp_pool_refill(&r);
      p += (uint32_t)rmp_pool_read(&r, 7, p, b, 12);
    }
    // within the ring's reach past its data (a wait) / beyond its reach (a seek)
    uint32_t to = variant ? 12488 : (r.rs + r.fill) / 2 + 100;
    CHECK(variant || (to * 2 >= r.rs + r.fill && to * 2 < r.rs + r.ring_cap));
    CHECK(rmp_pool_read(&r, 7, to, b, 12) == 0 && rmp_pool_ahead(&r) == 0);
    CHECK(variant ? r.seek == to : (r.wait == to && r.seek == RMP_POOL_NONE));
    int tries = 0;
    while (rmp_pool_read(&r, 7, to, b, 12) != 12 && tries < 10) { rmp_pool_refill(&r); ++tries; }
    CHECK(tries <= 2 && exact(to, 12));
  }
  // two positions at once, one in the head and one in the stream (two grains, a loop crossfade): the stream
  // position keeps its ring (no re-arm thrash), the head is always served
  for (int order = 0; order < 2; ++order) {
    for (uint32_t gb : {5000u, 9000u, 30000u}) {
      CHECK(rmp_pool_prepare(&r, 7, &ls, rd, nullptr));
      reads = 0;
      uint32_t sa = 0, sb = 0;
      for (uint32_t k = 0; k < 300; ++k) {
        if (rmp_pool_ahead(&r) < 4096) rmp_pool_refill(&r);
        uint32_t ga = (12 * k) % 3000, gbk = gb + 12 * k;   // A loops inside the head, B streams on
        int32_t x = 0, y = 0;
        if (order) { y = rmp_pool_read(&r, 7, gbk, b, 12); CHECK(exact(gbk, y)); x = rmp_pool_read(&r, 7, ga, b, 12); CHECK(exact(ga, x)); }
        else { x = rmp_pool_read(&r, 7, ga, b, 12); CHECK(exact(ga, x)); y = rmp_pool_read(&r, 7, gbk, b, 12); CHECK(exact(gbk, y)); }
        sa += 12u - (uint32_t)x; sb += 12u - (uint32_t)y;
      }
      CHECK(sa == 0 && sb <= 12 && reads <= 4);   // at most B's first block, while its stream starts
    }
  }
  // the same with the stream grain at 20000 and a sparser refill policy (fewer than 1024 ahead): ahead()
  // follows the stream position, not the head grain's, in both read orders
  for (int order = 0; order < 2; ++order)
    for (uint32_t thr : {4096u, 1024u}) {
      CHECK(rmp_pool_prepare(&r, 7, &ls, rd, nullptr));
      uint32_t ss = 0, sh = 0;
      for (uint32_t k = 0; k < 1500; ++k) {
        if (rmp_pool_ahead(&r) < thr) rmp_pool_refill(&r);
        uint32_t ga = 20000 + 12 * k, gh = (12 * k) % 3000;
        int32_t x, y;
        if (order) { x = rmp_pool_read(&r, 7, gh, b, 12); y = rmp_pool_read(&r, 7, ga, b, 12); }
        else { y = rmp_pool_read(&r, 7, ga, b, 12); x = rmp_pool_read(&r, 7, gh, b, 12); }
        sh += 12u - (uint32_t)x; ss += 12u - (uint32_t)y;
      }
      CHECK(sh == 0 && ss <= 12);
    }
  {   // a voice that stops after a stream read and a head read: one refill closes the period, then ahead()
      // reports its lead (not starving), so a picker with a 4096 threshold leaves it alone
    CHECK(rmp_pool_prepare(&r, 7, &ls, rd, nullptr));
    for (uint32_t p = 0; p < 20000; p += 12) {
      if (rmp_pool_ahead(&r) < 4096) rmp_pool_refill(&r);
      rmp_pool_read(&r, 7, p, b, 12);
    }
    rmp_pool_refill(&r);
    rmp_pool_read(&r, 7, 20000, b, 12);
    rmp_pool_read(&r, 7, 4096 - 20, b, 12);
    CHECK(rmp_pool_ahead(&r) == 8);   // reads since the refill, the last in the head: its frames left
    int calls = 0;
    for (int k = 0; k < 50; ++k)
      if (rmp_pool_ahead(&r) < 4096) { rmp_pool_refill(&r); ++calls; }
    CHECK(calls == 1 && rmp_pool_ahead(&r) >= 4096);
  }
  {   // a crossfade looper: loop [0, 30000), the last 480 frames read together with the head's first
    CHECK(rmp_pool_prepare(&r, 7, &ls, rd, nullptr));
    reads = 0;
    uint32_t sm = 0, sx = 0, p = 0;
    const uint32_t L = 30000, X = 480;
    for (int k = 0; k < 20000; ++k) {
      if (rmp_pool_ahead(&r) < 4096) rmp_pool_refill(&r);
      int32_t g = rmp_pool_read(&r, 7, p, b, 12);
      CHECK(exact(p, g));
      sm += 12u - (uint32_t)g;
      if (p >= L - X) { int32_t h = rmp_pool_read(&r, 7, p - (L - X), b, 12); CHECK(exact(p - (L - X), h)); sx += 12u - (uint32_t)h; }
      p += 12;
      if (p >= L) p = p - L + X;
    }
    // 8 passes of the loop, each streaming 30000 - 4096 frames: about 6.4 reads a pass
    CHECK(sm == 0 && sx == 0 && reads <= 70);
  }
  // the end: exactly the frames left
  CHECK(rmp_pool_read(&r, 7, lf - 3, b, 12) == 0);
  rmp_pool_refill(&r);
  CHECK(rmp_pool_read(&r, 7, lf - 3, b, 12) == 3 && exact(lf - 3, 3));
  CHECK(rmp_pool_read(&r, 7, lf, b, 12) == 0);
  printf("ok\n");
  return 0;
}

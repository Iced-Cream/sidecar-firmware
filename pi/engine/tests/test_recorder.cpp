// pi/engine/tests/test_recorder.cpp - the recorder (recorder.h) on a temp folder: the WAV header and its boot-time fix,
// the channel lists, a take's files (headers, lengths, every sample where it belongs: the files line up), 16-bit,
// the ring's overrun (dropped blocks written as silence, the files still aligned), arming by PLAY/STOP.
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "check.h"
#include "rec_wav.h"
#include "recorder.h"

static std::string g_root;
static FILE* g_f[rec::kSources];
static std::vector<std::string> g_log;
static std::string disk(const char* p) { return g_root + p; }

static bool BMkdir(const char* p) { return mkdir(disk(p).c_str(), 0755) == 0 || access(disk(p).c_str(), F_OK) == 0; }
static int BOpen(const char* p) {
  for (int i = 0; i < rec::kSources; ++i)
    if (!g_f[i]) return (g_f[i] = fopen(disk(p).c_str(), "wb+")) ? i : -1;
  return -1;
}
static bool BWrite(int h, const void* p, uint32_t n) { return fseek(g_f[h], 0, SEEK_END) == 0 && fwrite(p, 1, n, g_f[h]) == n; }
static bool BPwrite(int h, uint64_t off, const void* p, uint32_t n) {
  return fseek(g_f[h], (long)off, SEEK_SET) == 0 && fwrite(p, 1, n, g_f[h]) == n && fseek(g_f[h], 0, SEEK_END) == 0;
}
static bool BSync(int h) { return fflush(g_f[h]) == 0; }
static bool BClose(int h) { fclose(g_f[h]); g_f[h] = nullptr; return true; }
static bool BSpace(uint64_t* f, uint64_t* t) { *f = 1ull << 33, *t = 1ull << 34; return true; }
static uint32_t BNow() { timeval tv; gettimeofday(&tv, nullptr); return (uint32_t)(tv.tv_sec * 1000000 + tv.tv_usec); }
static int BList(const char* d, char (*names)[64], uint64_t* sizes, bool* dirs, int max) {
  DIR* dp = opendir(disk(d).c_str());
  if (!dp) return -1;
  int n = 0;
  while (dirent* e = readdir(dp)) {
    if (e->d_name[0] == '.' || n == max) continue;
    struct stat st;
    std::string p = disk(d) + "/" + e->d_name;
    stat(p.c_str(), &st);
    snprintf(names[n], 64, "%s", e->d_name);
    sizes[n] = (uint64_t)st.st_size;
    dirs[n] = S_ISDIR(st.st_mode);
    ++n;
  }
  closedir(dp);
  return n;
}
static bool BRead(const char* p, uint64_t off, void* b, uint32_t n) {
  FILE* f = fopen(disk(p).c_str(), "rb");
  if (!f) return false;
  bool ok = fseek(f, (long)off, SEEK_SET) == 0 && fread(b, 1, n, f) == n;
  fclose(f);
  return ok;
}
static bool BPatch(const char* p, uint64_t off, const void* b, uint32_t n) {
  FILE* f = fopen(disk(p).c_str(), "rb+");
  if (!f) return false;
  bool ok = fseek(f, (long)off, SEEK_SET) == 0 && fwrite(b, 1, n, f) == n;
  fclose(f);
  return ok;
}
static void BLog(const char* l) { g_log.push_back(l); printf("  log: %s\n", l); }
static const rec::Backend kB = {BMkdir, BOpen, BWrite, BPwrite, BSync, BClose, BSpace, BNow, BList, BRead, BPatch, BLog};

static std::vector<uint8_t> slurp(const std::string& p) {
  std::vector<uint8_t> v;
  FILE* f = fopen(disk(p.c_str()).c_str(), "rb");
  if (!f) return v;
  fseek(f, 0, SEEK_END);
  v.resize((size_t)ftell(f));
  fseek(f, 0, SEEK_SET);
  if (fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
  fclose(f);
  return v;
}

// the synthetic stream: IN channel c of frame t = (t << 8 | c) << 8 (unique per frame and channel, 24 bits of it kept)
static int32_t in_val(uint32_t t, int c) { return (int32_t)(((t & 0xFFFF) << 8 | (uint32_t)c) << 8); }
static int16_t voice_val(uint32_t t, int v) { return (int16_t)((t * 7 + (uint32_t)v * 1000) & 0x7FFF); }

static void feed(rec::Recorder& r, uint32_t& t, unsigned blocks, bool poll) {
  for (unsigned b = 0; b < blocks; ++b) {
    int32_t in[rec::kBlockFrames][rec::kInChannels];
    int16_t v[rec::kBlockFrames][rec::kVoices];
    float fx[rec::kBlockFrames][4];
    for (unsigned f = 0; f < rec::kBlockFrames; ++f) {
      for (int c = 0; c < rec::kInChannels; ++c) in[f][c] = in_val(t + f, c);
      for (int i = 0; i < rec::kVoices; ++i) v[f][i] = voice_val(t + f, i);
      for (int i = 0; i < 4; ++i) fx[f][i] = (i & 1 ? -0.25f : 0.5f);
    }
    r.out_block(v, fx, rec::kBlockFrames);
    r.in_block(in, rec::kBlockFrames);
    t += rec::kBlockFrames;
    if (poll) r.poll();
  }
}
static void finish(rec::Recorder& r) {
  for (int i = 0; i < 100000; ++i) {
    r.poll();
    rec::Status s;
    r.status(&s, false);
    if (s.state == rec::Recorder::kIdle || s.state == rec::Recorder::kArmed) return;
  }
  CHECK(!"the take never closed");
}
static int32_t s24(const uint8_t* p) { return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8; }

int main() {
  char tmp[] = "/tmp/rec-test-XXXXXX";
  CHECK(mkdtemp(tmp));
  g_root = tmp;

  // ---- WAV header
  uint8_t h[recwav::kHeader];
  recwav::build(h, 2, 24, 600);
  CHECK(recwav::ours(h) == 6 && recwav::get32(h + 4) == 504 + 600 && recwav::get32(h + 508) == 600);
  CHECK(recwav::get32(h + 24) == 48000 && recwav::get32(h + 28) == 288000 && recwav::get16(h + 34) == 24);
  CHECK(recwav::placeholder(3) % 3 == 0 && 504ull + recwav::placeholder(3) <= 0xFFFFFFFFull);
  recwav::build(h, 1, 24, recwav::placeholder(3));
  CHECK(recwav::fix(h, 512 + 3000 + 2) && recwav::get32(h + 508) == 3000);   // a partial frame dropped
  CHECK(!recwav::fix(h, 512 + 3000));                                        // already right
  uint8_t bad[512] = {0};
  CHECK(!recwav::ours(bad) && !recwav::fix(bad, 9999));

  // ---- channel lists
  uint32_t m = 0;
  char err[64];
  CHECK(rec::parse_list("all", &m, err, sizeof err) && m == (1u << rec::kSources) - 1);
  CHECK(rec::parse_list("none", &m, err, sizeof err) && m == 0);
  CHECK(rec::parse_list("master,bd,rs/cp,in_r,voice8,fx2", &m, err, sizeof err));
  CHECK(m == (1u << 0 | 1u << 3 | 1u << 5 | 1u << 12 | 1u << 20 | 1u << 22));
  CHECK(rec::parse_list("-bd,+1", &m, err, sizeof err) && m == (1u << 0 | 1u << 1 | 1u << 5 | 1u << 12 | 1u << 20 | 1u << 22));
  CHECK(rec::parse_list("0,in", &m, err, sizeof err) && m == (1u | 0xFFFu << 1));
  CHECK(!rec::parse_list("bd,xx", &m, err, sizeof err) && strstr(err, "xx"));
  CHECK(!rec::parse_list("23", &m, err, sizeof err));
  char ids[80];
  rec::list_ids(1u << 0 | 1u << 13 | 1u << 22, ids, sizeof ids);
  CHECK(strcmp(ids, "0,13,22") == 0);
  CHECK(rec::samples_per_frame(1u | 1u << 1 | 1u << 21) == 5 && rec::files(1u | 1u << 1 | 1u << 21) == 3);
  CHECK(rec::bytes_per_second(1u | 0xFFFu << 1, 24) == 14 * 3 * 48000);   // the 5-minute test's selection

  static rec::Recorder r;
  size_t memsz = (size_t)rec::kSources * rec::kChunk + (8u << 20);
  void* mem = aligned_alloc(64, memsz);
  r.init(mem, memsz, &kB);

  // ---- a take: master, MAIN_L, BD, IN_R, VOICE1, FX1; 24-bit; long enough for several chunks of each file
  CHECK(rec::parse_list("master,main_l,bd,in_r,voice1,fx1", &m, err, sizeof err));
  CHECK(r.configure(m, 24, err, sizeof err));
  CHECK(r.set_prefix("t") && !r.set_prefix("a/b") && !r.set_prefix(""));
  CHECK(r.start(err, sizeof err));
  uint32_t t = 0;
  feed(r, t, 1, true);   // the block that starts it is not recorded
  uint32_t t0 = t;
  feed(r, t, 30000, true);   // 210000 frames
  r.stop();
  finish(r);
  uint32_t frames = t - t0;
  rec::Status st;
  r.status(&st, true);
  CHECK(st.frames == frames && st.drops == 0 && st.side_under == 0 && strcmp(st.take, "t_001") == 0);
  CHECK(st.peak[3] > 0 && st.peak[3] <= 8388607);
  const char* names[] = {"00_MASTER", "01_MAIN_L", "03_BD", "12_IN_R", "13_VOICE1", "21_FX1"};
  int width[] = {2, 1, 1, 1, 1, 2};
  for (int k = 0; k < 6; ++k) {
    auto v = slurp(std::string("/Recordings/t_001/") + names[k] + ".wav");
    uint32_t align = 3 * width[k];
    CHECK(v.size() >= 512 && recwav::ours(v.data()) == align);
    uint32_t data = recwav::get32(v.data() + 508);
    CHECK(data == frames * align && v.size() == 512 + data + (data & 1));
    for (uint32_t f = 0; f < frames; f += 997) {   // every channel's sample at frame f is frame t0+f's
      const uint8_t* p = v.data() + 512 + f * align;
      uint32_t tt = t0 + f;
      if (k == 0) CHECK(s24(p) == in_val(tt, 0) >> 8 && s24(p + 3) == in_val(tt, 1) >> 8);
      if (k == 1) CHECK(s24(p) == in_val(tt, 0) >> 8);
      if (k == 2) CHECK(s24(p) == in_val(tt, 2) >> 8);
      if (k == 3) CHECK(s24(p) == in_val(tt, 11) >> 8);
      if (k == 4) CHECK(s24(p) == (int32_t)voice_val(tt - 7, 0) << 8);   // the OUT side's lead: the block before
      if (k == 5) CHECK(s24(p) == 0x400000 && s24(p + 3) == -0x200000);
    }
  }
  printf("test_recorder: a take of 6 files, %u frames each, aligned: PASS\n", frames);

  // ---- 16-bit, and the ring overrun: a small ring, nothing drained while 60000 frames come
  static rec::Recorder r2;
  size_t small = (size_t)rec::kSources * rec::kChunk + (1u << 20);
  void* mem2 = aligned_alloc(64, small);
  r2.init(mem2, small, &kB);
  CHECK(rec::parse_list("main_l,main_r,sd", &m, err, sizeof err));
  CHECK(r2.configure(m, 16, err, sizeof err) && r2.start(err, sizeof err));
  t = 0;
  feed(r2, t, 1, false);
  t0 = t;
  feed(r2, t, 60000 / 7, false);
  rec::Status s2;
  r2.status(&s2, false);
  CHECK(s2.drops > 0 && s2.ring_used == s2.ring_slots);
  uint32_t dropped = s2.dropped_frames;
  feed(r2, t, 2000, true);   // drained again (its first block still dropped), then more
  r2.stop();
  finish(r2);
  frames = t - t0;
  r2.status(&s2, false);
  CHECK(s2.frames == frames && s2.dropped_frames == dropped + 7);
  dropped = s2.dropped_frames;
  {
    auto a = slurp("/Recordings/take_001/01_MAIN_L.wav"), c = slurp("/Recordings/take_001/04_SD.wav");   // r2's prefix
    CHECK(recwav::ours(a.data()) == 2 && recwav::get32(a.data() + 508) == frames * 2 && c.size() == a.size());
    uint32_t kept = s2.ring_slots * rec::kBlockFrames, zero = 0, ok = 0;
    for (uint32_t f = 0; f < frames; ++f) {
      int16_t x = (int16_t)recwav::get16(a.data() + 512 + 2 * f), y = (int16_t)recwav::get16(c.data() + 512 + 2 * f);
      if (f < kept) CHECK(x == (int16_t)(in_val(t0 + f, 0) >> 16) && y == (int16_t)(in_val(t0 + f, 3) >> 16));
      else if (f < kept + dropped) { CHECK(x == 0 && y == 0); zero++; }
      else { CHECK(x == (int16_t)(in_val(t0 + f, 0) >> 16) && y == (int16_t)(in_val(t0 + f, 3) >> 16)); ok++; }
    }
    CHECK(zero == dropped && ok > 0);
    printf("test_recorder: 16-bit, overrun: %u blocks dropped, %u frames of silence, files still in time: PASS\n", s2.drops, zero);
  }

  // ---- armed: PLAY starts a take, STOP ends it, armed again; the next PLAY makes the next take
  CHECK(rec::parse_list("bd", &m, err, sizeof err) && r.configure(m, 24, err, sizeof err));
  CHECK(r.arm(err, sizeof err));
  t = 0;
  feed(r, t, 100, true);
  r.status(&st, false);
  CHECK(st.state == rec::Recorder::kArmed && st.frames == 0);
  r.transport(0xFA);
  feed(r, t, 1001, true);
  r.transport(0xFC);
  finish(r);
  r.status(&st, false);
  CHECK(st.state == rec::Recorder::kArmed && st.frames == 7000 && strcmp(st.take, "t_002") == 0);
  r.transport(0xFA);
  feed(r, t, 11, true);
  r.stop();   // a manual stop: the take ends and the arming with it
  finish(r);
  r.status(&st, false);
  CHECK(st.state == rec::Recorder::kIdle && st.frames == 70 && strcmp(st.take, "t_003") == 0);
  CHECK(!r.configure(m, 20, err, sizeof err));

  // ---- the boot scan: a take cut by a power loss (placeholder sizes, a partial frame at the end)
  {
    FILE* f = fopen(disk("/Recordings/t_003/03_BD.wav").c_str(), "rb+");
    recwav::build(h, 1, 24, recwav::placeholder(3));
    fwrite(h, 1, 512, f);
    fclose(f);
    truncate(disk("/Recordings/t_003/03_BD.wav").c_str(), 512 + 199);   // 66 frames and a third
    CHECK(r.fix_all("/Recordings") == 1 && r.fix_all("/Recordings") == 0);
    auto v = slurp("/Recordings/t_003/03_BD.wav");
    CHECK(recwav::get32(v.data() + 508) == 198 && recwav::get32(v.data() + 4) == 504 + 198);
  }
  free(mem);
  free(mem2);
  printf("test_recorder: PASS\n");
  return 0;
}

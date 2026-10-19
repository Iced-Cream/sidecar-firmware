// core/tests/test_mini_rec.cpp - the Sidecar Mini's recorder (core/rec/mini_rec.h) and the parts it shares with the
// Pi's (core/rec/rec_names.h, rec_wav.h) on a temp folder: stereo in the internal ring, stems refused without PSRAM
// and recorded with it (12 files, every sample where it belongs), a busy card (blocks dropped, written as silence,
// the files still aligned), a failing card, the stream stopping, PLAY / STOP arming, the power-loss repair, the take
// numbering and a full card.
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "check.h"
#include "rec/mini_rec.h"
#include "rec/rec_wav.h"

static std::string g_root;
static FILE* g_f[rec::kMiniSources];
static std::vector<std::string> g_log;
static uint32_t g_now = 1000;
static int g_busy = 0;          // polls the card stays busy
static bool g_fail = false;     // writes fail (the card pulled)
static uint64_t g_free = 8ull << 30, g_prealloc = 0;
static std::string disk(const char* p) { return g_root + p; }

static bool BMkdir(const char* p) { return mkdir(disk(p).c_str(), 0755) == 0 || access(disk(p).c_str(), F_OK) == 0; }
static int BOpen(const char* p, uint64_t pre) {
  g_prealloc = pre;
  for (int i = 0; i < rec::kMiniSources; ++i)
    if (!g_f[i]) return (g_f[i] = fopen(disk(p).c_str(), "wb+")) ? i : -1;
  return -1;
}
static bool BWrite(int h, const void* p, uint32_t n) {
  if (g_fail) return false;
  if (g_busy == 0) g_busy = 1;   // each write leaves the card busy for one poll
  return fseek(g_f[h], 0, SEEK_END) == 0 && fwrite(p, 1, n, g_f[h]) == n;
}
static bool BHeader(int h, const void* p, uint32_t n) {
  return !g_fail && fseek(g_f[h], 0, SEEK_SET) == 0 && fwrite(p, 1, n, g_f[h]) == n && fseek(g_f[h], 0, SEEK_END) == 0;
}
static bool BSync(int h) { return fflush(g_f[h]) == 0; }
static bool BClose(int h, uint64_t size) {
  fflush(g_f[h]);
  bool ok = ftruncate(fileno(g_f[h]), (off_t)size) == 0;
  fclose(g_f[h]);
  g_f[h] = nullptr;
  return ok && !g_fail;
}
static bool BBusy() { return g_busy > 0 ? (g_busy--, true) : false; }
static bool BSpace(uint64_t* f, uint64_t* t) { *f = g_free, *t = 16ull << 30; return true; }
static uint32_t BNow() { return g_now; }
static bool BList(const char* d, void (*fn)(void*, const char*, bool), void* ctx) {
  DIR* dp = opendir(disk(d).c_str());
  if (!dp) return false;
  while (dirent* e = readdir(dp)) {
    if (e->d_name[0] == '.') continue;
    struct stat st;
    stat((disk(d) + "/" + e->d_name).c_str(), &st);
    fn(ctx, e->d_name, S_ISDIR(st.st_mode));
  }
  closedir(dp);
  return true;
}
static bool BRead(const char* p, void* b, uint32_t n, uint64_t* size) {
  FILE* f = fopen(disk(p).c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  *size = (uint64_t)ftell(f);
  bool ok = fseek(f, 0, SEEK_SET) == 0 && fread(b, 1, n, f) == n;
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
static bool BSave(const char* p, const char* t) {
  FILE* f = fopen(disk(p).c_str(), "wb");
  if (!f) return false;
  fputs(t, f);
  fclose(f);
  return true;
}
static int BLoad(const char* p, char* out, unsigned cap) {
  FILE* f = fopen(disk(p).c_str(), "rb");
  if (!f) return -1;
  int n = (int)fread(out, 1, cap - 1, f);
  fclose(f);
  out[n] = 0;
  return n;
}
static bool BRemove(const char* p) { return unlink(disk(p).c_str()) == 0; }
static void BLog(const char* l) { g_log.push_back(l); printf("  log: %s\n", l); }
static const rec::MiniBackend kB = {BMkdir, BOpen, BWrite, BHeader, BSync, BClose, BBusy, BSpace, BNow,
                                    BList, BRead, BPatch, BSave, BLoad, BRemove, BLog};

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

// IN frame k, channel c: a value whose top 24 bits say both (and the sign varies)
static int32_t sample(uint32_t k, int c) { return (int32_t)(((k * 13u + (uint32_t)c * 977u) & 0xFFFFFFu) << 8); }

// n frames in Overbridge blocks of 7 (12 channels), a poll after each block (and `extra` more)
static uint32_t g_frame = 0;
static void feed(rec::MiniRecorder& r, uint32_t blocks, int extra = 1) {
  int32_t f[7 * 12];
  for (uint32_t b = 0; b < blocks; ++b) {
    for (int k = 0; k < 7; ++k)
      for (int c = 0; c < 12; ++c) f[k * 12 + c] = sample(g_frame + k, c);
    g_frame += 7;
    r.in_block(f, 7, 12);
    g_now += 146;
    for (int i = 0; i < extra; ++i) r.poll();
  }
}
static void drain(rec::MiniRecorder& r) {
  for (int i = 0; i < 10000 && r.busy(); ++i) g_now += 100, r.poll();
  CHECK(!r.busy());
}
static int32_t get_sample(const std::vector<uint8_t>& v, uint32_t i, unsigned bps) {
  const uint8_t* p = v.data() + recwav::kHeader + i * bps;
  return bps == 3 ? (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24)
                  : (int32_t)((uint32_t)p[0] << 16 | (uint32_t)p[1] << 24);
}

static void test_names() {
  uint32_t m = 0;
  char err[64];
  CHECK(rec::parse_list("master,bd,+in_l", &m, err, sizeof err) && m == (1u | 1u << 3 | 1u << 11));
  CHECK(rec::parse_list("in", &m, err, sizeof err) && m == rec::kInMask);
  CHECK(!rec::parse_list("nope", &m, err, sizeof err) && strstr(err, "nope"));
  CHECK(rec::take_number("take_007", "take") == 7 && rec::take_number("take_x", "take") == -1 && rec::take_number("tak_1", "take") == -1);
  char p[96];
  rec::file_path(p, sizeof p, "take_001", 5);
  CHECK(!strcmp(p, "/Recordings/take_001/05_RS-CP.wav"));
  CHECK(rec::valid_prefix("a-b_9") && !rec::valid_prefix("") && !rec::valid_prefix("a b"));
  volatile uint8_t go = 0;
  rec::transport_gate(0xFA, false, &go);
  CHECK(go == 0);
  rec::transport_gate(0xFA, true, &go);
  CHECK(go == 1);
  rec::transport_gate(0xFC, true, &go);
  CHECK(go == 0);
  CHECK(rec::samples_per_frame(1) == 2 && rec::bytes_per_second(1, 24) == 288000);
  printf("test_mini_rec: names, lists, take numbers, arming gate: PASS\n");
}

static void check_take(const char* take, uint32_t mask, unsigned bits, uint32_t first, uint32_t frames,
                       uint32_t silent_from = 0, uint32_t silent_n = 0) {
  unsigned bps = bits / 8;
  for (int id = 0; id < rec::kMiniSources; ++id) {
    if (!(mask & (1u << id))) continue;
    char p[96];
    rec::file_path(p, sizeof p, take, id);
    std::vector<uint8_t> v = slurp(p);
    const rec::Source& s = rec::source(id);
    uint32_t align = s.width * bps, data = frames * align;
    CHECK(v.size() == recwav::kHeader + data + (data & 1));
    CHECK(recwav::ours(v.data()) == align && recwav::get32(v.data() + recwav::kHeader - 4) == data);
    uint32_t bad = 0;
    for (uint32_t k = 0; k < frames; ++k)
      for (int c = 0; c < s.width; ++c) {
        int32_t want = sample(first + k, s.ch + c);
        if (silent_n && k >= silent_from && k < silent_from + silent_n) want = 0;
        int32_t mskd = bps == 3 ? (int32_t)(want & 0xFFFFFF00) : (int32_t)(want & 0xFFFF0000);
        if (get_sample(v, k * s.width + c, bps) != mskd) ++bad;
      }
    CHECK(bad == 0);
  }
}

static uint8_t g_internal[16384];   // the stock Teensy's ring (rec_mini.h RYTM_REC_RING_KB)

static void test_stereo() {
  rec::MiniRecorder r;
  r.init(g_internal, sizeof g_internal, 4096, 2, false, &kB);
  char err[80];
  rec::MiniStatus s;
  r.status(&s, false);
  CHECK(s.ready && s.ring_bytes == 16384 && s.block == 4096 && s.mask == 1 && s.bits == 24);
  CHECK(s.ring_ms == 56);   // 16 KB at 288 KB/s
  CHECK(!r.configure(rec::kInMask, 24, err, sizeof err) && strstr(err, "PSRAM"));
  CHECK(!r.configure(1u << 13, 24, err, sizeof err));   // the Pi's voices: not here
  CHECK(r.configure(1u | 0, 24, err, sizeof err));
  CHECK(r.start(err, sizeof err));
  feed(r, 1);   // the take starts with the next block
  uint32_t first = g_frame;
  feed(r, 3000);   // 21000 frames (0.44 s), 126 KB: several blocks
  r.stop();
  drain(r);
  r.status(&s, false);
  CHECK(!strcmp(s.take, "take_001") && s.frames == 21000 && s.drops == 0 && !s.error[0]);
  CHECK(s.writes >= 15 && s.peak[0] > 0);
  CHECK(g_prealloc > 0 && g_prealloc <= (256ull << 20));
  check_take("take_001", 1, 24, first, 21000);
  CHECK(access(disk(rec::kMarker).c_str(), F_OK) != 0);   // closed: no marker left
  // 16-bit, the next number
  CHECK(r.configure(1, 16, err, sizeof err) && r.set_prefix("take"));
  CHECK(r.start(err, sizeof err));
  feed(r, 1);
  first = g_frame;
  feed(r, 1001);
  r.stop();
  drain(r);
  r.status(&s, false);
  CHECK(!strcmp(s.take, "take_002") && s.frames == 7007);
  check_take("take_002", 1, 16, first, 7007);
  printf("test_mini_rec: stereo in a 16 KB ring (24 and 16 bit), stems refused without PSRAM (16 KB ring, 4 KB writes): PASS\n");
}

static void test_stems() {
  static uint8_t psram[1536 * 1024];
  rec::MiniRecorder r;
  r.init(psram, sizeof psram, 32768, 14, true, &kB);
  char err[80];
  CHECK(r.configure(rec::kInMask, 24, err, sizeof err) && r.set_prefix("stems"));
  rec::MiniStatus s;
  r.status(&s, false);
  CHECK(s.ring_bytes == 131072 && s.block == 32768 && s.need_kbps == 1687);
  CHECK(r.start(err, sizeof err));
  feed(r, 1, 3);
  uint32_t first = g_frame;
  feed(r, 4000, 3);   // 28000 frames
  r.stop();
  drain(r);
  r.status(&s, false);
  CHECK(!strcmp(s.take, "stems_001") && s.frames == 28000 && s.drops == 0);
  check_take("stems_001", rec::kInMask, 24, first, 28000);
  // MASTER and the stems at once (14 samples a frame)
  CHECK(r.configure(rec::kMiniMask, 24, err, sizeof err));
  CHECK(r.start(err, sizeof err));
  feed(r, 1, 3);
  first = g_frame;
  feed(r, 500, 3);
  r.stop();
  drain(r);
  check_take("stems_002", rec::kMiniMask, 24, first, 3500);
  printf("test_mini_rec: stems with PSRAM (12 and 13 files, aligned): PASS\n");
}

static void test_busy_card() {
  rec::MiniRecorder r;
  r.init(g_internal, sizeof g_internal, 4096, 2, false, &kB);
  char err[80];
  CHECK(r.set_prefix("slow") && r.configure(1, 24, err, sizeof err) && r.start(err, sizeof err));
  feed(r, 1);
  uint32_t first = g_frame;
  feed(r, 200);   // 1400 frames fine
  g_busy = 2000;  // the card busy for 2000 polls (~290 ms): the ring (57 ms) runs over
  feed(r, 2000);
  g_busy = 0;
  feed(r, 2000);
  r.stop();
  drain(r);
  rec::MiniStatus s;
  r.status(&s, false);
  CHECK(s.drops > 0 && s.dropped_frames == s.drops * 7 && s.busy_waits >= 2000 && s.frames == 4200 * 7);
  CHECK(s.ring_max_pct >= 90);
  // the files: every frame either its sample or silence, in time: the frames after the gap are where they belong
  char p[96];
  rec::file_path(p, sizeof p, "slow_001", 0);
  std::vector<uint8_t> v = slurp(p);
  CHECK(v.size() == recwav::kHeader + s.frames * 6);
  uint32_t zeros = 0, wrong = 0;
  for (uint32_t k = 0; k < s.frames; ++k) {
    int32_t l = get_sample(v, 2 * k, 3), want = (int32_t)(sample(first + k, 0) & 0xFFFFFF00);
    if (l == 0 && want != 0) ++zeros;
    else if (l != want) ++wrong;
  }
  CHECK(wrong == 0 && zeros == s.dropped_frames);
  printf("test_mini_rec: a busy card: %u blocks dropped, written as silence, the file in time: PASS\n", s.drops);
}

static void test_failures() {
  rec::MiniRecorder r;
  r.init(g_internal, sizeof g_internal, 4096, 2, false, &kB);
  char err[80];
  rec::MiniStatus s;
  // the card pulled mid-take: stopped, an error, idle
  CHECK(r.set_prefix("fail") && r.start(err, sizeof err));
  feed(r, 400);
  g_fail = true;
  feed(r, 400);
  drain(r);
  g_fail = false;
  r.status(&s, false);
  CHECK(s.state == 0 && strstr(s.error, "write failed"));
  // the stream stops (the Rytm unplugged): the take ends after kStallUs, its files whole
  CHECK(r.set_prefix("stall") && r.start(err, sizeof err));
  feed(r, 1);
  uint32_t first = g_frame;
  feed(r, 300);
  for (int i = 0; i < 100; ++i) g_now += 20000, r.poll();
  drain(r);
  r.status(&s, false);
  CHECK(s.state == 0 && strstr(s.error, "stream stopped") && s.frames == 2100);
  check_take("stall_001", 1, 24, first, 2100);
  // a full card: refused at the start, or stopped (files whole) when the free space runs out
  g_free = 10ull << 20;
  rec::MiniRecorder full;
  full.init(g_internal, sizeof g_internal, 4096, 2, false, &kB);
  CHECK(!full.start(err, sizeof err) && strstr(err, "card full"));
  rec::MiniRecorder near;
  near.init(g_internal, sizeof g_internal, 4096, 2, false, &kB);
  g_free = (18ull << 20) + (300 << 10);   // the 16 MB margin, 2 MB to start, 300 KB: stops after ~2.3 MB
  CHECK(near.set_prefix("full") && near.start(err, sizeof err));
  feed(near, 1);
  first = g_frame;
  feed(near, 70000);
  drain(near);
  near.status(&s, false);
  CHECK(s.state == 0 && strstr(s.error, "card full") && s.bytes > 0);
  check_take("full_001", 1, 24, first, s.frames);
  g_free = 8ull << 30;
  printf("test_mini_rec: card pulled, stream stopped, card full: PASS\n");
}

static void test_arm() {
  rec::MiniRecorder r;
  r.init(g_internal, sizeof g_internal, 4096, 2, false, &kB);
  char err[80];
  rec::MiniStatus s;
  CHECK(r.set_prefix("arm") && r.arm(err, sizeof err));
  feed(r, 100);
  r.status(&s, true);
  CHECK(s.state == 1 && s.frames == 0 && s.peak[0] > 0);   // armed: meters, nothing recorded
  r.transport(0xFA);
  feed(r, 1);
  uint32_t first = g_frame;
  feed(r, 300);
  r.transport(0xFC);
  for (int i = 0; i < 2000; ++i) {
    g_now += 100, r.poll();
    r.status(&s, false);
    if (s.state == 1) break;
  }
  CHECK(s.state == 1 && !strcmp(s.take, "arm_001"));   // re-armed for the next PLAY
  check_take("arm_001", 1, 24, first, 2100);
  r.transport(0xFA);
  feed(r, 1);
  feed(r, 100);
  r.stop();   // stop while recording in arm mode: ends, then idle (not re-armed)
  drain(r);
  r.status(&s, false);
  CHECK(s.state == 0 && !strcmp(s.take, "arm_002"));
  printf("test_mini_rec: PLAY / STOP arming, re-armed after a take: PASS\n");
}

static void test_power_loss() {
  // a take cut by a power loss: the placeholder sizes, and the marker naming it
  BMkdir("/Recordings");
  BMkdir("/Recordings/cut_001");
  uint8_t h[recwav::kHeader];
  recwav::build(h, 2, 24, recwav::placeholder(6));
  FILE* f = fopen(disk("/Recordings/cut_001/00_MASTER.wav").c_str(), "wb");
  fwrite(h, 1, sizeof h, f);
  std::vector<uint8_t> d(6000 + 3, 7);   // 1000 frames and a torn one
  fwrite(d.data(), 1, d.size(), f);
  fclose(f);
  BSave(rec::kMarker, "cut_001");
  rec::MiniRecorder r;
  r.init(g_internal, sizeof g_internal, 4096, 2, false, &kB);
  CHECK(r.fix_pending() == 1);
  std::vector<uint8_t> v = slurp("/Recordings/cut_001/00_MASTER.wav");
  CHECK(recwav::get32(v.data() + recwav::kHeader - 4) == 6000);
  CHECK(access(disk(rec::kMarker).c_str(), F_OK) != 0 && r.fix_pending() == 0);
  printf("test_mini_rec: the power-loss repair of the marked take: PASS\n");
}

int main() {
  char tmpl[] = "/tmp/mini_rec_XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_root = tmpl;
  test_names();
  test_stereo();
  test_stems();
  test_busy_card();
  test_failures();
  test_arm();
  test_power_loss();
  std::string rm = "rm -rf " + g_root;
  CHECK(system(rm.c_str()) == 0);
  printf("test_mini_rec: PASS\n");
  return 0;
}

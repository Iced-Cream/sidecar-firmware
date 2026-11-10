// core/samples/samples.cpp - see samples.h.
#include "samples.h"

#include <stdio.h>
#include <string.h>

#include <atomic>

#include "../browse/browse.h"
#include "../fmt/fmt.h"
#include "../platform.h"

namespace core {
namespace samples {

namespace {
struct Entry {
  std::atomic<uint8_t> state{kEmpty};   // READY publishes pcm..rate to the render side
  char path[kPathMax + 1];
  int16_t* pcm;
  uint32_t frames;
  uint8_t ch;
  float rate;
};
Entry s_e[kEntries];
uint8_t* s_arena;
size_t s_bytes, s_used;
const char* s_lists = "/.rytm/samples";
bool s_ok;

std::atomic<uint32_t> s_want_project{0};
std::atomic<bool> s_project_changed{false};
uint32_t s_project;
bool s_list_dirty;
uint32_t s_retire_at;   // now_ms() before which retired memory is not reused
bool s_retiring;
Stats s_st;

// the load under way
struct Load {
  int i = -1;
  File f;
  uint32_t left;        // bytes of data still to read
  uint32_t frames, done;
  int bits, ch, fmt;    // fmt 1 PCM, 3 float
  int16_t* dst;
};
Load s_ld;

// answers (card core -> control core): a small ring
struct Answer { uint8_t n; uint8_t d[40]; };
constexpr int kAnswers = 16;
Answer s_ans[kAnswers];
std::atomic<int> s_ans_rd{0}, s_ans_wr{0};

void answer(const uint8_t* body, int n) {
  int w = s_ans_wr.load(std::memory_order_relaxed);
  if (w - s_ans_rd.load(std::memory_order_acquire) >= kAnswers || n > 40 - 7) return;
  Answer& a = s_ans[w % kAnswers];
  static const uint8_t hdr[] = {0xF0, 0x7D, 0x52, 0x54, 0x51, 0x01};
  memcpy(a.d, hdr, 6);
  for (int k = 0; k < n; ++k) a.d[6 + k] = body[k] & 0x7F;
  a.d[6 + n] = 0xF7;
  a.n = (uint8_t)(7 + n);
  s_ans_wr.store(w + 1, std::memory_order_release);
}

const char* base_name(const char* p) {
  const char* b = strrchr(p, '/');
  return b ? b + 1 : p;
}

void list_path(char* out, size_t cap, uint32_t project) { sniprintf(out, cap, "%s/%08lx.lst", s_lists, (unsigned long)project); }

void save_list() {
  if (!s_project) return;
  static char buf[kEntries * (kPathMax + 6)];
  size_t n = 0;
  for (int i = 0; i < kEntries; ++i) {
    if (s_e[i].state.load(std::memory_order_relaxed) == kEmpty) continue;
    n += (size_t)sniprintf(buf + n, sizeof buf - n, "%d %s\n", i, s_e[i].path);
  }
  char p[64];
  list_path(p, sizeof p, s_project);
  fmt::write_atomic(p, (const uint8_t*)buf, (uint32_t)n);
  s_list_dirty = false;
}

// every entry unpublished, the arena to be reused once kRetireMs has passed
void retire_all() {
  if (s_ld.i >= 0) { s_ld.f.close(); s_ld.i = -1; }
  for (Entry& e : s_e) { e.state.store(kEmpty, std::memory_order_release); e.path[0] = 0; e.pcm = nullptr; }
  if (s_used) {   // memory the render side may have read: reused once its last read is surely over
    s_retiring = true;
    s_retire_at = now_ms() + kRetireMs;
  }
}

void read_list(uint32_t project) {
  char p[64];
  list_path(p, sizeof p, project);
  fmt::recover(p);
  File f;
  if (!f.open(p, kRead)) return;
  static char buf[kEntries * (kPathMax + 6) + 1];
  uint64_t size = f.fileSize();
  if (size >= sizeof buf) size = sizeof buf - 1;
  int got = (int)f.read(buf, (size_t)size);
  f.close();
  if (got < 0) return;
  buf[got] = 0;
  for (char* line = buf; line && *line;) {
    char* nl = strchr(line, '\n');
    if (nl) *nl = 0;
    int i = 0, off = 0;
    while (line[off] >= '0' && line[off] <= '9' && off < 4) i = i * 10 + (line[off++] - '0');
    bool num = off > 0 && line[off] == ' ';
    if (num) ++off;
    if (num && i < kEntries && line[off] == '/' && strlen(line + off) <= (size_t)kPathMax) {
      strcpy(s_e[i].path, line + off);
      s_e[i].state.store(kListed, std::memory_order_release);
    }
    line = nl ? nl + 1 : nullptr;
  }
}

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

// opens entry i's WAV and finds its data: false (FAILED) for anything it does not read
bool open_wav(int i) {
  Entry& e = s_e[i];
  Load& L = s_ld;
  if (!L.f.open(e.path, kRead)) return false;
  uint8_t h[12];
  if (L.f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
  bool have_fmt = false;
  float rate = 48000.0f;
  for (;;) {
    uint8_t c[8];
    if (L.f.read(c, 8) != 8) return false;
    uint32_t len = le32(c + 4);
    if (!memcmp(c, "fmt ", 4)) {
      uint8_t f[40];
      uint32_t k = len < sizeof f ? len : sizeof f;
      if (len < 16 || L.f.read(f, k) != (int)k) return false;
      if (len > k) L.f.seekSet(L.f.curPosition() + (len - k));
      L.fmt = le16(f);
      if (L.fmt == 0xFFFE && len >= 26) L.fmt = le16(f + 24);   // WAVE_FORMAT_EXTENSIBLE: its sub-format
      L.ch = le16(f + 2);
      rate = (float)le32(f + 4);
      L.bits = le16(f + 14);
      have_fmt = true;
    } else if (!memcmp(c, "data", 4)) {
      if (!have_fmt || (L.ch != 1 && L.ch != 2) || rate < 1000.0f) return false;
      if (!((L.fmt == 1 && (L.bits == 8 || L.bits == 16 || L.bits == 24 || L.bits == 32)) || (L.fmt == 3 && L.bits == 32)))
        return false;
      uint32_t fb = (uint32_t)(L.bits / 8 * L.ch);
      uint64_t avail = L.f.fileSize() - L.f.curPosition();
      if (len > avail) len = (uint32_t)avail;
      L.frames = len / fb;
      if (L.frames > kMaxFrames) L.frames = kMaxFrames;
      L.left = L.frames * fb;
      L.done = 0;
      e.ch = (uint8_t)L.ch;
      e.rate = rate;
      return true;
    } else {
      L.f.seekSet(L.f.curPosition() + len + (len & 1));
    }
  }
}

inline int16_t sample16(const uint8_t* p, int bits, int fmt) {
  if (fmt == 3) {
    float x;
    memcpy(&x, p, 4);
    x = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
    return (int16_t)(x * 32767.0f);
  }
  switch (bits) {
    case 8: return (int16_t)((p[0] - 128) << 8);
    case 16: return (int16_t)le16(p);
    case 24: return (int16_t)(p[1] | p[2] << 8);
    default: return (int16_t)(p[2] | p[3] << 8);
  }
}

// one step of the load under way, or the next entry's start
bool load_step() {
  Load& L = s_ld;
  if (L.i < 0) {
    for (int i = 0; i < kEntries; ++i) {
      if (s_e[i].state.load(std::memory_order_relaxed) != kListed) continue;
      s_e[i].state.store(kLoading, std::memory_order_relaxed);
      L.i = i;
      if (!open_wav(i)) {
        L.f.close();
        s_e[i].state.store(kFailed, std::memory_order_release);
        ++s_st.fails;
        L.i = -1;
        return true;
      }
      size_t need = (size_t)L.frames * L.ch * 2;
      if (s_used + need > s_bytes) {
        L.f.close();
        s_e[i].state.store(kTooBig, std::memory_order_release);
        ++s_st.fails;
        L.i = -1;
        return true;
      }
      L.dst = (int16_t*)(s_arena + s_used);
      s_used += (need + 63) & ~(size_t)63;
      return true;
    }
    return false;
  }
  static uint8_t buf[kChunk];
  const int fb = L.bits / 8 * L.ch;
  uint32_t want = L.left < kChunk ? L.left : kChunk;
  want -= want % (uint32_t)fb;
  int got = want ? L.f.read(buf, want) : 0;
  if (got <= 0 && L.left) {   // a short file or a card error: what came is the sample
    L.frames = L.done;
    L.left = 0;
  }
  const int bs = L.bits / 8;
  for (int k = 0; k + fb <= got; k += fb, ++L.done)
    for (int c = 0; c < L.ch; ++c) L.dst[(size_t)L.done * L.ch + c] = sample16(buf + k + c * bs, L.bits, L.fmt);
  L.left -= got > 0 ? (uint32_t)got : 0;
  if (L.left == 0) {
    Entry& e = s_e[L.i];
    L.f.close();
    e.pcm = L.dst;
    e.frames = L.done;
    e.state.store(L.done ? kReady : kFailed, std::memory_order_release);
    if (L.done) ++s_st.loads; else ++s_st.fails;
    L.i = -1;
  }
  return true;
}

int find_path(const char* p) {
  for (int i = 0; i < kEntries; ++i)
    if (s_e[i].state.load(std::memory_order_relaxed) != kEmpty && !strcmp(s_e[i].path, p)) return i;
  return -1;
}

// SADD's and SADDB's common end: p listed (or found), answered 76; p null: bad path
void add(uint8_t seq, uint8_t owner, const char* p) {
  uint8_t body[5] = {kIndex, seq, owner, 0x7F, kBad};
  if (!p || strstr(p, "/..")) { answer(body, 5); return; }
  int i = find_path(p);
  if (i < 0) {
    File f;
    if (!f.open(p, kRead) || f.isDir()) {
      f.close();
      body[4] = kMissing;
      answer(body, 5);
      return;
    }
    f.close();
    for (int k = 0; k < kEntries && i < 0; ++k)
      if (s_e[k].state.load(std::memory_order_relaxed) == kEmpty) i = k;
    if (i < 0) { body[4] = kFull; answer(body, 5); return; }
    strcpy(s_e[i].path, p);
    s_e[i].state.store(kListed, std::memory_order_release);
    ++s_st.adds;
    save_list();
  }
  body[3] = (uint8_t)i;
  body[4] = kOk;
  answer(body, 5);
}
}  // namespace

bool is_samples_op(uint8_t op) { return op == kAdd || op == kDrop || op == kInfo || op == kAddRef; }

bool begin(void* arena, size_t bytes, const char* lists) {
  uintptr_t a = ((uintptr_t)arena + 63) & ~(uintptr_t)63;
  if (!arena || bytes < 128 + (a - (uintptr_t)arena)) return false;
  s_arena = (uint8_t*)a;
  s_bytes = bytes - (a - (uintptr_t)arena);
  s_used = 0;
  if (lists) s_lists = lists;
  s_ok = true;
  return true;
}
bool ready() { return s_ok; }

void on_project(uint32_t project) {
  s_want_project.store(project, std::memory_order_relaxed);
  s_project_changed.store(true, std::memory_order_release);
}

bool card_op(const uint8_t* m, int n) {
  if (n < 9 || m[n - 1] != 0xF7) return false;
  const uint8_t op = m[6];
  if (op == kAdd) {
    if (n < 11) return false;
    uint8_t seq = m[7], owner = m[8];
    int pl = n - 10;
    char p[kPathMax + 1];
    if (!s_ok || pl < 2 || pl > kPathMax || m[9] != '/') return add(seq, owner, nullptr), true;
    for (int k = 0; k < pl; ++k) {
      char c = (char)m[9 + k];
      if (c < 0x20 || c == 0x7F) return add(seq, owner, nullptr), true;
      p[k] = c;
    }
    p[pl] = 0;
    add(seq, owner, p);
    return true;
  }
  if (op == kAddRef) {   // 3D SADDB <seq> <owner> <folder hi lo> <file hi lo>: a file core/browse listed
    if (n < 14) return false;
    uint8_t seq = m[7], owner = m[8];
    int folder = (m[9] & 0x7F) << 7 | (m[10] & 0x7F), file = (m[11] & 0x7F) << 7 | (m[12] & 0x7F);
    char p[512];
    if (!s_ok) return add(seq, owner, nullptr), true;
    if (!browse::file_path(folder, file, p, sizeof p)) {
      uint8_t body[5] = {kIndex, seq, owner, 0x7F, kMissing};
      answer(body, 5);
      return true;
    }
    add(seq, owner, strlen(p) <= (size_t)kPathMax ? p : nullptr);   // longer than a pool entry keeps: bad path
    return true;
  }
  if (op == kDrop) {
    int i = m[8];
    if (i < kEntries && s_e[i].state.load(std::memory_order_relaxed) != kEmpty) {
      if (s_ld.i == i) { s_ld.f.close(); s_ld.i = -1; }
      s_e[i].state.store(kEmpty, std::memory_order_release);
      s_e[i].path[0] = 0;
      ++s_st.drops;
      save_list();
    }
    return true;
  }
  if (op == kInfo) {
    if (n < 11) return false;
    int i = m[9];
    uint8_t b[9 + 16] = {kName, m[7], m[8], (uint8_t)(i & 0x7F), 0, 0, 0, 0, 0};
    int nb = 9;
    if (i < kEntries) {
      const Entry& e = s_e[i];
      uint8_t st = e.state.load(std::memory_order_acquire);
      uint32_t fr = st == kReady ? e.frames : 0;
      b[4] = st;
      b[5] = (uint8_t)(fr >> 21 & 0x7F); b[6] = (uint8_t)(fr >> 14 & 0x7F); b[7] = (uint8_t)(fr >> 7 & 0x7F);
      b[8] = (uint8_t)(fr & 0x7F);
      if (st != kEmpty)
        for (const char* s = base_name(e.path); *s && nb < 9 + 16; ++s) b[nb++] = (uint8_t)(*s & 0x7F);
    }
    answer(b, nb);
    return true;
  }
  return false;
}

bool card_poll() {
  if (!s_ok) return false;
  if (s_project_changed.exchange(false, std::memory_order_acq_rel)) {
    uint32_t p = s_want_project.load(std::memory_order_relaxed);
    if (p != s_project) {
      retire_all();
      s_project = p;
      s_st.project = p;
      if (p) read_list(p);
      return true;
    }
  }
  if (s_retiring) {
    if ((int32_t)(now_ms() - s_retire_at) < 0) return false;
    s_retiring = false;
    s_used = 0;
  }
  return load_step();
}

int take_answer(uint8_t* out, int cap) {
  int r = s_ans_rd.load(std::memory_order_relaxed);
  if (r == s_ans_wr.load(std::memory_order_acquire)) return 0;
  const Answer& a = s_ans[r % kAnswers];
  if (a.n > cap) return 0;
  memcpy(out, a.d, a.n);
  int n = a.n;
  s_ans_rd.store(r + 1, std::memory_order_release);
  return n;
}

bool get(int i, View* v) {
  if (i < 0 || i >= kEntries) return false;
  const Entry& e = s_e[i];
  if (e.state.load(std::memory_order_acquire) != kReady) return false;
  v->pcm = e.pcm;
  v->frames = e.frames;
  v->ch = e.ch;
  v->rate = e.rate;
  return true;
}

Stats stats() {
  Stats s = s_st;
  s.bytes_used = (uint32_t)(s_used > 0xFFFFFFFFu ? 0xFFFFFFFFu : s_used);
  s.bytes_total = (uint32_t)(s_bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : s_bytes);
  s.listed = s.ready = 0;
  for (const Entry& e : s_e) {
    uint8_t st = e.state.load(std::memory_order_relaxed);
    s.listed += st != kEmpty;
    s.ready += st == kReady;
  }
  return s;
}
State state(int i) { return i < 0 || i >= kEntries ? kEmpty : (State)s_e[i].state.load(std::memory_order_acquire); }
const char* path(int i) { return i < 0 || i >= kEntries ? "" : s_e[i].path; }

void reset_for_tests() {
  if (s_ld.i >= 0) { s_ld.f.close(); s_ld.i = -1; }
  for (Entry& e : s_e) { e.state.store(kEmpty); e.path[0] = 0; e.pcm = nullptr; }
  s_used = 0;
  s_project = 0;
  s_retiring = false;
  s_project_changed.store(false);
  s_ans_rd.store(0);
  s_ans_wr.store(0);
  s_st = Stats{};
}

}  // namespace samples
}  // namespace core

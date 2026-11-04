// core/browse/browse.cpp - see browse.h.
#include "browse/browse.h"

#include <string.h>
#include <strings.h>

#include "core_port.h"
#include "hooks.h"
#include "msamp/cardq.h"
#include "platform.h"

namespace core {
namespace browse {
namespace {
struct Dir { int16_t parent; char name[kPathName]; };
struct Ent { uint8_t kind; uint16_t id; uint32_t size; char name[kName + 1]; };

// the card core's: the folder table and the listing being read
Dir g_dir[kMaxFolders];
int g_ndirs = 1;
char g_root[64] = "/Samples";
File g_file;
bool g_reading = false;
uint32_t g_work = 0, g_done = 0;   // the ask being read, the last one answered
int g_folder = 0;

// the answer: written by the card core while g_ready is false, sent by the control side while it is true
Ent g_ent[kMaxEntries];
int g_n = 0, g_answer_folder = 0, g_answer_parent = 0;
uint32_t g_answer_req = 0;
bool g_ready = false;
int g_pos = 0;   // the control side's: entries sent

// the control side's ask
uint32_t g_req = 0;
int g_req_folder = 0;
bool g_card = false;
Stats g_st;

bool ready() { return __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE); }

int message(uint8_t* m, int f, int parent, int first, int total, const Ent* e, int count) {
  int n = 0;
  m[n++] = 0xF0, m[n++] = 0x7D, m[n++] = 0x52, m[n++] = 0x54, m[n++] = kCmdListing;
  m[n++] = (f >> 7) & 0x7F, m[n++] = f & 0x7F;
  m[n++] = (parent >> 7) & 0x7F, m[n++] = parent & 0x7F;
  m[n++] = (first >> 7) & 0x7F, m[n++] = first & 0x7F;
  m[n++] = (total >> 7) & 0x7F, m[n++] = total & 0x7F;
  for (int k = 0; k < count; ++k) {
    uint32_t size = e[k].size < (1u << 28) ? e[k].size : (1u << 28) - 1;
    m[n++] = e[k].kind;
    m[n++] = (e[k].id >> 7) & 0x7F, m[n++] = e[k].id & 0x7F;
    m[n++] = (size >> 21) & 0x7F, m[n++] = (size >> 14) & 0x7F, m[n++] = (size >> 7) & 0x7F, m[n++] = size & 0x7F;
    int len = (int)strlen(e[k].name);
    m[n++] = (uint8_t)len;
    memcpy(m + n, e[k].name, (size_t)len);
    n += len;
  }
  m[n++] = 0xF7;
  return n;
}
constexpr int kMsgMax = 13 + kPerMessage * (8 + kName) + 1;

void empty(int f) {
  uint8_t m[16];
  to_rytm(m, message(m, f, 0, 0, 0, nullptr, 0));
  ++g_st.messages;
}

// folder f's path: the root, then each folder's name down to it
int path(int f, char* out, int cap) {
  if (f <= 0) return sniprintf(out, cap, "%s", g_root);
  int n = path(g_dir[f].parent, out, cap);
  if (n < 0 || n >= cap) return cap;
  return n + sniprintf(out + n, cap - n, "/%s", g_dir[f].name);
}

int find_or_add(int parent, const char* name) {
  for (int i = 1; i < g_ndirs; ++i)
    if (g_dir[i].parent == parent && !strcmp(g_dir[i].name, name)) return i;
  if (g_ndirs >= kMaxFolders) return -1;
  g_dir[g_ndirs].parent = (int16_t)parent;
  memcpy(g_dir[g_ndirs].name, name, strlen(name) + 1);
  g_st.folders = g_ndirs + 1;
  return g_ndirs++;
}

bool before(const Ent& x, const Ent& y) {
  if (x.kind != y.kind) return x.kind > y.kind;   // folders first
  return strcasecmp(x.name, y.name) < 0;
}
// an insertion sort (no libc qsort on the Pi): kMaxEntries at most, on the card core
void sort(Ent* e, int n) {
  for (int i = 1; i < n; ++i) {
    Ent t = e[i];
    int j = i;
    while (j > 0 && before(t, e[j - 1])) e[j] = e[j - 1], --j;
    e[j] = t;
  }
}

void start(int f) {
  g_file.close();
  g_folder = f;
  g_n = 0;
  g_reading = true;
  char p[512];
  if (f < 0 || f >= g_ndirs) { g_reading = false; return; }   // a number this boot never gave: an empty folder
  int len = path(f, p, sizeof p);
  if (len >= (int)sizeof p || !g_file.open(p, kRead) || !g_file.isDir()) {
    g_file.close();
    g_reading = false;   // (a missing root: an empty folder)
  }
}

void finish() {
  g_file.close();
  g_reading = false;
  sort(g_ent, g_n);
  uint16_t file = 0;
  for (int i = 0; i < g_n; ++i) if (!g_ent[i].kind) g_ent[i].id = file++;
  g_answer_folder = g_folder;
  g_answer_parent = g_folder > 0 && g_folder < g_ndirs ? g_dir[g_folder].parent : 0;
  g_answer_req = g_work;
  g_done = g_work;
  g_pos = 0;
  __atomic_store_n(&g_ready, true, __ATOMIC_RELEASE);
}
}  // namespace

void begin(bool card) { g_card = card; }
void set_root(const char* p) { sniprintf(g_root, sizeof g_root, "%s", p && *p ? p : "/Samples"); }

bool sysex(const uint8_t* d, int len) {
  if (len < 8 || d[4] != kCmdList) return false;
  int f = (d[5] & 0x7F) << 7 | (d[6] & 0x7F);
  ++g_st.asks;
  if (!g_card) { empty(f); return true; }
  __atomic_store_n(&g_req_folder, f, __ATOMIC_RELAXED);
  __atomic_store_n(&g_req, g_req + 1, __ATOMIC_RELEASE);
  return true;
}

int card_step() {
  if (ready()) return 0;   // the control side still sending (or dropping) the last answer
  uint32_t r = __atomic_load_n(&g_req, __ATOMIC_ACQUIRE);
  if (r == g_done) return 0;
  if (r != g_work) {
    g_work = r;
    start(__atomic_load_n(&g_req_folder, __ATOMIC_RELAXED));
  }
  int k = 0;
  char name[256];
  bool dir;
  uint64_t size;
  uint16_t fd, ft;
  while (g_reading && k < kStepEntries) {
    if (!g_file.nextEntry(name, sizeof name, &dir, &size, &fd, &ft)) {
      if (g_file.nextFailed()) ++g_st.errors;   // (what was read is the answer)
      break;
    }
    ++k;
    if (name[0] == '.') continue;
    if (g_n >= kMaxEntries) { ++g_st.left_out; continue; }
    Ent& e = g_ent[g_n];
    e.kind = dir ? 1 : 0;
    e.id = 0;
    e.size = dir ? 0 : size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)size;
    if (dir) {
      int id = strlen(name) < (size_t)kPathName ? find_or_add(g_folder, name) : -1;
      if (id < 0) { ++g_st.left_out; continue; }
      e.id = (uint16_t)id;
    }
    int len = 0;
    for (const char* q = name; *q && len < kName; ++q) e.name[len++] = (*q >= 32 && *q < 127) ? *q : '?';
    e.name[len] = 0;
    ++g_n;
  }
  if (!g_reading || k < kStepEntries) finish();
  return k ? k : 1;
}

int host_step(int max_msgs) {
  if (!cardq::worker()) card_step();   // no card core (the Mac's tests): the reading runs here
  if (!ready()) return 0;
  if (g_answer_req != __atomic_load_n(&g_req, __ATOMIC_ACQUIRE)) {   // asked again since: this answer is stale
    ++g_st.superseded;
    __atomic_store_n(&g_ready, false, __ATOMIC_RELEASE);
    return 0;
  }
  int k = 0;
  uint8_t m[kMsgMax];
  while (k < max_msgs) {
    int count = g_n - g_pos < kPerMessage ? g_n - g_pos : kPerMessage;
    to_rytm(m, message(m, g_answer_folder, g_answer_parent, g_pos, g_n, g_ent + g_pos, count));
    ++k, ++g_st.messages;
    g_pos += count;
    if (g_pos >= g_n) {
      ++g_st.listings;
      g_st.entries += (uint32_t)g_n;
      __atomic_store_n(&g_ready, false, __ATOMIC_RELEASE);
      break;
    }
  }
  return k;
}

Stats stats() {
  Stats s = g_st;
  if (s.folders < 1) s.folders = 1;
  return s;
}
}  // namespace browse
}  // namespace core

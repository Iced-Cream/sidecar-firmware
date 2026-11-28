// core/code/code.cpp - see code.h.
#include "code.h"

#include <stddef.h>
#include <string.h>

#include <new>

#include "../dsp/dsp.h"
#include "../kv/kv_card.h"
#include "../lib/sha256.h"
#include "../platform.h"
#include "../proc/proc.h"

namespace core {
namespace code {

// a blob's io is the engine's own block: the same layout
static_assert(sizeof(scx_io) == sizeof(proc::Block), "scx_io = proc::Block");
static_assert(offsetof(scx_io, in) == offsetof(proc::Block, in) && offsetof(scx_io, out) == offsetof(proc::Block, out) &&
                  offsetof(scx_io, frames) == offsetof(proc::Block, frames) &&
                  offsetof(scx_io, sin) == offsetof(proc::Block, sin) &&
                  offsetof(scx_io, sout) == offsetof(proc::Block, sout),
              "scx_io = proc::Block");
static_assert(SCX_CHANNELS == proc::kChannels && SCX_SLOT_IO == proc::kSlotIO && SCX_PARAMS_MAX == proc::kParams, "ABI");
static_assert(kBlobs == dsp::kShipped, "one library entry a loaded blob");
static_assert(sizeof(scx_header) == 64, "SCX header");

namespace {
constexpr uint8_t kVer = 1;
constexpr const char* kDir = "/.rytm/code";
constexpr const char* kIndex = "/.rytm/code/index.txt";
constexpr const char* kArmed = "/.rytm/code/.armed";
constexpr uint32_t kAdapter = 256;   // the Proc adapter at the start of a slot; the blob's state after it

// ---- single producer, single consumer rings between the two cores ----
template <class T, uint32_t N>
struct Ring {
  T buf[N];
  alignas(64) uint32_t wr = 0;
  alignas(64) uint32_t rd = 0;
  bool push(const T& v) {
    uint32_t w = __atomic_load_n(&wr, __ATOMIC_RELAXED), r = __atomic_load_n(&rd, __ATOMIC_ACQUIRE);
    if (w - r >= N) return false;
    buf[w % N] = v;
    __atomic_store_n(&wr, w + 1, __ATOMIC_RELEASE);
    return true;
  }
  bool pop(T* v) {
    uint32_t r = __atomic_load_n(&rd, __ATOMIC_RELAXED), w = __atomic_load_n(&wr, __ATOMIC_ACQUIRE);
    if (r == w) return false;
    *v = buf[r % N];
    __atomic_store_n(&rd, r + 1, __ATOMIC_RELEASE);
    return true;
  }
  void clear() { __atomic_store_n(&rd, __atomic_load_n(&wr, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE); }
};

struct Answer { uint8_t n; uint8_t d[24]; };
Ring<Answer, 32> s_ans_ctl;    // control core's answers
Ring<Answer, 32> s_ans_card;   // the card core's (WANT, REFUSED)
enum ReqOp : uint8_t { kArm = 1, kDisarm = 2, kMarkBad = 3, kTouch = 4 };
struct Req { uint8_t op; uint8_t hash[32]; };
Ring<Req, 32> s_req;           // control -> card: crash marks, bad marks

Stats s_st;

void put32(uint8_t* b, uint32_t v) {
  b[0] = (uint8_t)(v >> 28 & 0x0F), b[1] = (uint8_t)(v >> 21 & 0x7F), b[2] = (uint8_t)(v >> 14 & 0x7F);
  b[3] = (uint8_t)(v >> 7 & 0x7F), b[4] = (uint8_t)(v & 0x7F);
}
uint32_t get32(const uint8_t* b) {
  return (uint32_t)b[0] << 28 | (uint32_t)b[1] << 21 | (uint32_t)b[2] << 14 | (uint32_t)b[3] << 7 | b[4];
}
Answer make_answer(const uint8_t* body, int n) {
  Answer a{};
  static const uint8_t hdr[] = {0xF0, 0x7D, 0x52, 0x54, 0x51, kVer};
  memcpy(a.d, hdr, 6);
  memcpy(a.d + 6, body, (size_t)n);
  a.d[6 + n] = 0xF7;
  a.n = (uint8_t)(7 + n);
  return a;
}
void refuse_on(Ring<Answer, 32>& q, int seq, int owner, int code) {
  const uint8_t b[] = {kRefused, (uint8_t)seq, (uint8_t)owner, (uint8_t)code};
  q.push(make_answer(b, 4));
}
void name8(uint8_t* out, const char* name) {
  for (int i = 0; i < 8; ++i) out[i] = 0;
  for (int i = 0; i < 8 && name[i]; ++i) out[i] = (uint8_t)(name[i] & 0x7F);
}
// a name from 8 wire bytes: NUL padded, 1..8 characters, nothing after the first NUL
bool name_from(const uint8_t* b, char* out) {
  int k = 0;
  while (k < 8 && b[k]) { out[k] = (char)b[k]; ++k; }
  out[k] = 0;
  if (!k) return false;
  for (int i = k; i < 8; ++i) if (b[i]) return false;
  return true;
}

// ================= control core =================
uint8_t* s_arena;
uint32_t s_units;
const Board* s_board;
uint8_t s_unit_used[kArenaBytes / kUnit * 4];   // a unit's blob + 1 (0 free); room for a 32 MB arena
uint8_t* s_buf[2];                              // the card core's two blob buffers

struct Blob {
  bool used;
  uint8_t hash[32];
  scx_header h;
  uint8_t* mem;
  uint32_t first, units;
  const scx_block* d;
  dsp::ParamInfo params[SCX_PARAMS_MAX];
  dsp::BlockInfo info;
  char name[SCX_NAME_MAX + 1];
  int owner;                  // this session's offerer; -1 none
  bool dropping;
  // crash marks: 1 armed at load (disarm at armed_at + 1 s), 2 waiting for the first PLACE, 3 armed at the first
  // PLACE (disarm at armed_at + 1 s), 0 done
  uint8_t arm;
  uint32_t armed_at;
};
Blob s_blob[kBlobs];

// what the card core read and checked, handed over in one of the two buffers
struct Ready { uint8_t seq, owner; uint8_t hash[32]; uint32_t size; int buf; };
Ready s_ready;
alignas(64) uint32_t s_ready_full;   // 1: s_ready is the control core's

void request(uint8_t op, const uint8_t* hash) {
  Req r{};
  r.op = op;
  memcpy(r.hash, hash, 32);
  s_req.push(r);
}

// ---- the Proc adapter: a blob's block in a processor slot ----
struct ScxProc final : proc::Proc {
  const scx_block* d;
  void* st;
  const char* nm;
  void process(const proc::Block& b) override { d->process(st, reinterpret_cast<const scx_io*>(&b)); }
  void set_param(int p, int v) override { d->set_param(st, p, v); }
  void link_reset() override { if (d->link_reset) d->link_reset(st); }
  void midi(int src, uint8_t status, uint8_t d1, uint8_t d2) override { if (d->midi) d->midi(st, src, status, d1, d2); }
  const char* name() const override { return nm; }
};
static_assert(sizeof(ScxProc) <= kAdapter, "the adapter fits");

proc::Proc* make_proc(void* mem, const void* ctx) {
  Blob* b = (Blob*)const_cast<void*>(ctx);
  if (!b || !b->used || b->dropping) return nullptr;
  uint8_t* m = (uint8_t*)mem;
  uint8_t* state = m + kAdapter;
  const uint32_t sb = (b->d->state_bytes + 63) & ~63u;
  memset(state, 0, sb);
  alloc_window(state + sb, dsp::kSlotBytes - kAdapter - sb);   // what make() may allocate: the rest of the slot
  void* st = b->d->make(state);
  alloc_window(nullptr, 0);
  if (!st) return nullptr;
  ScxProc* p = new (mem) ScxProc();
  p->d = b->d, p->st = st, p->nm = b->name;
  if (b->arm == 2) request(kArm, b->hash);   // its code runs for the first time: armed for a second
  if (b->arm == 1 || b->arm == 2) b->arm = 3, b->armed_at = now_ms();   // (within the load's second: that one extended)
  return p;
}

bool protect(uint8_t* p, size_t n, Prot prot) {
  if (s_board && s_board->protect) return s_board->protect(p, n, prot);
  if (prot != kProtRW) __builtin___clear_cache((char*)p, (char*)p + n);
  return true;
}

int find_hash(const uint8_t* hash) {
  for (int i = 0; i < kBlobs; ++i) if (s_blob[i].used && !memcmp(s_blob[i].hash, hash, 32)) return i;
  return -1;
}
int find_name(const char* name) {
  for (int i = 0; i < kBlobs; ++i) if (s_blob[i].used && !strcmp(s_blob[i].name, name)) return i;
  return -1;
}

void unload(int i) {
  Blob& b = s_blob[i];
  dsp::remove_block(&b.info);
  protect(b.mem, (size_t)b.units * kUnit, kProtRW);
  for (uint32_t u = b.first; u < b.first + b.units; ++u) s_unit_used[u] = 0;
  if (b.arm == 1 || b.arm == 3) request(kDisarm, b.hash);
  log("code: %s unloaded", b.name);
  b = Blob{};
  ++s_st.unloaded;
}

// first fit of n whole units; -1 none
int alloc_units(uint32_t n, int blob) {
  for (uint32_t s = 0; s + n <= s_units; ++s) {
    uint32_t k = 0;
    while (k < n && !s_unit_used[s + k]) ++k;
    if (k == n) {
      for (k = 0; k < n; ++k) s_unit_used[s + k] = (uint8_t)(blob + 1);
      return (int)s;
    }
    s += k;
  }
  return -1;
}
// unloads blobs nobody offered this session and no slot holds, oldest-loaded order not tracked: lowest index first
bool evict_one() {
  for (int i = 0; i < kBlobs; ++i)
    if (s_blob[i].used && s_blob[i].owner < 0 && !dsp::users(&s_blob[i].info)) { unload(i); return true; }
  return false;
}

void answer_loaded(int seq, int owner, const char* name) {
  uint8_t b[11] = {kLoaded, (uint8_t)seq, (uint8_t)owner};
  name8(b + 3, name);
  s_ans_ctl.push(make_answer(b, 11));
}
bool refuse_ctl(int seq, int owner, int code) {
  refuse_on(s_ans_ctl, seq, owner, code);
  ++s_st.refused;
  s_st.last_refused = code;
  return false;
}

// the card core read (fast path) or received a checked blob: load it
void load_ready(const Ready& r) {
  const uint8_t* f = s_buf[r.buf];
  ++s_st.from_card;
  // the card core armed the crash mark: disarmed again on every way out before the constructors run
  auto no = [&](int code) { request(kDisarm, r.hash); refuse_ctl(r.seq, r.owner, code); };
  int have = find_hash(r.hash);
  if (have >= 0) {   // loaded meanwhile (two offers of one blob)
    request(kDisarm, r.hash);
    Blob& b = s_blob[have];
    if (b.owner >= 0 && b.owner != r.owner) { refuse_ctl(r.seq, r.owner, kClash); return; }
    b.owner = r.owner, b.info.owner = r.owner, b.dropping = false;
    answer_loaded(r.seq, r.owner, b.name);
    return;
  }
  scx_header h;
  int c = check(f, r.size, &h);
  if (c != kOk) { no(c); return; }
  char name[SCX_NAME_MAX + 1];
  memcpy(name, h.name, SCX_NAME_MAX);
  name[SCX_NAME_MAX] = 0;
  if (dsp::builtin(name)) { no(kClash); return; }
  int other = find_name(name);
  if (other >= 0) {   // an older version: replaced when it is free and nobody else offered it
    const Blob& o = s_blob[other];
    if ((o.owner >= 0 && o.owner != r.owner) || dsp::users(&o.info)) { no(kClash); return; }
    unload(other);
  }
  int i = 0;
  while (i < kBlobs && s_blob[i].used) ++i;
  if (i == kBlobs && evict_one()) { i = 0; while (i < kBlobs && s_blob[i].used) ++i; }
  if (i == kBlobs) { no(kFull); return; }
  const uint32_t units = (mem_bytes(h) + kUnit - 1) / kUnit;
  int first = alloc_units(units, i);
  while (first < 0 && evict_one()) first = alloc_units(units, i);
  if (first < 0) { no(kFull); return; }
  Blob& b = s_blob[i];
  b = Blob{};
  b.used = true;
  memcpy(b.hash, r.hash, 32);
  b.h = h;
  b.first = (uint32_t)first, b.units = units;
  b.mem = s_arena + (size_t)first * kUnit;
  memcpy(b.name, name, sizeof b.name);
  b.owner = r.owner;
  auto fail = [&](int code) {
    protect(b.mem, (size_t)units * kUnit, kProtRW);
    for (uint32_t u = b.first; u < b.first + units; ++u) s_unit_used[u] = 0;
    if (code != kFull) request(kMarkBad, r.hash);   // the same bytes fail the same way every time
    request(kDisarm, r.hash);
    b = Blob{};
    refuse_ctl(r.seq, r.owner, code);
  };
  if (!protect(b.mem, (size_t)units * kUnit, kProtRW)) { fail(kFull); return; }
  c = place(f, r.size, b.mem, units * kUnit);
  if (c != kOk) { fail(c); return; }
  if (!protect(b.mem, (size_t)units * kUnit, kProtRWX)) { fail(kFull); return; }
  // the card core armed the crash mark before handing it over: the constructors may run
  run_init(b.mem, h);
  const scx_block* d = describe(b.mem, h, dsp::kSlotBytes - kAdapter, &c);
  if (!d) { fail(c); return; }
  protect(b.mem, (size_t)units * kUnit, kProtRX);
  b.d = d;
  for (uint32_t p = 0; p < d->nparams; ++p) b.params[p] = dsp::ParamInfo{d->params[p].name, d->params[p].def};
  b.info.name = b.name;
  b.info.nparams = (int)d->nparams;
  b.info.params = b.params;
  b.info.bytes = dsp::kSlotBytes;
  b.info.make = make_proc;
  b.info.ctx = &b;
  b.info.owner = r.owner;
  if (!dsp::add_block(&b.info)) { fail(kClash); return; }
  b.arm = 1;
  b.armed_at = now_ms();
  ++s_st.loaded;
  char hx[17];
  lib::hex(b.hash, 8, hx);
  log("code: %s loaded (%u bytes, %u KB arena, sha %s..), owner %d", b.name, (unsigned)r.size, (unsigned)(units * 64),
      hx, r.owner);
  answer_loaded(r.seq, r.owner, b.name);
}

// ================= card core =================
kv::CardBackend s_card_default;
kv::Backend* s_card;
uint64_t s_cap = kCacheCap;

struct Entry { uint8_t hash[32]; uint32_t size; char name[SCX_NAME_MAX + 1]; uint32_t last; bool bad; };
Entry s_idx[kIndexMax];
int s_nidx;
uint32_t s_gen, s_boot_gen;
bool s_card_up, s_idx_dirty;
uint8_t s_armed[kBlobs][32];
int s_narmed;

struct Transfer {
  bool on;
  uint8_t seq, owner;
  uint8_t hash[32];
  uint32_t size, got;
  char name[SCX_NAME_MAX + 1];
  uint32_t last_ms, asked_at;
  int asks;
};
Transfer s_tx;
int s_txbuf = 0;   // which buffer the transfer writes (the other is the load buffer)
char s_text[kIndexMax * 100 + 64];

void path_of(const uint8_t* hash, char* out, int cap) {
  char hx[65];
  lib::hex(hash, 32, hx);
  sniprintf(out, (size_t)cap, "%s/%s.scx", kDir, hx);
}
int idx_find(const uint8_t* hash) {
  for (int i = 0; i < s_nidx; ++i) if (!memcmp(s_idx[i].hash, hash, 32)) return i;
  return -1;
}
void idx_remove(int i) {
  for (int k = i; k + 1 < s_nidx; ++k) s_idx[k] = s_idx[k + 1];
  --s_nidx;
  s_idx_dirty = true;
}
uint64_t idx_bytes() {
  uint64_t n = 0;
  for (int i = 0; i < s_nidx; ++i) n += s_idx[i].size;
  return n;
}
void write_index() {
  int n = sniprintf(s_text, sizeof s_text, "scx-index 1 gen %lu\n", (unsigned long)s_gen);
  for (int i = 0; i < s_nidx && n < (int)sizeof s_text - 100; ++i) {
    char hx[65];
    lib::hex(s_idx[i].hash, 32, hx);
    n += sniprintf(s_text + n, sizeof s_text - (size_t)n, "%s %lu %s %lu %d\n", hx, (unsigned long)s_idx[i].size,
                   s_idx[i].name, (unsigned long)s_idx[i].last, s_idx[i].bad ? 1 : 0);
  }
  s_card->write_atomic(kIndex, (const uint8_t*)s_text, (uint32_t)n);
  s_idx_dirty = false;
}
void write_armed() {
  int n = 0;
  for (int i = 0; i < s_narmed; ++i) {
    lib::hex(s_armed[i], 32, s_text + n);
    n += 64;
    s_text[n++] = '\n';
  }
  if (n) s_card->write_atomic(kArmed, (const uint8_t*)s_text, (uint32_t)n);
  else s_card->remove(kArmed);
}

// ---- reading the index ----
bool parse_u32(const char*& p, uint32_t* v) {
  if (*p < '0' || *p > '9') return false;
  uint64_t x = 0;
  while (*p >= '0' && *p <= '9') { x = x * 10 + (uint64_t)(*p++ - '0'); if (x > 0xFFFFFFFFull) return false; }
  *v = (uint32_t)x;
  return true;
}
bool parse_index(int n) {
  s_text[n] = 0;
  const char* p = s_text;
  if (strncmp(p, "scx-index 1 gen ", 16)) return false;
  p += 16;
  if (!parse_u32(p, &s_gen) || *p++ != '\n') return false;
  while (*p && s_nidx < kIndexMax) {
    Entry e{};
    if (!lib::unhex_prefix(p, e.hash, 32) || p[64] != ' ') return false;
    p += 65;
    if (!parse_u32(p, &e.size) || *p++ != ' ') return false;
    int k = 0;
    while (*p && *p != ' ' && k < SCX_NAME_MAX) e.name[k++] = *p++;
    uint32_t bad = 0;
    if (*p++ != ' ' || !parse_u32(p, &e.last) || *p++ != ' ' || !parse_u32(p, &bad) || *p++ != '\n') return false;
    e.bad = bad != 0;
    s_idx[s_nidx++] = e;
  }
  return true;
}
// a lost or damaged index: every <hash>.scx in the folder that reads back with its hash and checks
void rebuild_each(const char* name, void*) {
  size_t L = strlen(name);
  if (L != 68 || strcmp(name + 64, ".scx") || s_nidx >= kIndexMax) return;
  Entry e{};
  if (!lib::unhex_prefix(name, e.hash, 32)) return;
  char path[96];
  path_of(e.hash, path, sizeof path);
  uint8_t* f = s_buf[s_txbuf];
  int got = s_card->read(path, f, kMaxBlob);
  uint8_t h[32];
  scx_header hd;
  if (got <= 0) return;
  lib::sha256(f, (size_t)got, h);
  if (memcmp(h, e.hash, 32) || check(f, (uint32_t)got, &hd) != kOk) { s_card->remove(path); return; }
  e.size = (uint32_t)got;
  memcpy(e.name, hd.name, SCX_NAME_MAX);
  s_idx[s_nidx++] = e;
}

void card_up() {
  if (s_card_up) return;
  s_card_up = true;
  if (!s_card) s_card = &s_card_default;
  s_nidx = 0;
  s_gen = 0;
  int n = s_card->read(kIndex, (uint8_t*)s_text, sizeof s_text - 1);
  if (n < 0 || !parse_index(n)) {
    s_nidx = 0;
    s_card->list(kDir, rebuild_each, nullptr);
    s_idx_dirty = true;
    log("code: card index rebuilt, %d blobs", s_nidx);
  }
  s_boot_gen = s_gen;
  // the crash mark: whatever was armed when the last boot ended has crashed it
  n = s_card->read(kArmed, (uint8_t*)s_text, sizeof s_text - 1);
  for (int k = 0; n > 0 && k + 64 <= n; k += 65) {
    uint8_t h[32];
    if (!lib::unhex_prefix(s_text + k, h, 32)) break;
    int i = idx_find(h);
    if (i >= 0) {
      s_idx[i].bad = true;
      s_idx_dirty = true;
      log("code: %s crashed the last boot: refused until a new version", s_idx[i].name);
    }
  }
  if (n >= 0) s_card->remove(kArmed);
  s_narmed = 0;
  s_tx = Transfer{};
}

void arm(const uint8_t* hash) {
  for (int i = 0; i < s_narmed; ++i) if (!memcmp(s_armed[i], hash, 32)) return;
  if (s_narmed < kBlobs) memcpy(s_armed[s_narmed++], hash, 32);
  write_armed();
}
void disarm(const uint8_t* hash) {
  for (int i = 0; i < s_narmed; ++i)
    if (!memcmp(s_armed[i], hash, 32)) {
      memmove(s_armed[i], s_armed[i + 1], (size_t)(s_narmed - i - 1) * 32);
      --s_narmed;
      write_armed();
      return;
    }
}

// the cache under its cap: the oldest offered first, never one offered this boot (or the one just written)
void evict_card(const uint8_t* keep) {
  while (s_nidx > 0 && (idx_bytes() > s_cap || s_nidx >= kIndexMax)) {
    int old = -1;
    for (int i = 0; i < s_nidx; ++i) {
      const Entry& e = s_idx[i];
      if (e.last > s_boot_gen || !memcmp(e.hash, keep, 32)) continue;
      if (old < 0 || e.last < s_idx[old].last) old = i;
    }
    if (old < 0) return;
    char path[96];
    path_of(s_idx[old].hash, path, sizeof path);
    s_card->remove(path);
    log("code: %s evicted from the card cache", s_idx[old].name);
    idx_remove(old);
  }
}

void card_refuse(int seq, int owner, int code) {
  refuse_on(s_ans_card, seq, owner, code);
  ++s_st.refused;
  s_st.last_refused = code;
}
void want(uint32_t off) {
  uint8_t b[8] = {kWant, s_tx.seq, s_tx.owner};
  put32(b + 3, off);
  s_ans_card.push(make_answer(b, 8));
  s_tx.asked_at = now_ms();
  ++s_st.wants;
}
// a checked blob in the load buffer: armed, then handed to the control core
void hand_over(int seq, int owner, const uint8_t* hash, uint32_t size, int buf) {
  arm(hash);
  s_ready = Ready{(uint8_t)seq, (uint8_t)owner, {0}, size, buf};
  memcpy(s_ready.hash, hash, 32);
  __atomic_store_n(&s_ready_full, 1u, __ATOMIC_RELEASE);
}
bool load_free() { return !__atomic_load_n(&s_ready_full, __ATOMIC_ACQUIRE); }

void card_offer(const uint8_t* b, int bn) {
  int seq = b[0], owner = b[1];
  uint8_t hash[37 + 3];
  if (kv::unpack7(b + 2, 37, hash) < 32) { ++s_st.ops_bad; return; }
  uint32_t size = get32(b + 39);
  char name[SCX_NAME_MAX + 1];
  if (!name_from(b + 44, name)) { card_refuse(seq, owner, kBad); return; }
  if (size < kHeader || size > kMaxBlob || size > s_cap) { card_refuse(seq, owner, size > kMaxBlob || size > s_cap ? kFull : kBad); return; }
  int i = idx_find(hash);
  if (i >= 0) {
    Entry& e = s_idx[i];
    if (e.bad) { card_refuse(seq, owner, kCrashed); return; }
    e.last = ++s_gen;
    s_idx_dirty = true;
    char path[96];
    path_of(hash, path, sizeof path);
    const int lb = 1 - s_txbuf;
    int got = s_card->read(path, s_buf[lb], kMaxBlob);
    uint8_t h[32];
    if (got == (int)size) lib::sha256(s_buf[lb], (size_t)got, h);
    if (got == (int)size && !memcmp(h, hash, 32) && !strcmp(e.name, name)) {
      ++s_st.fast;
      hand_over(seq, owner, hash, size, lb);
      return;
    }
    // damaged or not what was offered: forget it and take it again
    s_card->remove(path);
    idx_remove(i);
  }
  if (s_tx.on && !memcmp(s_tx.hash, hash, 32) && s_tx.size == size) {   // the same blob again: resume
    s_tx.seq = (uint8_t)seq, s_tx.owner = (uint8_t)owner;
    s_tx.asks = 0;
    want(s_tx.got);
    return;
  }
  s_tx = Transfer{};
  s_tx.on = true;
  s_tx.seq = (uint8_t)seq, s_tx.owner = (uint8_t)owner;
  memcpy(s_tx.hash, hash, 32);
  s_tx.size = size;
  memcpy(s_tx.name, name, sizeof name);
  s_tx.last_ms = now_ms();
  want(0);
}

void card_chunk(const uint8_t* b, int bn) {
  if (!s_tx.on || b[1] != s_tx.owner) { ++s_st.ops_bad; return; }
  uint32_t off = get32(b + 2);
  uint8_t raw[kv::kChunk + 8];
  int rn = bn > 7 ? kv::unpack7(b + 7, bn - 7, raw) : -1;
  if (rn <= 0 || rn > kv::kChunk) { ++s_st.ops_bad; return; }
  ++s_st.chunks;
  s_tx.last_ms = now_ms();
  if (off < s_tx.got) return;   // a repeat
  if (off > s_tx.got) {         // a gap: ask once from where it is, again only after a second
    if (now_ms() - s_tx.asked_at > 1000 || s_tx.asks == 0) { ++s_tx.asks; want(s_tx.got); }
    return;
  }
  if ((uint64_t)off + (uint32_t)rn > s_tx.size) { ++s_st.ops_bad; return; }
  memcpy(s_buf[s_txbuf] + off, raw, (size_t)rn);
  s_tx.got += (uint32_t)rn;
  s_tx.asks = 0;
}

void card_commit(const uint8_t* b) {
  int seq = b[0], owner = b[1];
  if (!s_tx.on || owner != s_tx.owner) { card_refuse(seq, owner, kBad); return; }
  ++s_st.commits;
  if (s_tx.got < s_tx.size) { s_tx.seq = (uint8_t)seq; want(s_tx.got); return; }
  s_tx.on = false;
  const uint8_t* f = s_buf[s_txbuf];
  uint8_t h[32];
  lib::sha256(f, s_tx.size, h);
  if (memcmp(h, s_tx.hash, 32)) { card_refuse(seq, owner, kHashBad); return; }
  scx_header hd;
  int c = check(f, s_tx.size, &hd);
  if (c == kOk && strncmp(hd.name, s_tx.name, SCX_NAME_MAX)) c = kBad;
  if (c != kOk) { card_refuse(seq, owner, c); return; }
  char path[96];
  path_of(h, path, sizeof path);
  evict_card(h);
  if (s_card->write_atomic(path, f, s_tx.size)) {
    int i = idx_find(h);
    if (i < 0 && s_nidx < kIndexMax) i = s_nidx++;
    if (i >= 0) {
      Entry& e = s_idx[i];
      e = Entry{};
      memcpy(e.hash, h, 32);
      e.size = s_tx.size;
      memcpy(e.name, s_tx.name, sizeof e.name);
      e.last = ++s_gen;
    }
    evict_card(h);
    write_index();
  } else {
    log("code: %s not written to the card (loaded for this boot only)", s_tx.name);
  }
  const int buf = s_txbuf;
  s_txbuf = 1 - s_txbuf;   // the received buffer becomes the load buffer
  hand_over(seq, owner, h, s_tx.size, buf);
}
}  // namespace

// ================= control core =================
bool begin(void* arena, size_t arena_bytes, uint8_t* buf_a, uint8_t* buf_b, const Board* board, kv::Backend* card) {
  if (!arena || ((uintptr_t)arena % kUnit) || arena_bytes % kUnit || arena_bytes / kUnit > sizeof s_unit_used) return false;
  if (!buf_a || !buf_b) return false;
  s_arena = (uint8_t*)arena;
  s_units = (uint32_t)(arena_bytes / kUnit);
  s_buf[0] = buf_a, s_buf[1] = buf_b;
  s_board = board;
  s_card = card;
  memset(s_unit_used, 0, sizeof s_unit_used);
  if (!protect(s_arena, arena_bytes, kProtRW)) return false;
  return true;
}
void set_cache_cap(uint64_t bytes) { s_cap = bytes ? bytes : kCacheCap; }

bool is_code_op(uint8_t op) { return op >= kOffer && op <= kDrop; }

int from_host(const uint8_t* m, int n) {
  if (n < 8 || m[0] != 0xF0 || m[4] != 0x51 || m[n - 1] != 0xF7 || !is_code_op(m[6])) { ++s_st.ops_bad; return -1; }
  const uint8_t* b = m + 7;
  const int bn = n - 8;
  for (int i = 0; i < bn; ++i) if (b[i] & 0x80) { ++s_st.ops_bad; return -1; }
  if (m[5] != kVer || !s_arena) {
    if (m[6] == kOffer && bn >= 2) refuse_ctl(b[0], b[1], s_arena ? kBad : kFull);
    ++s_st.ops_bad;
    return 0;
  }
  switch (m[6]) {
    case kOffer: {   // <seq> <owner> <sha 37> <size 5> <name 8>
      if (bn != 52) { ++s_st.ops_bad; if (bn >= 2) refuse_ctl(b[0], b[1], kBad); return 0; }
      ++s_st.offers;
      uint8_t hash[40];
      if (kv::unpack7(b + 2, 37, hash) < 32) { ++s_st.ops_bad; refuse_ctl(b[0], b[1], kBad); return 0; }
      int i = find_hash(hash);
      if (i < 0) return 1;   // the card core looks for it
      Blob& bl = s_blob[i];
      char name[SCX_NAME_MAX + 1];
      if (!name_from(b + 44, name) || strcmp(name, bl.name)) { refuse_ctl(b[0], b[1], kBad); return 0; }
      if (bl.owner >= 0 && bl.owner != b[1]) { refuse_ctl(b[0], b[1], kClash); return 0; }
      bl.owner = b[1], bl.info.owner = b[1], bl.dropping = false;
      request(kTouch, hash);   // its last-offered time on the card
      ++s_st.fast;
      answer_loaded(b[0], b[1], bl.name);
      return 0;
    }
    case kChunk:
      if (bn < 8 || bn > 7 + 128) { ++s_st.ops_bad; return -1; }
      return 1;
    case kCommit:
      if (bn != 2) { ++s_st.ops_bad; return -1; }
      return 1;
    case kDrop: {    // <owner> <name 8>
      char name[SCX_NAME_MAX + 1];
      if (bn != 9 || !name_from(b + 1, name)) { ++s_st.ops_bad; return -1; }
      int i = find_name(name);
      if (i < 0 || s_blob[i].owner != b[0]) { ++s_st.ops_bad; return 0; }
      ++s_st.drops;
      s_blob[i].dropping = true;
      s_blob[i].owner = -1;
      s_blob[i].info.owner = -2;   // nobody may PLACE it now
      poll();
      return 0;
    }
  }
  return -1;
}

void hello() {
  for (Blob& b : s_blob)
    if (b.used) b.owner = -1, b.info.owner = -2;
}

void poll() {
  if (__atomic_load_n(&s_ready_full, __ATOMIC_ACQUIRE)) {
    Ready r = s_ready;
    load_ready(r);
    __atomic_store_n(&s_ready_full, 0u, __ATOMIC_RELEASE);
  }
  const uint32_t now = now_ms();
  for (int i = 0; i < kBlobs; ++i) {
    Blob& b = s_blob[i];
    if (!b.used) continue;
    if (b.dropping && !dsp::users(&b.info)) { unload(i); continue; }
    if ((b.arm == 1 || b.arm == 3) && now - b.armed_at >= 1000) {
      request(kDisarm, b.hash);
      b.arm = b.arm == 1 ? 2 : 0;
    }
  }
}

int take_answer(uint8_t* out, int cap) {
  Answer a;
  // peek-free: an answer that does not fit is dropped (the queues' answers are at most 24 bytes)
  if (!s_ans_ctl.pop(&a) && !s_ans_card.pop(&a)) return 0;
  if (a.n > cap) return 0;
  memcpy(out, a.d, a.n);
  return a.n;
}

// ================= card core =================
bool card_can_take(const uint8_t* m, int n) {
  if (n < 7 || !is_code_op(m[6])) return true;
  return (m[6] != kOffer && m[6] != kCommit) || load_free();
}

void card_op(const uint8_t* m, int n) {
  card_up();
  const uint8_t* b = m + 7;
  const int bn = n - 8;
  switch (m[6]) {
    case kOffer: if (bn == 52) card_offer(b, bn); break;
    case kChunk: card_chunk(b, bn); break;
    case kCommit: if (bn == 2) card_commit(b); break;
    default: ++s_st.ops_bad;
  }
}

void card_hello() {
  card_up();
  s_tx = Transfer{};
}

void card_poll() {
  card_up();
  Req r;
  while (s_req.pop(&r)) {
    if (r.op == kArm) arm(r.hash);
    else if (r.op == kDisarm) disarm(r.hash);
    else if (r.op == kMarkBad || r.op == kTouch) {
      int i = idx_find(r.hash);
      if (i >= 0) {
        if (r.op == kMarkBad) s_idx[i].bad = true;
        else s_idx[i].last = ++s_gen;
        s_idx_dirty = true;
      }
    }
  }
  if (s_idx_dirty) write_index();
  // a transfer that went quiet before its end: asked again, three times a second apart
  if (s_tx.on && s_tx.got < s_tx.size && now_ms() - s_tx.last_ms > 1000 && now_ms() - s_tx.asked_at > 1000 && s_tx.asks < 3) {
    ++s_tx.asks;
    want(s_tx.got);
  }
  s_st.card_blobs = (uint32_t)s_nidx;
  s_st.card_bytes = idx_bytes();
}

void card_reboot_for_test() {
  s_card_up = false;
  s_tx = Transfer{};
  s_ans_card.clear();
  s_req.clear();
  __atomic_store_n(&s_ready_full, 0u, __ATOMIC_RELEASE);
}

int loaded() {
  int n = 0;
  for (const Blob& b : s_blob) n += b.used;
  return n;
}
bool blob(int i, BlobView* v) {
  for (const Blob& b : s_blob) {
    if (!b.used) continue;
    if (i-- == 0) {
      *v = BlobView{b.name, b.hash, b.owner, dsp::users(&b.info), mem_bytes(b.h)};
      return true;
    }
  }
  return false;
}
Stats stats() { return s_st; }

}  // namespace code
}  // namespace core

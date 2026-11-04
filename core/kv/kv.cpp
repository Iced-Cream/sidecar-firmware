// core/kv/kv.cpp - see kv.h.
#include "kv.h"

#include <string.h>

#include "platform.h"   // sniprintf (newlib's integer printf on the Pi: no snprintf there)

namespace core {
namespace kv {

const char* status_name(int s) {
  switch (s) {
    case kOk: return "ok";
    case kMissing: return "missing";
    case kNoProject: return "no project";
    case kFull: return "full";
    case kCardError: return "card error";
    case kBad: return "bad";
    case kTooBig: return "too big";
  }
  return "?";
}

static const char* dir_of(uint32_t project, char* out, int cap) {
  int n = sniprintf(out, cap, "/.rytm/store/%08lx", (unsigned long)project);
  return n > 0 && n < cap ? out : nullptr;
}

const char* Store::path(uint32_t project, int scope, uint16_t handle, char* out, int cap) {
  int n = sniprintf(out, cap, "/.rytm/store/%08lx/%d-%04x.bin", (unsigned long)project, scope, handle);
  return n > 0 && n < cap ? out : nullptr;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// "<scope>-<4 hex>.bin" -> the record; anything else in the folder (a .tmp, a .prev, a stray file) is not a record
void Store::add_name(const char* name, void* ctx) {
  Store* s = (Store*)ctx;
  if (strlen(name) != 10 || name[1] != '-' || strcmp(name + 6, ".bin") != 0) return;
  int scope = name[0] - '0';
  if (scope < 0 || scope >= kScopes) return;
  unsigned h = 0;
  for (int i = 2; i < 6; ++i) {
    int v = hexval(name[i]);
    if (v < 0) return;
    h = h << 4 | (unsigned)v;
  }
  if (h == 0 || s->n_ >= kMaxRecords || s->find(scope, (uint16_t)h) >= 0) return;
  s->recs_[s->n_].scope = (uint8_t)scope;
  s->recs_[s->n_].handle = (uint16_t)h;
  ++s->n_;
  uint16_t nh = (uint16_t)(h + 1);
  if (nh == 0) nh = 1;
  if (nh > s->next_) s->next_ = nh;
}

Status Store::open(uint32_t project) {
  project_ = project;
  n_ = 0;
  next_ = 1;
  if (!project) return kNoProject;
  char dir[kPathMax];
  if (!dir_of(project, dir, sizeof dir)) return kBad;
  b_->list(dir, add_name, this);   // no folder yet: a project with no records
  return kOk;
}

int Store::find(int scope, uint16_t handle) const {
  for (int i = 0; i < n_; ++i)
    if (recs_[i].scope == scope && recs_[i].handle == handle) return i;
  return -1;
}

Status Store::read(int scope, uint16_t handle, uint8_t* d, uint32_t cap, uint32_t* n) {
  *n = 0;
  if (!project_) return kNoProject;
  if (scope < 0 || scope >= kScopes || handle == 0) return kBad;
  if (find(scope, handle) < 0) return kMissing;
  char p[kPathMax];
  if (!path(project_, scope, handle, p, sizeof p)) return kBad;
  int r = b_->read(p, d, cap);
  if (r < 0) return kCardError;
  *n = (uint32_t)r;
  return kOk;
}

Status Store::write(int scope, const uint8_t* d, uint32_t n, uint16_t* handle) {
  *handle = 0;
  if (!project_) return kNoProject;
  if (scope < 0 || scope >= kScopes) return kBad;
  if (n > kMaxRecord) return kTooBig;
  if (n_ >= kMaxRecords) return kFull;
  // the next handle the project does not hold (wraps past 0xFFFF, skips 0 and live ones; n_ < 65535 so one is free)
  uint16_t h = next_ ? next_ : 1;
  while (h == 0 || find(scope, h) >= 0) h = (uint16_t)(h + 1);
  char p[kPathMax];
  if (!path(project_, scope, h, p, sizeof p)) return kBad;
  if (!b_->write_atomic(p, d, n)) return kCardError;
  recs_[n_].scope = (uint8_t)scope;
  recs_[n_].handle = h;
  ++n_;
  next_ = (uint16_t)(h + 1) ? (uint16_t)(h + 1) : 1;
  *handle = h;
  return kOk;
}

Status Store::keep(const uint8_t* scopes, const uint16_t* handles, int count, int* removed) {
  *removed = 0;
  if (!project_) return kNoProject;
  int w = 0;
  for (int i = 0; i < n_; ++i) {
    bool kept = false;
    for (int k = 0; k < count && !kept; ++k) kept = scopes[k] == recs_[i].scope && handles[k] == recs_[i].handle;
    char p[kPathMax];
    if (!kept && path(project_, recs_[i].scope, recs_[i].handle, p, sizeof p) && b_->remove(p)) {
      ++*removed;
      continue;
    }
    recs_[w++] = recs_[i];   // kept, or a remove that failed (tried again at the next KEEP)
  }
  n_ = w;
  return kOk;
}

// ---- codec

int pack7(const uint8_t* in, int n, uint8_t* out) {
  int o = 0;
  for (int i = 0; i < n; i += 7) {
    int g = n - i < 7 ? n - i : 7;
    uint8_t hi = 0;
    for (int j = 0; j < g; ++j) hi |= (uint8_t)((in[i + j] >> 7) << j);
    out[o++] = hi;
    for (int j = 0; j < g; ++j) out[o++] = in[i + j] & 0x7F;
  }
  return o;
}

int unpack7(const uint8_t* in, int n, uint8_t* out) {
  int o = 0;
  for (int i = 0; i < n;) {
    uint8_t hi = in[i++];
    if (hi & 0x80) return -1;
    int g = n - i < 7 ? n - i : 7;
    if (g == 0) return -1;
    if (hi >> g) return -1;   // a high bit for a byte the group does not have
    for (int j = 0; j < g; ++j) {
      if (in[i + j] & 0x80) return -1;
      out[o++] = (uint8_t)(in[i + j] | ((hi >> j & 1) << 7));
    }
    i += g;
  }
  return o;
}

static void put_h(uint8_t* b, uint16_t h) { b[0] = (uint8_t)(h >> 14 & 3); b[1] = (uint8_t)(h >> 7 & 0x7F); b[2] = (uint8_t)(h & 0x7F); }
static int get_h(const uint8_t* b) { return b[0] > 3 ? -1 : (b[0] << 14 | b[1] << 7 | b[2]); }
static void put_14(uint8_t* b, uint32_t v) { b[0] = (uint8_t)(v >> 7 & 0x7F); b[1] = (uint8_t)(v & 0x7F); }
static uint32_t get_14(const uint8_t* b) { return (uint32_t)b[0] << 7 | b[1]; }

void Service::send(const uint8_t* body, int n) {
  uint8_t m[8 + 16 + kChunk + kChunk / 7 + 2];
  int o = 0;
  m[o++] = 0xF0; m[o++] = 0x7D; m[o++] = 0x52; m[o++] = 0x54; m[o++] = kCmd; m[o++] = kVer;
  memcpy(m + o, body, (size_t)n);
  o += n;
  m[o++] = 0xF7;
  out_(m, o);
}

void Service::status(uint8_t seq, uint8_t code) {
  uint8_t b[3] = {kOpStatus, seq, code};
  ++c_.refused;
  c_.last_refused = code;
  send(b, 3);
}

void Service::sysex(const uint8_t* m, int n) {
  // F0 7D 52 54 51 01 <op> ... F7
  if (n < 8 || m[0] != 0xF0 || m[1] != 0x7D || m[2] != 0x52 || m[3] != 0x54 || m[4] != kCmd || m[n - 1] != 0xF7) return;
  if (m[5] != kVer) { status(0x7F, kBad); return; }
  const uint8_t* b = m + 7;
  int bn = n - 8;
  for (int i = 0; i < bn; ++i) if (b[i] & 0x80) { status(0x7F, kBad); return; }
  switch (m[6]) {
    case kOpHello: {
      if (bn != 5 || b[0] > 0x0F) { status(0x7F, kBad); return; }
      uint32_t p = (uint32_t)b[0] << 28 | (uint32_t)b[1] << 21 | (uint32_t)b[2] << 14 | (uint32_t)b[3] << 7 | b[4];
      wscope_ = -1; kn_ = 0; kbad_ = false;
      Status s = s_->open(p);
      if (s != kOk) { status(0x7F, s); return; }
      uint8_t r[4] = {kOpHelloAck, kVer};
      put_14(r + 2, (uint32_t)s_->records());
      ++c_.hellos;
      send(r, 4);
      return;
    }
    case kOpRead: {
      if (bn != 5) { status(0x7F, kBad); return; }
      uint8_t seq = b[0], scope = b[1];
      int h = get_h(b + 2);
      if (h < 0) { status(seq, kBad); return; }
      uint32_t len = 0;
      Status s = s_->read(scope, (uint16_t)h, wbuf_, sizeof wbuf_, &len);
      if (s != kOk) { status(seq, s); wscope_ = -1; return; }
      // wbuf_ is borrowed for the answer: a WRITE in progress is abandoned (the companion sends one at a time)
      wscope_ = -1;
      ++c_.reads;
      uint32_t off = 0;
      do {
        uint32_t c = len - off < (uint32_t)kChunk ? len - off : (uint32_t)kChunk;
        uint8_t r[10 + kChunk + kChunk / 7 + 1] = {kOpData, seq, scope};
        put_h(r + 3, (uint16_t)h);
        put_14(r + 6, len);
        put_14(r + 8, off);
        int o = 10 + pack7(wbuf_ + off, (int)c, r + 10);
        send(r, o);
        off += c;
      } while (off < len);
      return;
    }
    case kOpWrite: {
      if (bn < 6) { status(0x7F, kBad); return; }
      uint8_t seq = b[0], scope = b[1];
      uint32_t total = get_14(b + 2), off = get_14(b + 4);
      if (total > kMaxRecord) { status(seq, kTooBig); wscope_ = -1; return; }
      if (off == 0) { wscope_ = scope; wseq_ = seq; wtotal_ = total; wgot_ = 0; }
      else if (wscope_ != scope || wseq_ != seq || wtotal_ != total || wgot_ != off) { status(seq, kBad); wscope_ = -1; return; }
      uint8_t tmp[kChunk];
      int c = bn - 6 > 0 ? unpack7(b + 6, bn - 6, tmp) : 0;
      if (c < 0 || c > kChunk || wgot_ + (uint32_t)c > wtotal_) { status(seq, kBad); wscope_ = -1; return; }
      memcpy(wbuf_ + wgot_, tmp, (size_t)c);
      wgot_ += (uint32_t)c;
      if (wgot_ < wtotal_) return;
      uint16_t h = 0;
      Status s = s_->write(scope, wbuf_, wtotal_, &h);
      wscope_ = -1;
      if (s != kOk) { status(seq, s); return; }
      ++c_.writes;
      uint8_t r[6] = {kOpWritten, seq, scope};
      put_h(r + 3, h);
      send(r, 6);
      return;
    }
    case kOpKeep: {
      if (bn < 3 || bn != 3 + 4 * b[2]) { status(0x7F, kBad); kn_ = 0; return; }
      uint8_t seq = b[0], fin = b[1];
      for (int i = 0; i < b[2]; ++i) {
        int h = get_h(b + 3 + 4 * i + 1);
        if (h < 0 || kn_ >= kMaxRecords) { kbad_ = true; continue; }
        kscope_[kn_] = b[3 + 4 * i];
        khandle_[kn_] = (uint16_t)h;
        ++kn_;
      }
      if (!fin) return;
      // a list that went wrong drops nothing: a KEEP removes data, so only a whole, clean list may
      if (kbad_) { status(seq, kBad); kn_ = 0; kbad_ = false; return; }
      int removed = 0;
      Status s = s_->keep(kscope_, khandle_, kn_, &removed);
      kn_ = 0;
      if (s != kOk) { status(seq, s); return; }
      ++c_.keeps;
      uint8_t r[4] = {kOpKept, seq};
      put_14(r + 2, (uint32_t)removed);
      send(r, 4);
      return;
    }
  }
  status(0x7F, kBad);
}

}  // namespace kv
}  // namespace core

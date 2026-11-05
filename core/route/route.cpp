// core/route/route.cpp - see route.h.
#include "route.h"

#include <string.h>

namespace core {
namespace route {

namespace {
// ---- the control side (one core: the host link's and the lock sink's) ----
Point s_pt[kPoints];
Stats s_st;
int s_open_n;            // written by the control core, read by any() from the render side
struct Answer { uint8_t n; uint8_t d[12]; };
constexpr int kAnswers = 16;
Answer s_ans[kAnswers];
int s_ans_rd, s_ans_wr;

// ---- control -> render: a single-producer single-consumer ring ----
enum Kind : uint8_t { kAim = 0, kSetGain = 1, kClose = 2, kCloseAll = 3 };
struct Cmd { uint8_t kind, pt, src, dst; uint16_t gain; int64_t at; };   // at: the frame a stamped gain lands by, -1 now
constexpr uint32_t kQ = 128;
Cmd s_q[kQ];
alignas(64) uint32_t s_wr = 0;
alignas(64) uint32_t s_rd = 0;
uint32_t s_q_dropped;
bool push(const Cmd& c) {
  uint32_t w = __atomic_load_n(&s_wr, __ATOMIC_RELAXED), r = __atomic_load_n(&s_rd, __ATOMIC_ACQUIRE);
  if (w - r >= kQ) { ++s_q_dropped; return false; }
  s_q[w & (kQ - 1)] = c;
  __atomic_store_n(&s_wr, w + 1, __ATOMIC_RELEASE);
  return true;
}
bool pop(Cmd* c) {
  uint32_t r = __atomic_load_n(&s_rd, __ATOMIC_RELAXED), w = __atomic_load_n(&s_wr, __ATOMIC_ACQUIRE);
  if (r == w) return false;
  *c = s_q[r & (kQ - 1)];
  __atomic_store_n(&s_rd, r + 1, __ATOMIC_RELEASE);
  return true;
}

void set_open_n(int n) { __atomic_store_n(&s_open_n, n, __ATOMIC_RELEASE); s_st.open = n; }
int count_open() {
  int n = 0;
  for (const Point& p : s_pt) n += p.open;
  return n;
}

// loop nodes: the voices 0..7 (IN 2..9 / OUT 4..11) and the processor slots 8..11 (a slot's input and output are one
// node: the processor joins them); -1 for a channel that cannot be part of a loop (MAIN, MAIN FX, AUDIO IN)
bool is_slot(int c) { return c >= kChannels && c < kNodes; }
int slot_of(int c) { return (c - kChannels) / 2; }
int src_node(int c) { return c >= 2 && c <= 9 ? c - 2 : is_slot(c) ? 8 + slot_of(c) : -1; }
int dst_node(int c) { return c >= 4 && c <= 11 ? c - 4 : is_slot(c) ? 8 + slot_of(c) : -1; }
// does node `from` feed node `to` through open points (skipping point `skip`)?
bool reaches(int from, int to, int skip, int depth = 0) {
  if (from == to) return true;
  if (depth > kPoints) return false;
  for (int i = 0; i < kPoints; ++i) {
    const Point& p = s_pt[i];
    if (!p.open || i == skip) continue;
    int a = src_node(p.src), b = dst_node(p.dst);
    if (a == from && b >= 0 && reaches(b, to, skip, depth + 1)) return true;
  }
  return false;
}
// 0: fine; else the refusal code for src -> dst (point `skip` left out of the loop check: the one being re-aimed)
int check(int src, int dst, int skip) {
  if (src < 2 || src >= kNodes || dst < 0 || dst >= kNodes) return kBad;   // MAIN (IN 0-1) is never a source
  int a = src_node(src), b = dst_node(dst);
  if (a >= 0 && b >= 0 && reaches(b, a, skip)) return kLoop;   // the new edge a -> b closes a cycle back to a
  return 0;
}

void answer(const uint8_t* body, int n) {
  if ((s_ans_wr - s_ans_rd) >= kAnswers) return;
  Answer& a = s_ans[s_ans_wr % kAnswers];
  static const uint8_t hdr[] = {0xF0, 0x7D, 0x52, 0x54, 0x51, 0x01};
  memcpy(a.d, hdr, 6);
  memcpy(a.d + 6, body, (size_t)n);
  a.d[6 + n] = 0xF7;
  a.n = (uint8_t)(7 + n);
  ++s_ans_wr;
}
void routed(int seq, int owner, int pt) { const uint8_t b[] = {kRouted, (uint8_t)seq, (uint8_t)owner, (uint8_t)pt}; answer(b, 4); }
bool refuse(int seq, int owner, int code) {
  const uint8_t b[] = {kRefused, (uint8_t)seq, (uint8_t)owner, (uint8_t)code};
  answer(b, 4);
  ++s_st.refused;
  s_st.last_refused = code;
  return false;
}
uint16_t v14(const uint8_t* b) { return (uint16_t)(b[0] << 7 | b[1]); }

// ---- the step clock's stamp (control side): Rytm audio blocks -> the render side's frame count ----
alignas(64) int64_t s_frames;   // frames process() has rendered (render side writes, control side reads)
int s_lead = kLead;
int64_t s_target = -1;          // the frame the gains set now land by (-1: at once)
int64_t s_min, s_min_prev;      // the earliest a tick has arrived (frames - 32 x block): this window and the last
int64_t s_max;
int s_win_n;
bool s_have;
constexpr int kWindow = 256;    // ticks a window
constexpr int64_t kResync = 4800;   // an offset this far from the earliest (100 ms): a new stream, start again

// ---- the render side ----
struct Live {
  bool on, pend, closing;
  uint8_t src, dst, psrc, pdst;
  float cur, want, goal, inc;
  int left;
  int64_t gat;      // a stamped gain waiting: it starts its ramp at gat - kRamp and lands at gat (-1: none)
  float gwant;
  uint32_t blk;     // the block it was last run in (a point re-aimed mid-block to another source waits a block)
};
uint32_t s_blk;     // render side: blocks begun (process_in)
uint32_t s_live_slots;   // render side: bit s, a live point (on or fading) touches slot s; read by slot_quiet()
uint32_t s_late;   // render side: stamped gains that came too late for a whole ramp
Live s_live[kPoints];
int s_live_n;   // points on (render side), read by any()

void ramp_to(Live& l, float goal, int len = kRamp) {
  l.goal = goal;
  if (l.cur == goal) { l.left = 0; l.inc = 0; return; }
  if (len < 1) len = 1;
  l.inc = (goal - l.cur) / (float)len;
  l.left = len;
}
void arrived(Live& l);
void retarget(Live& l) {
  ramp_to(l, (l.pend || l.closing) ? 0.0f : l.want);
  if (l.left == 0) arrived(l);   // already there (a point at 0 re-aimed or closed)
}
// the point reached its goal: a pending re-aim is applied (then up again), a close finishes
void arrived(Live& l) {
  if (l.cur != 0.0f) return;
  if (l.closing) { l.on = false; l.closing = l.pend = false; return; }
  if (l.pend) { l.src = l.psrc; l.dst = l.pdst; l.pend = false; ramp_to(l, l.want); }
}
// a gain lands: ramps from where it is to the new value over len frames (unless a re-aim or close is under way)
void land(Live& l, float want, int len) {
  l.want = want;
  if (l.pend || l.closing) { retarget(l); return; }
  ramp_to(l, want, len);
  if (l.left == 0) arrived(l);
}
void apply(const Cmd& c, int64_t now) {
  if (c.kind == kCloseAll) {
    for (Live& l : s_live) if (l.on) { l.closing = true; retarget(l); }
    return;
  }
  if (c.pt >= kPoints) return;
  Live& l = s_live[c.pt];
  switch (c.kind) {
    case kAim:
      l.want = (float)c.gain / (float)kUnity;
      l.gat = -1;
      if (!l.on) {
        l.on = true; l.closing = l.pend = false;
        l.src = c.src; l.dst = c.dst; l.cur = 0.0f;
        ramp_to(l, l.want);
      } else if (l.src != c.src || l.dst != c.dst || l.closing) {
        l.closing = false;
        l.pend = true; l.psrc = c.src; l.pdst = c.dst;
        retarget(l);
      } else {
        retarget(l);
      }
      break;
    case kSetGain: {
      if (!l.on) break;
      float w = (float)c.gain / (float)kUnity;
      l.gat = -1;
      if (c.at < 0) { l.want = w; retarget(l); break; }
      if (c.at - kRamp > now) { l.gat = c.at; l.gwant = w; break; }   // early: wait, so it lands at its frame
      int len = (int)(c.at - now);
      if (len < kMinRamp) { len = kMinRamp; ++s_late; }
      land(l, w, len);
      break;
    }
    case kClose:
      l.gat = -1;
      if (l.on) { l.closing = true; retarget(l); }
      break;
  }
}
}  // namespace

bool is_route_op(uint8_t op) { return op >= kRoute && op <= kGain; }

bool sysex(const uint8_t* m, int n) {
  if (n < 8 || m[0] != 0xF0 || m[4] != 0x51 || m[n - 1] != 0xF7 || !is_route_op(m[6])) { ++s_st.ops_bad; return false; }
  const uint8_t* b = m + 7;
  int bn = n - 8;
  for (int i = 0; i < bn; ++i) if (b[i] & 0x80) { ++s_st.ops_bad; return false; }
  bool asks = m[6] == kRoute || m[6] == kMove;   // these two are answered, refusals too (when seq and owner came)
  if (m[5] != 1) { ++s_st.ops_bad; return asks && bn >= 2 ? refuse(b[0], b[1], kBad) : false; }
  switch (m[6]) {
    case kRoute: {   // <seq> <owner> <src> <dst> <hi> <lo>
      if (bn != 6) { ++s_st.ops_bad; return bn >= 2 ? refuse(b[0], b[1], kBad) : false; }
      int seq = b[0], c = check(b[2], b[3], -1);
      if (c) return refuse(seq, b[1], c);
      int pt = -1;
      for (int i = 0; i < kPoints && pt < 0; ++i) if (!s_pt[i].open) pt = i;
      if (pt < 0) return refuse(seq, b[1], kFull);
      uint16_t g = v14(b + 4);
      if (!push({kAim, (uint8_t)pt, b[2], b[3], g, -1})) return refuse(seq, b[1], kFull);
      s_pt[pt] = Point{true, b[1], b[2], b[3], g};
      set_open_n(count_open());
      ++s_st.routed;
      routed(seq, b[1], pt);
      return true;
    }
    case kMove: {    // <seq> <owner> <point> <src> <dst>
      if (bn != 5) { ++s_st.ops_bad; return bn >= 2 ? refuse(b[0], b[1], kBad) : false; }
      int seq = b[0], pt = b[2];
      if (pt >= kPoints || !s_pt[pt].open || s_pt[pt].owner != b[1]) return refuse(seq, b[1], kBad);
      int c = check(b[3], b[4], pt);
      if (c) return refuse(seq, b[1], c);
      if (!push({kAim, (uint8_t)pt, b[3], b[4], s_pt[pt].gain, -1})) return refuse(seq, b[1], kFull);
      s_pt[pt].src = b[3];
      s_pt[pt].dst = b[4];
      ++s_st.moved;
      routed(seq, b[1], pt);
      return true;
    }
    case kUnroute: { // <owner> <point | 7F>
      if (bn != 2) { ++s_st.ops_bad; return false; }
      bool any_closed = false;
      for (int i = 0; i < kPoints; ++i) {
        Point& p = s_pt[i];
        if (!p.open || p.owner != b[0] || (b[1] != 0x7F && b[1] != i)) continue;
        if (!push({kClose, (uint8_t)i, 0, 0, 0, -1})) break;
        p.open = false;
        ++s_st.closed;
        any_closed = true;
      }
      set_open_n(count_open());
      if (!any_closed) ++s_st.ops_bad;
      return any_closed;
    }
    case kGain: {    // <owner> <point> <hi> <lo>
      if (bn != 4 || b[1] >= kPoints || !s_pt[b[1]].open || s_pt[b[1]].owner != b[0]) { ++s_st.ops_bad; return false; }
      int64_t t = s_target;
      s_target = -1;   // GAIN itself is never stamped
      bool ok = set_gain(b[1], v14(b + 2));
      s_target = t;
      return ok;
    }
  }
  return false;
}

int take_answer(uint8_t* out, int cap) {
  if (s_ans_rd == s_ans_wr) return 0;
  const Answer& a = s_ans[s_ans_rd % kAnswers];
  if (a.n > cap) return 0;
  memcpy(out, a.d, a.n);
  ++s_ans_rd;
  return a.n;
}

bool set_gain(int pt, int v) {
  if (pt < 0 || pt >= kPoints || !s_pt[pt].open || v < 0 || v > 16383) return false;
  if (!push({kSetGain, (uint8_t)pt, 0, 0, (uint16_t)v, s_target})) return false;
  s_pt[pt].gain = (uint16_t)v;
  ++s_st.gains;
  if (s_target >= 0) ++s_st.timed;
  return true;
}

void reset() {
  bool had = count_open() > 0;
  for (Point& p : s_pt) p.open = false;
  set_open_n(0);
  if (had || __atomic_load_n(&s_live_n, __ATOMIC_ACQUIRE)) push({kCloseAll, 0, 0, 0, 0, -1});
  s_have = false;   // the next stream's ticks start the stamp's estimate again
  s_target = -1;
}

void stamp(int64_t block) {
  if (block < 0) { s_target = -1; return; }
  int64_t now = __atomic_load_n(&s_frames, __ATOMIC_ACQUIRE);
  int64_t off = now - (int64_t)kBlockFrames * block;
  if (!s_have || off < s_min - kResync || off > s_min + kResync) {
    s_have = true;
    s_min = s_min_prev = s_max = off;
    s_win_n = 0;
    ++s_st.resyncs;
  }
  if (off < s_min) s_min = off;
  if (off > s_max) s_max = off;
  int64_t earliest = s_min < s_min_prev ? s_min : s_min_prev;
  s_st.spread = (int)(s_max - earliest);
  if (++s_win_n >= kWindow) { s_min_prev = s_min; s_min = s_max = off; s_win_n = 0; }
  s_target = (int64_t)kBlockFrames * block + earliest + s_lead;
}

int64_t frames() { return __atomic_load_n(&s_frames, __ATOMIC_ACQUIRE); }
void set_lead(int frames) { s_lead = frames < 0 ? 0 : frames > 4800 ? 4800 : frames; }

bool any() {
  return __atomic_load_n(&s_open_n, __ATOMIC_ACQUIRE) > 0 || __atomic_load_n(&s_live_n, __ATOMIC_ACQUIRE) > 0;
}

namespace {
const float* src_buf(int c, const float* const* in, const float* const* sout) {
  if (c < kChannels) return in ? in[c] : nullptr;
  return sout ? sout[c - kChannels] : nullptr;
}
float* dst_buf(int c, float* const* out, float* const* sin) {
  if (c < kChannels) return out ? out[c] : nullptr;
  return sin ? sin[c - kChannels] : nullptr;
}
// one point's frames from the buffers this stage has (null: not here); a re-aim mid-block takes effect at its frame
struct Bufs { const float* const* in; const float* const* sout; float* const* out; float* const* sin; };
void run(Live& l, const Bufs& b, int64_t now, int frames) {
  const float* src = src_buf(l.src, b.in, b.sout);
  float* dst = dst_buf(l.dst, b.out, b.sin);
  for (int f = 0; f < frames; ++f) {
    if (l.gat >= 0 && now + f >= l.gat - kRamp) { l.gat = -1; land(l, l.gwant, kRamp); }
    if (l.left > 0) {
      l.cur += l.inc;
      if (--l.left == 0) {
        l.cur = l.goal;
        arrived(l);
        if (!l.on) return;
        src = src_buf(l.src, b.in, b.sout);   // (re-aimed: a source of the other stage reads nothing here)
        dst = dst_buf(l.dst, b.out, b.sin);
      }
    }
    if (l.cur != 0.0f && src && dst) dst[f] += l.cur * src[f];
  }
}
void count_live() {
  int on = 0;
  uint32_t touch = 0;
  for (const Live& l : s_live) {
    if (!l.on) continue;
    ++on;
    if (is_slot(l.src)) touch |= 1u << slot_of(l.src);
    if (is_slot(l.dst)) touch |= 1u << slot_of(l.dst);
    if (l.pend && is_slot(l.psrc)) touch |= 1u << slot_of(l.psrc);
    if (l.pend && is_slot(l.pdst)) touch |= 1u << slot_of(l.pdst);
  }
  __atomic_store_n(&s_live_slots, touch, __ATOMIC_RELEASE);
  __atomic_store_n(&s_live_n, on, __ATOMIC_RELEASE);
}
}  // namespace

void process_in(const float* const* in, float* const* out, float* const* sin, int frames) {
  int64_t now = __atomic_load_n(&s_frames, __ATOMIC_RELAXED);
  Cmd c;
  while (pop(&c)) apply(c, now);
  ++s_blk;
  for (Live& l : s_live) {
    if (!l.on || is_slot(l.src)) continue;
    l.blk = s_blk;
    run(l, Bufs{in, nullptr, out, sin}, now, frames);
  }
}

void order(uint8_t* slots) {
  // Kahn's algorithm over the live points between slots; whatever a (fading) cycle leaves goes last, in slot order
  int indeg[kSlots] = {};
  for (const Live& l : s_live)
    if (l.on && is_slot(l.src) && is_slot(l.dst) && slot_of(l.src) != slot_of(l.dst)) ++indeg[slot_of(l.dst)];
  bool done[kSlots] = {};
  int n = 0;
  for (bool progress = true; progress && n < kSlots;) {
    progress = false;
    for (int s = 0; s < kSlots; ++s) {
      if (done[s] || indeg[s]) continue;
      done[s] = true;
      slots[n++] = (uint8_t)s;
      progress = true;
      for (const Live& l : s_live)
        if (l.on && is_slot(l.src) && is_slot(l.dst) && slot_of(l.src) == s && slot_of(l.dst) != s) --indeg[slot_of(l.dst)];
    }
  }
  for (int s = 0; s < kSlots; ++s) if (!done[s]) slots[n++] = (uint8_t)s;
}

void process_slot(int s, const float* const* sout, float* const* out, float* const* sin, int frames) {
  int64_t now = __atomic_load_n(&s_frames, __ATOMIC_RELAXED);
  for (Live& l : s_live) {
    if (!l.on || !is_slot(l.src) || slot_of(l.src) != s || l.blk == s_blk) continue;
    l.blk = s_blk;
    run(l, Bufs{nullptr, sout, out, sin}, now, frames);
  }
}

void end_block(int frames) {
  int64_t now = __atomic_load_n(&s_frames, __ATOMIC_RELAXED);
  // points from slots that did not run this block (no sout given): their ramps still move on
  for (Live& l : s_live) {
    if (!l.on || l.blk == s_blk) continue;
    l.blk = s_blk;
    run(l, Bufs{nullptr, nullptr, nullptr, nullptr}, now, frames);
  }
  count_live();
  __atomic_store_n(&s_frames, now + frames, __ATOMIC_RELEASE);
}

void process(const float* const* in, float* const* out, int frames) {
  process_in(in, out, nullptr, frames);
  end_block(frames);
}

void close_slot(int s) {
  if (s < 0 || s >= kSlots) return;
  for (int i = 0; i < kPoints; ++i) {
    Point& p = s_pt[i];
    if (!p.open || !((is_slot(p.src) && slot_of(p.src) == s) || (is_slot(p.dst) && slot_of(p.dst) == s))) continue;
    if (!push({kClose, (uint8_t)i, 0, 0, 0, -1})) break;
    p.open = false;
    ++s_st.closed;
  }
  set_open_n(count_open());
}

bool slot_quiet(int s) {
  if (s < 0 || s >= kSlots) return true;
  for (const Point& p : s_pt)
    if (p.open && ((is_slot(p.src) && slot_of(p.src) == s) || (is_slot(p.dst) && slot_of(p.dst) == s))) return false;
  // the render side has taken every queued change and no live point touches it
  return __atomic_load_n(&s_rd, __ATOMIC_ACQUIRE) == __atomic_load_n(&s_wr, __ATOMIC_ACQUIRE) &&
         !(__atomic_load_n(&s_live_slots, __ATOMIC_ACQUIRE) & (1u << s));
}

Point point(int p) { return p >= 0 && p < kPoints ? s_pt[p] : Point{}; }
Stats stats() {
  Stats s = s_st;
  s.late = __atomic_load_n(&s_late, __ATOMIC_RELAXED);
  s.lead = s_lead;
  return s;
}

static const char* slot_name(int c) {
  static char names[kSlotChans][8];
  char* n = names[c - kChannels];
  if (!n[0]) {   // "S0 L" .. "S15 R"
    int s = slot_of(c), i = 0;
    n[i++] = 'S';
    if (s >= 10) n[i++] = (char)('0' + s / 10);
    n[i++] = (char)('0' + s % 10);
    n[i++] = ' ';
    n[i++] = (c - kChannels) & 1 ? 'R' : 'L';
    n[i] = 0;
  }
  return n;
}
const char* in_name(int c) {
  static const char* const n[kChannels] = {"MAIN L", "MAIN R", "BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB",
                                           "IN L", "IN R"};
  return c >= 0 && c < kChannels ? n[c] : is_slot(c) ? slot_name(c) : "?";
}
const char* out_name(int c) {
  static const char* const n[kChannels] = {"MAIN L", "MAIN R", "FX L", "FX R", "BD", "SD", "RS/CP", "BT", "LT", "MT/HT",
                                           "CH/OH", "CY/CB"};
  return c >= 0 && c < kChannels ? n[c] : is_slot(c) ? slot_name(c) : "?";
}

}  // namespace route
}  // namespace core

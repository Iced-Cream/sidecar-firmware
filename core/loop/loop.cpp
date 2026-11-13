// core/loop/loop.cpp - see loop.h. Control side (core 3): the grabs, the store, the state machine, one tick a control pass.
// Core 0: in_block (the ring) and out_mix (the playback), which take what to play from one published snapshot.
#include "loop/loop.h"

#include <math.h>
#include <string.h>

#include "lfo.h"
#include "platform.h"
#include "seq/seq.h"

namespace core {
namespace loop {

namespace {

// ------------------------------------------------------------------------------------------------ the environment
CORE_COLD bool def_beat_map(double* o, double* p, int* l) { return seq::beat_map(o, p, l); }
CORE_COLD uint64_t def_frames() { return seq::frames(); }
CORE_COLD uint16_t def_muted() { return seq::muted(); }
CORE_COLD bool def_clock(float* t, float* us, bool* run) {
  lfo::ClockState c;
  lfo::clock_state(&c);
  *run = c.run && c.ticking;
  return lfo::clock_now(t, us);
}
Env env_;
CORE_COLD bool beat_map(double* o, double* p, int* l) { return (env_.beat_map ? env_.beat_map : def_beat_map)(o, p, l); }
CORE_COLD uint64_t frames_now() { return (env_.frames ? env_.frames : def_frames)(); }
CORE_COLD uint16_t muted_now() { return (env_.muted ? env_.muted : def_muted)(); }

Ring ring_;
uint8_t* store_ = nullptr;
uint64_t store_bytes_ = 0;
bool on_ = false;
Stats stats_;

// ------------------------------------------------------------------------------------------------ the grabs
Grab g_[kMaxGrabs];   // 0 newest
uint64_t g_off_[kMaxGrabs];   // each one's place in the store (bytes)
uint64_t g_k0_[kMaxGrabs];    // its first frame's absolute ring index (the ring's source until copied)
int ng_ = 0;
uint32_t next_id_ = 1;

struct Pending {
  bool on;
  int half_bars;
  double E, grid;   // the grid line (index, frames) the grab ends on
  uint64_t press;
} pend_;
struct Copy {
  bool on;
  uint32_t id;
  uint64_t k;       // next ring index to copy
  uint32_t done, total;   // frames
} cp_;
int need_bpm_ = 47;

// a level that moves linearly over kRampFrames from where it was when core 3 set it (core 0 evaluates it at its frame)
constexpr int kRampFrames = 480;
struct Ramp {
  float from = 1, to = 1;
  uint64_t at = 0;
  float at_h(uint64_t h) const {
    if (h <= at) return from;
    uint64_t d = h - at;
    return d >= (uint64_t)kRampFrames ? to : from + (to - from) * (float)d / (float)kRampFrames;
  }
};
// what core 0 plays: a snapshot, rewritten whole (core 3), taken under a seqlock (core 0)
struct Snap {
  uint32_t loop_id;     // a new id is a new loop (a swap in at `start`, a crossfade when `replace`)
  bool active, replace, from_ring;
  const int32_t* pcm;   // frame 0 of the store's copy (kPre frames before it)
  uint64_t idx0;        // from_ring: frame 0's absolute ring index
  uint32_t len;
  uint64_t start;       // the host render frame of sample 0
  uint64_t stop_at;     // 0: none
  uint32_t stop_len;    // frames of the fade
  Ramp own, lp;         // the Sidecar's own output's gain (BAL), the loop's (LEV x BAL)
};
Snap snap_;             // core 3's working copy
Snap pub_;
uint32_t pub_seq_ = 0;  // odd while pub_ is written
uint32_t loop_ids_ = 0;
struct LoopInfo {
  bool on;
  uint32_t grab_id;
  uint64_t start;
  uint32_t len;
  uint64_t stop_at, stop_end;
  uint8_t half_bars;
} lp_;

CORE_COLD void publish() {
  __atomic_store_n(&pub_seq_, pub_seq_ + 1, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  pub_ = snap_;
  __atomic_store_n(&pub_seq_, pub_seq_ + 1, __ATOMIC_RELEASE);
}

CORE_COLD int find_id(uint32_t id) {
  for (int i = 0; i < ng_; ++i)
    if (g_[i].id == id) return i;
  return -1;
}
CORE_COLD int64_t bar_frames(double per16) { return (int64_t)(16.0 * per16 + 0.5); }

// the bars: the sequencer's beat map; without its 2C ticks the MIDI clock's (96 ticks a bar, less exact: spec 3.4)
struct Map {
  double origin, per16;   // origin: render frame of bar line 0, `lat` early; per16: frames of a 1/16
  int lat;
  bool midi;
};
CORE_COLD bool get_map(Map* m, uint64_t now) {
  m->midi = false;
  if (beat_map(&m->origin, &m->per16, &m->lat)) return true;
  float ticks, tick_us;
  bool run = true;
  if (!env_.clock && !env_.beat_map) {   // (the defaults: the lfo clock)
    if (!def_clock(&ticks, &tick_us, &run)) return false;
  } else if (env_.clock) {
    if (!env_.clock(&ticks, &tick_us, &run)) return false;
  } else {
    return false;
  }
  if (!run || tick_us < 1000.0f) return false;
  double per_tick = (double)tick_us * 0.048;
  m->per16 = 6.0 * per_tick;
  m->origin = (double)now - (double)ticks * per_tick - m->lat;   // (no latency early: the Rytm's own time)
  m->origin += m->lat;
  m->midi = true;
  return true;
}
// when bar line / grid line k's audio arrives in the ring
CORE_COLD double arrival(const Map& m, double k, double grid) { return m.origin + m.lat + k * grid; }

// ------------------------------------------------------------------------------------------------ the settings
int bal_ = 64, lev_ = 100, fade_ = kFade1, live_via_ = 0, mod_bal_ = -1, mod_lev_ = -1;
bool auto_mute_ = false;
CORE_COLD int bal_now() { return mod_bal_ >= 0 ? mod_bal_ : bal_; }
CORE_COLD int lev_now() { return mod_lev_ >= 0 ? mod_lev_ : lev_; }
// the levels as they play: core 3 moves each from where it is to the new value over 10 ms
CORE_COLD void retarget() {
  uint64_t now = frames_now();
  float og = own_gain(bal_now()), lg = loop_gain(bal_now()) * (float)lev_now() / 100.0f;
  snap_.own = {snap_.own.at_h(now), og, now};
  snap_.lp = {snap_.lp.at_h(now), lg, now};
  if (on_) publish();
}

// ------------------------------------------------------------------------------------------------ the store
CORE_COLD bool protected_id(uint32_t id) { return lp_.on && lp_.grab_id == id; }
CORE_COLD void drop_at(int i) {
  if (cp_.on && cp_.id == g_[i].id) cp_.on = false;
  for (int j = i; j + 1 < ng_; ++j) g_[j] = g_[j + 1], g_off_[j] = g_off_[j + 1], g_k0_[j] = g_k0_[j + 1];
  --ng_;
  ++stats_.dropped;
}
CORE_COLD bool drop_oldest() {   // the oldest that is not playing or copying
  for (int i = ng_ - 1; i >= 0; --i)
    if (!protected_id(g_[i].id) && !(cp_.on && cp_.id == g_[i].id)) {
      drop_at(i);
      return true;
    }
  return false;
}
CORE_COLD bool find_room(uint64_t bytes, uint64_t* off) {
  // first fit among the grabs' extents (by offset); false: no gap
  for (;;) {
    uint64_t pos = 0;
    for (;;) {
      uint64_t best = ~0ull;   // the nearest grab start at or after pos
      uint64_t end_of_overlap = pos;
      bool clash = false;
      for (int i = 0; i < ng_; ++i) {
        uint64_t a = g_off_[i], b = a + (uint64_t)(g_[i].frames + kPre) * 8;
        if (a < pos + bytes && b > pos) clash = true, end_of_overlap = b > end_of_overlap ? b : end_of_overlap;
        if (a >= pos && a < best) best = a;
      }
      (void)best;
      if (!clash) {
        if (pos + bytes <= store_bytes_) {
          *off = pos;
          return true;
        }
        break;
      }
      pos = (end_of_overlap + 15) & ~15ull;
    }
    if (!drop_oldest()) return false;
  }
}

// ------------------------------------------------------------------------------------------------ the grabs
CORE_COLD void make_loop(int gi, uint64_t start, bool replace) {
  const Grab& g = g_[gi];
  snap_.loop_id = ++loop_ids_;
  snap_.active = true, snap_.replace = replace;
  snap_.from_ring = !g.copied;
  snap_.pcm = g.copied ? g.pcm : nullptr;
  snap_.idx0 = g_k0_[gi];
  snap_.len = g.frames;
  snap_.start = start;
  snap_.stop_at = 0, snap_.stop_len = 0;
  lp_ = {true, g.id, start, g.frames, 0, 0, g.half_bars};
  publish();
}

// the render frame of the first grid line after `now` that is far enough ahead for the audio thread to take the command
CORE_COLD uint64_t next_line(const Map& m, double grid, uint64_t now) {
  double k = floor(((double)now + 96.0 - m.origin) / grid) + 1.0;
  return (uint64_t)(m.origin + k * grid);
}

CORE_COLD void finalize(uint64_t now) {
  Map m;
  if (!get_map(&m, now)) {
    pend_.on = false;
    ++stats_.refused;
    return;
  }
  int hb = pend_.half_bars;
  double g = pend_.grid;
  int64_t len = (int64_t)(hb * 8 * m.per16 + 0.5);
  double a_e = arrival(m, pend_.E, g);
  uint64_t start_h = (uint64_t)(a_e - (double)len);
  uint64_t k0;
  if (!ring_.index_at(start_h, &k0) || k0 < (uint64_t)kPre) {
    pend_.on = false;
    ++stats_.refused;
    return;
  }
  uint64_t bytes = (uint64_t)(len + kPre) * 8;
  uint64_t off;
  if (ng_ >= kMaxGrabs && !drop_oldest()) {
    pend_.on = false;
    ++stats_.refused;
    return;
  }
  if (!find_room(bytes, &off)) {
    pend_.on = false;
    ++stats_.refused;
    return;
  }
  for (int j = ng_; j > 0; --j) g_[j] = g_[j - 1], g_off_[j] = g_off_[j - 1], g_k0_[j] = g_k0_[j - 1];
  ++ng_;
  Grab& n = g_[0];
  n = Grab{};
  n.id = next_id_++;
  n.frames = (uint32_t)len;
  n.half_bars = (uint8_t)hb;
  n.bpm = (float)(60.0 * 48000.0 / (4.0 * m.per16));
  n.start = start_h;
  n.gap = ring_.gap_in(start_h, (uint32_t)len) ? 1 : 0;
  g_off_[0] = off;
  g_k0_[0] = k0;
  cp_ = {true, n.id, k0 - (uint64_t)kPre, 0, (uint32_t)(len + kPre)};
  ++stats_.grabs;
  bool replace = lp_.on && (lp_.stop_at == 0 || now < lp_.stop_end);
  if (replace) ++stats_.resamples;
  // the loop starts at the next grid line (bar line for every length but the half bar's grid)
  make_loop(0, next_line(m, g, now), replace);
  pend_.on = false;
}

CORE_COLD void pump_pending(uint64_t now) {
  if (!pend_.on || cp_.on) return;
  Map m;
  if (!get_map(&m, now)) {
    pend_.on = false;
    ++stats_.refused;
    return;
  }
  double a_e = arrival(m, pend_.E, pend_.grid);
  uint64_t k;
  if ((double)now < a_e) return;
  // the ring has every frame up to that line's: its index is written
  int64_t offs;
  if (!ring_.offset(&offs)) return;
  k = (uint64_t)((int64_t)a_e - offs);
  if (ring_.written() < k) return;
  finalize(now);
}

CORE_COLD void pump_copy() {
  if (!cp_.on) return;
  int gi = find_id(cp_.id);
  if (gi < 0) {
    cp_.on = false;
    return;
  }
  uint32_t n = cp_.total - cp_.done;
  if (n > kPieceBytes / 8) n = kPieceBytes / 8;
  uint64_t w = ring_.written();
  bool lost = cp_.k >= w || w - cp_.k + n > ring_.capacity();   // the writer would be on the frames we copy
  int32_t* dst = (int32_t*)(store_ + g_off_[gi]) + 2 * (size_t)cp_.done;
  uint32_t got = lost ? 0 : ring_.copy_idx(cp_.k, n, dst);
  if (got != n) {   // overtaken: abandoned, the grab refused; a loop playing from it goes (cut)
    ++stats_.abandoned;
    bool was_playing = protected_id(cp_.id);
    cp_.on = false;
    if (was_playing) {
      snap_.active = false;
      publish();
      lp_.on = false;
    }
    int i = find_id(cp_.id);
    if (i >= 0) {
      for (int j = i; j + 1 < ng_; ++j) g_[j] = g_[j + 1], g_off_[j] = g_off_[j + 1], g_k0_[j] = g_k0_[j + 1];
      --ng_;
    }
    return;
  }
  if (n * 8 > stats_.worst_copy_bytes) stats_.worst_copy_bytes = n * 8;
  cp_.k += n, cp_.done += n;
  if (cp_.done >= cp_.total) {
    Grab& g = g_[gi];
    g.pcm = (int32_t*)(store_ + g_off_[gi]) + 2 * kPre;
    g.copied = 1;
    cp_.on = false;
    ++stats_.copies;
    if (protected_id(g.id) && snap_.from_ring && snap_.active) {   // the loop goes on from the store, the same frames
      snap_.from_ring = false;
      snap_.pcm = g.pcm;
      publish();
    }
  }
}

}  // namespace

// ------------------------------------------------------------------------------------------------ the public side
CORE_COLD void set_env(const Env* e) { env_ = e ? *e : Env(); }

CORE_COLD void init(int32_t* ring, uint32_t ring_frames, uint8_t* store, uint64_t store_bytes) {
  on_ = ring && ring_frames && store && store_bytes;
  store_ = store, store_bytes_ = store_bytes;
  if (ring && ring_frames) ring_.init(ring, ring_frames);
  seq::set_clock_tracks(seq::kClockLoop, on_ ? 1 : 0);   // BD's ticks: the looper's bars (spec 3.4)
  reset();
}

CORE_COLD void reset() {
  ng_ = 0, next_id_ = 1, pend_ = Pending(), cp_ = Copy(), stats_ = Stats(), lp_ = LoopInfo();
  snap_ = Snap();
  bal_ = 64, lev_ = 100, fade_ = kFade1, live_via_ = 0, mod_bal_ = mod_lev_ = -1, auto_mute_ = false;
  if (on_) publish();
  if (ring_.ready()) ring_.init(ring_.data(), ring_.capacity());
}

CORE_COLD bool on() { return on_; }
bool wants_in() { return on_; }

CORE_COLD int grab(int half_bars) {
  if (!on_) return kErrOff;
  if (half_bars != kHalf && half_bars != k1 && half_bars != k2 && half_bars != k4 && half_bars != k8 && half_bars != k16)
    return kErrBadArg;
  uint64_t now = frames_now();
  Map m;
  if (!get_map(&m, now)) {
    ++stats_.refused;
    return kErrStopped;
  }
  double grid = half_bars == kHalf ? 8 * m.per16 : 16 * m.per16;
  double E = floor(((double)now - m.origin) / grid);   // the last grid line (render-domain); its audio comes lat later
  if (E < 0) {
    ++stats_.refused;
    return kErrStopped;
  }
  double len = (double)half_bars * 8 * m.per16;
  double a_e = arrival(m, E, grid);
  double at = (double)now > a_e ? (double)now : a_e;   // a press that waits: measured from the line's arrival
  // spec 3.3: it must fit with what has been written since its first frame: the grab, the part of the grid in progress,
  // the round trip, the copy's time (a 256 KB piece a control pass, taken as 0.1 ms) and a 0.5 s margin
  double cap = (double)ring_.capacity();
  double copy = (len + kPre) * 8 / (double)kPieceBytes * 4.8;   // frames of the copy (a piece = 4.8 frames of 0.1 ms)
  if (at - (a_e - len) + m.lat + copy + 24000.0 > cap) {
    // the BPM a grab of this many bars needs at the worst point of the bar (a whole bar in progress)
    double bars = (double)half_bars / 2.0, room = cap - m.lat - 24000.0 - copy;
    int need = (int)ceil(240.0 * 48000.0 * (bars + 1.0) / room);   // 4 beats a bar: bpm = 240 s x (bars + 1) / (room / 48000)
    need_bpm_ = need;
    ++stats_.refused;
    return kErrTooLong;
  }
  uint64_t k;
  if (!ring_.index_at((uint64_t)(a_e - len - kPre) > 0 ? (uint64_t)(a_e - len - kPre) : 0, &k)) {
    ++stats_.refused;
    return kErrNoRoom;   // (the ring has not got that far back: just started)
  }
  uint64_t bytes = (uint64_t)((len + kPre) * 8);
  if (bytes > store_bytes_) {
    ++stats_.refused;
    return kErrNoRoom;
  }
  pend_ = {true, half_bars, E, grid, now};
  pump_pending(now);
  return kOk;
}

CORE_COLD const char* err_text(int err, int half_bars, char* b, int cap) {
  switch (err) {
    case kErrStopped: sniprintf(b, cap, "PLAY FIRST"); break;
    case kErrTooLong: sniprintf(b, cap, "%d BARS NEEDS %d BPM", half_bars / 2, need_bpm_); break;
    case kErrNoRoom: sniprintf(b, cap, "NO ROOM"); break;
    case kErrOff: sniprintf(b, cap, "LOOPER OFF"); break;
    default: sniprintf(b, cap, "BAD LENGTH"); break;
  }
  return b;
}

CORE_COLD void tick(uint32_t us) {
  (void)us;
  if (!on_) return;
  uint64_t now = frames_now();
  pump_pending(now);
  pump_copy();
  if (lp_.on && lp_.stop_at && now >= lp_.stop_end) lp_.on = false;   // faded out: the ring alone
}

CORE_COLD int grabs() { return ng_; }
CORE_COLD const Grab* grab_at(int i) { return i >= 0 && i < ng_ ? &g_[i] : nullptr; }
CORE_COLD bool pending() { return pend_.on; }
CORE_COLD const Stats& stats() { return stats_; }

CORE_COLD State state() {
  if (!lp_.on) return kRing;
  uint64_t now = frames_now();
  if (lp_.stop_at) return now >= lp_.stop_end ? kRing : kFading;
  return now < lp_.start ? kWait : kPlaying;
}

// ------------------------------------------------------------------------------------------------ settings, levels, stop
CORE_COLD int clamp127(int v) { return v < 0 ? 0 : v > 127 ? 127 : v; }
CORE_COLD void set_bal(int v) { bal_ = clamp127(v), retarget(); }
CORE_COLD void set_lev(int v) { lev_ = clamp127(v), retarget(); }
CORE_COLD void set_fade(int f) { fade_ = f < 0 ? 0 : f >= kFades ? kFades - 1 : f; }
CORE_COLD void set_auto_mute(bool on) { auto_mute_ = on; }
CORE_COLD void set_live_via(int perf) { live_via_ = perf < 0 ? 0 : perf > 12 ? 12 : perf; }
CORE_COLD int bal() { return bal_; }
CORE_COLD int lev() { return lev_; }
CORE_COLD int fade() { return fade_; }
CORE_COLD bool auto_mute() { return auto_mute_; }
CORE_COLD int live_via() { return live_via_; }
CORE_COLD void set_bal_mod(int v) { mod_bal_ = v < 0 ? -1 : clamp127(v), retarget(); }
CORE_COLD void set_lev_mod(int v) { mod_lev_ = v < 0 ? -1 : clamp127(v), retarget(); }
CORE_COLD int bal_eff() { return bal_now(); }
CORE_COLD int lev_eff() { return lev_now(); }

// spec 3.8: 0..64 the loop rises from 0 to full (equal power: sin), 64..127 the Sidecar's own output falls from full
// to 0 (cos); the other side full
CORE_COLD float loop_gain(int b) { return b >= 64 ? 1.0f : sinf(1.5707963f * (float)b / 64.0f); }
CORE_COLD float own_gain(int b) { return b <= 64 ? 1.0f : b >= 127 ? 0.0f : cosf(1.5707963f * (float)(b - 64) / 63.0f); }
CORE_COLD int live_perf(int b) { return b <= 64 ? 127 : 127 - (b - 64) * 127 / 63; }

// the FADE setting in bars: 1/4 1/2 1 2 4
CORE_COLD double fade_bars() {
  static const double kB[kFades] = {0.25, 0.5, 1, 2, 4};
  return kB[fade_];
}

CORE_COLD void stop(bool cut, bool now) {
  if (!on_ || !lp_.on) return;
  uint64_t t = frames_now();
  Map m;
  bool have = get_map(&m, t);
  uint64_t at = t + 96;   // (far enough ahead for the audio thread to take it)
  if (!now && have) at = next_line(m, 16.0 * m.per16, t);
  uint32_t len = cut || !have ? (uint32_t)kCutFrames : (uint32_t)(fade_bars() * 16.0 * m.per16 + 0.5);
  if (len < (uint32_t)kCutFrames) len = kCutFrames;
  if (at <= lp_.start) {   // the loop has not started: it never will
    snap_.active = false;
    publish();
    lp_.on = false;
    return;
  }
  snap_.stop_at = at, snap_.stop_len = len;
  lp_.stop_at = at, lp_.stop_end = at + len;
  publish();
}

// ------------------------------------------------------------------------------------------------ core 0
void in_block(const int32_t* lr, int n, uint64_t arrival_frame, unsigned lost) {
  if (ring_.ready()) ring_.write(lr, n, arrival_frame, lost);
}

namespace {
struct Voice {
  bool active;
  uint32_t id;
  bool from_ring;
  const int32_t* pcm;
  uint64_t idx0;
  uint32_t len;
  uint64_t start, stop_at;
  uint32_t stop_len;
  bool fade_in;
};
Voice cur_, old_;
Ramp ramp_own_, ramp_loop_;
uint32_t seen_seq_ = 0;

inline void vframe(const Voice& v, int64_t k, float* l, float* r) {
  if (v.from_ring) {
    int32_t a, b;
    ring_.frame(v.idx0 + (uint64_t)k, &a, &b);
    *l = (float)a, *r = (float)b;
  } else {
    *l = (float)v.pcm[2 * k], *r = (float)v.pcm[2 * k + 1];
  }
}
// the voice's frame at host frame h (with its seam and its own stop's fade); false: silent
inline bool vat(const Voice& v, uint64_t h, float* l, float* r) {
  if (!v.active || h < v.start) return false;
  float g = 1.0f;
  if (v.stop_at && h >= v.stop_at) {
    uint64_t d = h - v.stop_at;
    if (d >= v.stop_len) return false;
    g = 1.0f - (float)(d + 1) / (float)v.stop_len;
  }
  uint64_t p = (h - v.start) % v.len;
  float a, b;
  vframe(v, (int64_t)p, &a, &b);
  if (p + kSeam >= v.len) {   // the seam: the end fades into the frames that lead into the start
    int j = (int)(p + kSeam - v.len);
    float w = (float)(j + 1) / kSeam, c, d;
    vframe(v, -kSeam + j, &c, &d);
    a = a + (c - a) * w, b = b + (d - b) * w;
  }
  *l = a * g, *r = b * g;
  return true;
}
inline int32_t sat(float x) { return x >= 2147483520.0f ? 2147483647 : x <= -2147483648.0f ? INT32_MIN : (int32_t)x; }
inline int32_t add(int32_t a, float x) {
  int64_t s = (int64_t)a + (int64_t)x;
  return s > 2147483647 ? 2147483647 : s < INT32_MIN ? INT32_MIN : (int32_t)s;
}

void take_snapshot(uint64_t h0) {
  uint32_t s0 = __atomic_load_n(&pub_seq_, __ATOMIC_ACQUIRE);
  if (s0 == seen_seq_ || (s0 & 1)) return;
  Snap s = pub_;
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if (__atomic_load_n(&pub_seq_, __ATOMIC_RELAXED) != s0) return;   // rewritten meanwhile: the next block
  seen_seq_ = s0;
  ramp_own_ = s.own, ramp_loop_ = s.lp;
  if (!s.active) {   // gone (an abandoned copy): a cut from here
    if (cur_.active && !(cur_.stop_at && cur_.stop_at <= h0)) cur_.stop_at = h0, cur_.stop_len = kCutFrames;
    return;
  }
  if (s.loop_id != cur_.id) {   // a new loop
    bool xf = s.replace && cur_.active;
    if (xf) old_ = cur_;
    else old_.active = false;
    cur_ = Voice{true, s.loop_id, s.from_ring, s.pcm, s.idx0, s.len, s.start, s.stop_at, s.stop_len, !xf};
  } else {   // the same loop: its source (the store's copy now) and its stop
    cur_.from_ring = s.from_ring, cur_.pcm = s.pcm;
    cur_.stop_at = s.stop_at, cur_.stop_len = s.stop_len;
  }
}
}  // namespace

bool wants_out() { return on_; }

void out_mix(int32_t (*F)[12], int n, uint64_t render_frame) {
  if (!on_) return;
  take_snapshot(render_frame);
  if (!cur_.active && !old_.active) return;
  for (int f = 0; f < n; ++f) {
    uint64_t h = render_frame + (uint64_t)f;
    float l = 0, r = 0, a, b;
    bool any = false;
    if (vat(cur_, h, &a, &b)) {
      float w = h - cur_.start < (uint64_t)kSeam ? (float)(h - cur_.start + 1) / kSeam : 1.0f;   // a start fades in; a swap crossfades
      l += a * w, r += b * w;
      any = true;
    }
    if (old_.active) {
      float c, d;
      if (h < cur_.start + kSeam && vat(old_, h, &c, &d)) {
        float w = h < cur_.start ? 1.0f : 1.0f - (float)(h - cur_.start + 1) / kSeam;
        l += c * w, r += d * w;
        any = true;
      } else if (h >= cur_.start + kSeam) {
        old_.active = false;
      }
    }
    if (!any) continue;
    // BAL: the Sidecar's own output (every channel of F so far) falls toward `own` as the loop is heard (spec 3.8)
    float og = ramp_own_.at_h(h);
    if (og < 0.99999f) {
      float e = h >= cur_.start ? (float)(h - cur_.start + 1) / kRampFrames : 0.0f;
      if (e > 1.0f) e = 1.0f;
      if (cur_.stop_at && h >= cur_.stop_at) {
        float d = (float)(h - cur_.stop_at + 1) / (float)cur_.stop_len;
        e *= d >= 1.0f ? 0.0f : 1.0f - d;
      }
      float m = 1.0f + (og - 1.0f) * e;
      for (int c = 0; c < 12; ++c) F[f][c] = (int32_t)((float)F[f][c] * m);
    }
    float lg = ramp_loop_.at_h(h);
    F[f][0] = add(F[f][0], l * lg), F[f][1] = add(F[f][1], r * lg);
  }
  // a stopped voice that has faded out is gone (its stop's end passed)
  if (cur_.active && cur_.stop_at && render_frame + (uint64_t)n >= cur_.stop_at + cur_.stop_len) cur_.active = false;
}

}  // namespace loop
}  // namespace core

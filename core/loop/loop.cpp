// core/loop/loop.cpp - see loop.h. Control side (core 3): the grabs, the store, the state machine, one tick a control pass.
// Core 0: in_block (the ring) and out_mix (the playback), which take what to play from one published snapshot.
#include "loop/loop.h"

#include <math.h>
#include <string.h>

#include "fmt/fmt.h"
#include "lfo.h"
#include "mod/mod.h"
#include "mod/rytm_out.h"
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

// auto-mute (spec 3.7): the tracks the looper muted at the loop's start; `seen`: those the Rytm has reported muted since, so
// a later report without one is the user's doing and the track is theirs
struct AutoMute {
  bool on;
  uint16_t set, seen;
} am_;
bool dirty_ = false;     // a setting changed (loop.txt)
int post_lat_ = 0;
fmt::KvKeep keep_lines_;   // the lines of loop.txt this build does not know, written back as they were
char msg_[40];
uint32_t msg_gen_ = 0;
CORE_COLD void say(const char* t) { sniprintf(msg_, sizeof msg_, "%s", t), ++msg_gen_; }
// KEEP: 0 none, 1 asked, 2 core 0 is writing it
uint8_t keep_state_ = 0;
uint32_t keep_id_ = 0;
// calibration (spec 3.4): kCalN clicks kCalGap frames apart from kCalAt; core 0 renders them and listens
constexpr int kCalN = 3, kCalGap = 9600, kCalClick = 24, kCalWindow = 4800;
uint64_t cal_at_ = 0;      // the first click's render frame (0: none)
int32_t cal_found_[kCalN];   // the round trip found for each click (frames), -1 not yet
int32_t cal_floor_ = 0;
uint8_t cal_state_ = 0;    // 0 idle, 1 clicks on their way
bool via_on_ = false;   // LIVE VIA has set a macro that is not back at full yet
int via_macro_ = 0;

// ------------------------------------------------------------------------------------------------ the store
CORE_COLD bool protected_id(uint32_t id) { return (lp_.on && lp_.grab_id == id) || (keep_state_ && keep_id_ == id); }
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
  ng_ = 0, next_id_ = 1, pend_ = Pending(), cp_ = Copy(), stats_ = Stats(), lp_ = LoopInfo(), am_ = AutoMute(), via_on_ = false;
  snap_ = Snap();
  bal_ = 64, lev_ = 100, fade_ = kFade1, live_via_ = 0, mod_bal_ = mod_lev_ = -1, auto_mute_ = false;
  dirty_ = false, post_lat_ = 0, keep_state_ = 0, cal_state_ = 0, cal_at_ = 0, msg_[0] = 0, msg_gen_ = 0, keep_lines_.n = 0;
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

// auto-mute and LIVE VIA follow the loop: on at its start, off at the start of its fade back (or cut)
CORE_COLD void live_and_mutes(uint64_t now) {
  bool audible = lp_.on && now >= lp_.start && !(lp_.stop_at && now >= lp_.stop_at);
  if (audible && auto_mute_ && !am_.on) {
    uint16_t before = muted_now();
    am_ = {true, (uint16_t)(~before & 0xFFF), 0};
    if (am_.set) mod::rytm_mute(am_.set, 0);
  }
  if (am_.on) {
    uint16_t cur = muted_now();
    am_.seen |= am_.set & cur;
    uint16_t dropped = am_.seen & ~cur;   // a mute we made and the Rytm showed, now gone: the user's
    am_.set &= ~dropped, am_.seen &= ~dropped;
    if (!audible) {
      if (am_.set) mod::rytm_mute(0, am_.set);
      am_ = AutoMute();
    }
  }
  if (live_via_ && audible) {
    via_macro_ = live_via_, via_on_ = true;
    mod::rytm_perf(live_via_, live_perf(bal_now()));
  } else if (via_on_ && !audible) {   // the loop is leaving: the live comes back at once
    mod::rytm_perf(via_macro_, 127);
    via_on_ = false;
  }
}

namespace {
CORE_COLD void cal_tick(uint64_t now);
}
CORE_COLD void tick(uint32_t us) {
  (void)us;
  if (!on_) return;
  seq::set_clock_tracks(seq::kClockLoop, 1);   // (seq::init in engine_begin resets the users: asked again each pass, a store)
  uint64_t now = frames_now();
  pump_pending(now);
  pump_copy();
  if (lp_.on && lp_.stop_at && now >= lp_.stop_end) lp_.on = false;   // faded out: the ring alone
  live_and_mutes(now);
  cal_tick(now);
}

CORE_COLD bool play(int g) {
  if (!on_ || g < 0 || g >= ng_) return false;
  uint64_t now = frames_now();
  Map m;
  if (!get_map(&m, now)) return false;
  double grid = g_[g].half_bars == kHalf ? 8 * m.per16 : 16 * m.per16;
  bool replace = lp_.on && (lp_.stop_at == 0 || now < lp_.stop_end);
  make_loop(g, next_line(m, grid, now), replace);
  return true;
}
CORE_COLD int playing() {
  if (!lp_.on) return -1;
  int i = find_id(lp_.grab_id);
  return i;
}
CORE_COLD bool playing_info(uint64_t* start, uint32_t* len) {
  if (!lp_.on) return false;
  *start = lp_.start, *len = lp_.len;
  return true;
}
CORE_COLD bool bar_position(float* bar, float* of_bars) {
  uint64_t now = frames_now();
  if (!lp_.on || now < lp_.start || lp_.len == 0) return false;
  Map m;
  double b = lp_.half_bars / 2.0;
  *of_bars = (float)b;
  *bar = (float)(((now - lp_.start) % lp_.len) / (double)lp_.len * b);
  (void)m;
  return true;
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
CORE_COLD void set_bal(int v) { bal_ = clamp127(v), dirty_ = true, retarget(); }
CORE_COLD void set_lev(int v) { lev_ = clamp127(v), dirty_ = true, retarget(); }
CORE_COLD void set_fade(int f) { fade_ = f < 0 ? 0 : f >= kFades ? kFades - 1 : f, dirty_ = true; }
CORE_COLD void set_auto_mute(bool on) { auto_mute_ = on, dirty_ = true; }
CORE_COLD void set_live_via(int perf) { live_via_ = perf < 0 ? 0 : perf > 12 ? 12 : perf, dirty_ = true; }
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

// ------------------------------------------------------------------------------------------------ calibration
CORE_COLD void cal_start() {
  if (!on_) return;
  for (int i = 0; i < kCalN; ++i) cal_found_[i] = -1;
  cal_floor_ = 0;
  cal_state_ = 1;
  __atomic_store_n(&cal_at_, frames_now() + 14400, __ATOMIC_RELEASE);   // 300 ms: the floor is measured first
}
CORE_COLD bool cal_wanted() { return cal_state_ == 1; }
CORE_COLD bool cal_info(uint64_t* first, int* n, uint32_t* gap) {
  uint64_t a = __atomic_load_n(&cal_at_, __ATOMIC_ACQUIRE);
  if (!a) return false;
  *first = a, *n = kCalN, *gap = kCalGap;
  return true;
}
CORE_COLD int post_lat() { return post_lat_; }
CORE_COLD void set_post_lat(int f) { post_lat_ = f, dirty_ = true; }
namespace {
CORE_COLD void cal_tick(uint64_t now) {
  if (cal_state_ != 1) return;
  uint64_t a = __atomic_load_n(&cal_at_, __ATOMIC_ACQUIRE);
  if (now < a + (uint64_t)kCalN * kCalGap + kCalWindow) return;
  int v[kCalN], n = 0;
  for (int i = 0; i < kCalN; ++i) {
    int32_t f = __atomic_load_n(&cal_found_[i], __ATOMIC_ACQUIRE);
    if (f >= 0) v[n++] = f;
  }
  cal_state_ = 0;
  __atomic_store_n(&cal_at_, 0, __ATOMIC_RELEASE);
  if (n < 2) {
    ++stats_.cal_fail;
    say("CAL: NO CLICK HEARD");
    return;
  }
  for (int i = 1; i < n; ++i)   // the median
    for (int j = i; j > 0 && v[j] < v[j - 1]; --j) { int t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
  double o, p16;
  int lat = 0;
  beat_map(&o, &p16, &lat);
  post_lat_ = v[n / 2] - lat;
  dirty_ = true;
  ++stats_.cal_ok;
  char b[40];
  sniprintf(b, sizeof b, "POST LAT %d", post_lat_);
  say(b);
}
}  // namespace

// ------------------------------------------------------------------------------------------------ KEEP
CORE_COLD bool keep(int g, char* msg, int cap) {
  const Grab* gr = grab_at(g);
  if (!on_ || !gr) {
    sniprintf(msg, cap, "NO GRAB");
    return false;
  }
  if (gr->kept) {
    sniprintf(msg, cap, "ALREADY KEPT");
    return false;
  }
  if (!gr->copied) {
    sniprintf(msg, cap, "COPYING: WAIT");
    return false;
  }
  if (keep_state_) {
    sniprintf(msg, cap, "KEEP BUSY");
    return false;
  }
  keep_id_ = gr->id, keep_state_ = 1;
  sniprintf(msg, cap, "KEEPING");
  return true;
}
CORE_COLD bool keep_take(KeepJob* j) {   // (core 0)
  if (__atomic_load_n(&keep_state_, __ATOMIC_ACQUIRE) != 1) return false;
  int i = find_id(keep_id_);
  if (i < 0 || !g_[i].copied) {
    keep_state_ = 0;
    return false;
  }
  *j = KeepJob{g_[i].id, g_[i].frames, g_[i].pcm, g_[i].bpm, g_[i].half_bars, g_[i].start};
  __atomic_store_n(&keep_state_, 2, __ATOMIC_RELEASE);
  return true;
}
CORE_COLD void keep_finished(uint32_t id, bool ok, const char* name) {
  int i = find_id(id);
  if (ok && i >= 0) g_[i].kept = 1, ++stats_.kept;
  char b[40];
  if (ok) sniprintf(b, sizeof b, "KEPT %s", name ? name : "");
  else sniprintf(b, sizeof b, "KEEP FAILED");
  say(b);
  __atomic_store_n(&keep_state_, 0, __ATOMIC_RELEASE);
}

// ------------------------------------------------------------------------------------------------ the card (loop.txt)
namespace {
const char kFile[] = "/.rytm/loop.txt";
}
CORE_COLD bool dirty() { return dirty_; }
CORE_COLD void load() {
  keep_lines_.n = 0;
  fmt::post_wait(kFile);
  fmt::recover(kFile);
  File f;
  if (!f.open(kFile, kRead)) return;
  static char b[512];
  memset(b, 0, sizeof b);
  int got = f.read(b, sizeof b - 1);
  f.close();
  static const char* const kKnown[] = {"bal", "lev", "fade", "mute", "live", "postlat", nullptr};
  if (got > 0) fmt::kv_keep(b, (uint32_t)got, kKnown, &keep_lines_);
  auto num = [&](const char* key, int* out) {   // parsed by hand: the Pi's bare-metal build has no strtol
    size_t kl = strlen(key);
    for (const char* l = b; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : nullptr) {
      if (strncmp(l, key, kl) || l[kl] != '=') continue;
      const char* e = l + kl + 1;
      bool neg = *e == '-';
      if (neg) e++;
      const char* d = e;
      int v = 0;
      while (*e >= '0' && *e <= '9' && v < 100000) v = v * 10 + (*e++ - '0');
      if (e != d && (*e == 0 || *e == '\n' || *e == '\r')) {
        *out = neg ? -v : v;
        return true;
      }
    }
    return false;
  };
  int v;
  if (num("bal", &v)) bal_ = clamp127(v);
  if (num("lev", &v)) lev_ = clamp127(v);
  if (num("fade", &v)) fade_ = v < 0 ? 0 : v >= kFades ? kFades - 1 : v;
  if (num("mute", &v)) auto_mute_ = v != 0;
  if (num("live", &v)) live_via_ = v < 0 ? 0 : v > 12 ? 12 : v;
  if (num("postlat", &v)) post_lat_ = v < -9999 ? -9999 : v > 9999 ? 9999 : v;
  dirty_ = false;
  retarget();
}
CORE_COLD bool save() {
  if (!dirty_) return false;
  char* t = fmt::post_begin();
  if (!t) return false;   // another settings write is in flight: at a later pass (still dirty)
  uint32_t n = (uint32_t)sniprintf(t, 128, CORE_STR("bal=%d\nlev=%d\nfade=%d\nmute=%d\nlive=%d\npostlat=%d\n"), bal_, lev_, fade_,
                                   auto_mute_ ? 1 : 0, live_via_, post_lat_);
  n = fmt::kv_append(keep_lines_, t, n, fmt::kPostMax);
  if (!fmt::post_end(kFile, n)) return false;
  dirty_ = false;
  return true;
}

// ------------------------------------------------------------------------------------------------ console and status
CORE_COLD const char* last_message() { return msg_; }
CORE_COLD uint32_t message_gen() { return msg_gen_; }
CORE_COLD int ring_seconds() {
  uint64_t w = ring_.written();
  uint64_t f = w < ring_.capacity() ? w : ring_.capacity();
  return (int)(f / 48000);
}
namespace {
const char* const kStateName[] = {"ring", "wait", "playing", "fading"};
const char* const kFadeName[kFades] = {"1/4", "1/2", "1", "2", "4"};
CORE_COLD const char* len_name(int hb, char* b, int cap) {
  if (hb == 1) sniprintf(b, cap, "1/2");
  else sniprintf(b, cap, "%d", hb / 2);
  return b;
}
CORE_COLD int parse_len(const char* s) {   // "0.5" "1/2" "1" .. "16" -> half bars (0 bad)
  if (!strcmp(s, "0.5") || !strcmp(s, "1/2")) return kHalf;
  int v = 0;
  const char* e = s;
  while (*e >= '0' && *e <= '9') v = v * 10 + (*e++ - '0');
  if (*e || e == s) return 0;
  return v == 1 || v == 2 || v == 4 || v == 8 || v == 16 ? v * 2 : 0;
}
CORE_COLD bool parse_int(const char* s, int* v) {
  bool neg = *s == '-';
  if (neg) ++s;
  const char* d = s;
  int x = 0;
  while (*s >= '0' && *s <= '9' && x < 100000) x = x * 10 + (*s++ - '0');
  if (s == d || *s) return false;
  *v = neg ? -x : x;
  return true;
}
}  // namespace

CORE_COLD bool command(const char* p, char* o, int cap) {
  if (strncmp(p, "loop", 4) != 0 || (p[4] && p[4] != '=')) return false;
  if (cap <= 0) return true;
  o[0] = 0;
  if (!on_) {
    sniprintf(o, cap, "loop: off (loopram=0, or no RAM for it)");
    return true;
  }
  if (!p[4]) {
    char gl[96] = "";
    int n = 0;
    for (int i = 0; i < ng_ && n < (int)sizeof gl - 16; ++i) {
      char l[8];
      n += sniprintf(gl + n, sizeof gl - n, "%s%s%s%s", i ? " " : "", len_name(g_[i].half_bars, l, sizeof l), g_[i].kept ? "k" : "", g_[i].copied ? "" : "~");
    }
    float bar, of;
    char pos[24] = "";
    if (bar_position(&bar, &of)) sniprintf(pos, sizeof pos, " pos %.2f/%.0f", bar, of);
    sniprintf(o, cap, "loop: %s, ring %d s, %d grabs [%s]%s, bal %d lev %d fade %s mute %s live %s postlat %d", kStateName[state()],
              ring_seconds(), ng_, gl, pos, bal_, lev_, kFadeName[fade_], auto_mute_ ? "on" : "off",
              live_via_ ? "perf" : "off", post_lat_);
    if (live_via_) {
      int k = (int)strlen(o);
      sniprintf(o + k - 3, cap - k + 3, "perf%d postlat %d", live_via_, post_lat_);
    }
    return true;
  }
  const char* a = p + 5;
  char b[48];
  int v;
  if (!strncmp(a, "grab:", 5)) {
    int hb = parse_len(a + 5);
    if (!hb) sniprintf(o, cap, "loop=grab:L (L 0.5 1 2 4 8 16 bars)");
    else {
      int e = grab(hb);
      if (e) say(err_text(e, hb, b, sizeof b)), sniprintf(o, cap, "loop: %s", b);
      else sniprintf(o, cap, "loop: grab %s bars%s", len_name(hb, b, sizeof b), pend_.on ? " (waiting for the bar line's audio)" : "");
    }
  } else if (!strncmp(a, "play:", 5)) {
    int g;
    if (!parse_int(a + 5, &g) || !play(g)) say("NO GRAB / PLAY FIRST"), sniprintf(o, cap, "loop: no grab %s, or the transport is stopped", a + 5);
    else sniprintf(o, cap, "loop: play grab %d from the next bar line", g);
  } else if (!strncmp(a, "stop", 4)) {
    bool cut = false, now = false;
    for (const char* w = a + 4; *w == ':';) {
      ++w;
      if (!strncmp(w, "cut", 3)) cut = true, w += 3;
      else if (!strncmp(w, "fade", 4)) cut = false, w += 4;
      else if (!strncmp(w, "now", 3)) now = true, w += 3;
      else break;
    }
    stop(cut, now);
    sniprintf(o, cap, "loop: stop (%s, %s)", cut ? "cut" : "fade", now ? "at once" : "at the next bar line");
  } else if (!strncmp(a, "keep:", 5)) {
    int g;
    if (!parse_int(a + 5, &g)) sniprintf(o, cap, "loop=keep:N (N 0 newest)");
    else {
      keep(g, b, sizeof b);
      say(b);
      sniprintf(o, cap, "loop: %s", b);
    }
  } else if (!strncmp(a, "bal:", 4) || !strncmp(a, "lev:", 4)) {
    if (!parse_int(a + 4, &v)) sniprintf(o, cap, "loop=%.3s:0..127", a);
    else {
      bool isbal = a[0] == 'b';
      v = clamp127(v);
      mod::manual(mod::tid(mod::kLoop, 0, isbal ? 0 : 1), (float)v);
      if (isbal) set_bal(v);
      else set_lev(v);
      sniprintf(o, cap, "loop: %s %d", isbal ? "bal" : "lev", v);
    }
  } else if (!strncmp(a, "fade:", 5)) {
    int f = -1;
    for (int i = 0; i < kFades; ++i)
      if (!strcmp(a + 5, kFadeName[i])) f = i;
    if (!strcmp(a + 5, "0.25")) f = 0;
    if (!strcmp(a + 5, "0.5")) f = 1;
    if (f < 0) sniprintf(o, cap, "loop=fade:1/4|1/2|1|2|4 (bars)");
    else set_fade(f), sniprintf(o, cap, "loop: fade %s bars", kFadeName[f]);
  } else if (!strncmp(a, "mute:", 5)) {
    if (!strcmp(a + 5, "on") || !strcmp(a + 5, "off")) set_auto_mute(a[5 + 1] == 'n'), sniprintf(o, cap, "loop: auto-mute %s", auto_mute_ ? "on" : "off");
    else sniprintf(o, cap, "loop=mute:on|off");
  } else if (!strncmp(a, "live:", 5)) {
    int m = -1;
    if (!strcmp(a + 5, "off")) m = 0;
    else if (!strncmp(a + 5, "perf", 4) && parse_int(a + 9, &v) && v >= 1 && v <= 12) m = v;
    if (m < 0) sniprintf(o, cap, "loop=live:off|perf1..perf12");
    else set_live_via(m), sniprintf(o, cap, "loop: live via %s", m ? "PERF" : "off");
    if (m > 0) sniprintf(o + strlen(o), cap - (int)strlen(o), " %d", m);
  } else if (!strcmp(a, "cal")) {
    cal_start();
    sniprintf(o, cap, "loop: calibrating POST latency (3 clicks on POST 0 / 1, the Rytm playing or not)");
  } else {
    sniprintf(o, cap, "loop: grab:L play:N stop[:cut|fade][:now] keep:N bal:V lev:V fade:B mute:on|off live:off|perfN cal");
  }
  return true;
}

CORE_COLD int json(char* o, int cap) {
  if (cap <= 0) return 0;
  int n = sniprintf(o, cap, "\"loop\":{\"on\":%d", on_ ? 1 : 0);
  if (!on_) return n + sniprintf(o + n, cap - n, "}");
  float bar, of;
  bool pos = bar_position(&bar, &of);
  n += sniprintf(o + n, cap - n,
                 ",\"state\":\"%s\",\"ring_s\":%d,\"bal\":%d,\"lev\":%d,\"fade\":%d,\"mute\":%d,\"live\":%d,\"post_lat\":%d,\"pending\":%d,\"cal\":%d",
                 kStateName[state()], ring_seconds(), bal_, lev_, fade_, auto_mute_ ? 1 : 0, live_via_, post_lat_, pend_.on ? 1 : 0,
                 cal_state_ == 1 ? 1 : 0);
  if (pos) n += sniprintf(o + n, cap - n, ",\"bar\":%.3f,\"bars\":%.1f", bar, of);
  n += sniprintf(o + n, cap - n, ",\"msg\":\"%s\",\"msg_n\":%u,\"grabs\":[", msg_, (unsigned)msg_gen_);
  int pi = playing();
  for (int i = 0; i < ng_ && n < cap - 120; ++i)
    n += sniprintf(o + n, cap - n, "%s{\"id\":%u,\"hb\":%d,\"bpm\":%.1f,\"kept\":%d,\"gap\":%d,\"copied\":%d,\"playing\":%d}", i ? "," : "",
                   (unsigned)g_[i].id, g_[i].half_bars, (double)g_[i].bpm, g_[i].kept, g_[i].gap, g_[i].copied, i == pi ? 1 : 0);
  n += sniprintf(o + n, cap - n, "]}");
  return n < cap ? n : 0;
}

// ------------------------------------------------------------------------------------------------ core 0
void in_block(const int32_t* lr, int n, uint64_t arrival_frame, unsigned lost) {
  if (!ring_.ready()) return;
  ring_.write(lr, n, arrival_frame, lost);
  uint64_t c0 = __atomic_load_n(&cal_at_, __ATOMIC_ACQUIRE);
  if (!c0) return;
  // the calibration's listening (spec 3.4): the stream's own level before the first click, then each click's window
  uint64_t first = arrival_frame - (uint64_t)(n - 1);
  int32_t fl = cal_floor_;
  if (first < c0) {
    for (int i = 0; i < n; ++i) {
      int32_t a = lr[2 * i] < 0 ? -lr[2 * i] : lr[2 * i];
      if (a > fl) fl = a;
    }
    cal_floor_ = fl;
    return;
  }
  int32_t thr = fl > 0x7FFFFFFF / 8 ? 0x7FFFFFFF / 2 : 4 * fl;
  if (thr < (1 << 21)) thr = 1 << 21;   // -60 dBFS at least
  for (int c = 0; c < kCalN; ++c) {
    uint64_t t = c0 + (uint64_t)c * kCalGap;
    if (arrival_frame < t || first >= t + kCalWindow || cal_found_[c] >= 0) continue;
    for (int i = 0; i < n; ++i) {
      uint64_t h = first + (uint64_t)i;
      int32_t a = lr[2 * i] < 0 ? -lr[2 * i] : lr[2 * i];
      if (h >= t && h < t + kCalWindow && a > thr) {
        __atomic_store_n(&cal_found_[c], (int32_t)(h - t), __ATOMIC_RELEASE);
        break;
      }
    }
  }
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
  uint64_t c0 = __atomic_load_n(&cal_at_, __ATOMIC_ACQUIRE);
  if (c0) {   // loop=cal: a click (a short half-sine burst) onto POST 0 / 1, each kCalGap
    for (int f = 0; f < n; ++f) {
      uint64_t h = render_frame + (uint64_t)f;
      if (h < c0 || (h - c0) / kCalGap >= (uint64_t)kCalN || (h - c0) % kCalGap >= (uint64_t)kCalClick) continue;
      int32_t x = (int32_t)(8.0e8f * sinf(3.14159265f * (float)((h - c0) % kCalGap + 1) / (kCalClick + 1)));
      F[f][0] = add(F[f][0], (float)x), F[f][1] = add(F[f][1], (float)x);
    }
  }
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

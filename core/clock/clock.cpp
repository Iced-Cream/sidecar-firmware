// core/clock/clock.cpp - see clock.h.
#include "clock.h"

#include "../device/device.h"

namespace core {
namespace clock {
namespace {
Clock g_slots[kSlots];

template <class F> bool add(F* tab, F f) {
  for (int i = 0; i < kListeners; ++i) if (tab[i] == f) return true;
  for (int i = 0; i < kListeners; ++i) if (!tab[i]) { tab[i] = f; return true; }
  return false;
}
}  // namespace

int tracks_for(const device::Profile* p) {
  if (!p || p->id == device::kIdRytm) return kTracks;   // the Rytm: its 12 and the Storage phase's 13th
  return p->tracks < kTracks ? p->tracks : kTracks;
}

Clock& slot(int s) { return g_slots[s >= 0 && s < kSlots ? s : 0]; }
int Clock::slot() const { return (int)(this - g_slots); }

bool Clock::on_tick(TickFn f) { return f && add(tick_fn_, f); }
bool Clock::on_state(StateFn f) { return f && add(state_fn_, f); }

void Clock::set_tracks(int n) {
  n_tracks_ = n < 0 ? 0 : n > kTracks ? kTracks : n;
  for (int t = n_tracks_; t < kTracks; ++t) last_[t].on = false;
}

void Clock::reset() {
  transport_ = kStop, pattern_ = -1, queued_ = -1, bpm_ = 0;
  for (Last& l : last_) l.on = false;
  have_blk_ = false, blk_hi_ = 0;
}

int64_t Clock::unwrap(const uint8_t* b) {
  uint32_t blk = (uint32_t)(b[0] & 0x7F) << 21 | (uint32_t)(b[1] & 0x7F) << 14 | (uint32_t)(b[2] & 0x7F) << 7 | (b[3] & 0x7F);
  const uint32_t span = 1u << 28;
  if (have_blk_ && blk < last_blk_ && last_blk_ - blk > span / 2) blk_hi_ += span;   // wrapped forward
  have_blk_ = true, last_blk_ = blk;
  return blk_hi_ + blk;
}

void Clock::ticks(const uint8_t* b, int n) {
  int cnt = b[4];
  int rec = cnt > 0 ? (n - 5) / cnt : 0;   // 4 bytes a tick, 5 from a sender that adds one
  if (cnt < 1 || (rec != 4 && rec != 5) || 5 + rec * cnt != n) { ++st_.bad; return; }
  Tick t;
  t.block = unwrap(b);
  t.slot = (uint8_t)slot();
  if (transport_ != kPlay) transport_ = kPlay;   // ticks before the TRANSPORT (or a lost one): playing
  for (int i = 0; i < cnt; ++i) {
    const uint8_t* p = b + 5 + rec * i;
    t.track = p[0], t.step = p[1], t.len = p[2], t.speed = p[3], t.extra = rec == 5 ? p[4] : 0;
    ++st_.ticks;
    if (t.track >= n_tracks_) { ++st_.off_track; continue; }
    last_[t.track] = {true, t.step, t.len, t.speed};
    for (TickFn f : tick_fn_) if (f) f(t);
  }
}

void Clock::state(const uint8_t* a, int n) {
  if (n < 1) { ++st_.bad; return; }
  ++st_.states;
  State s = {a[0], a + 1, n - 1, -1, (uint8_t)slot()};
  switch (a[0]) {
    case kTransport:
      if (n < 6 || a[1] > kPause) { ++st_.bad; return; }
      s.block = unwrap(a + 2);
      transport_ = a[1];
      break;
    case kPattern:
      if (n < 3) { ++st_.bad; return; }
      if (a[1] < 128) pattern_ = a[1];
      queued_ = a[2] == kNone ? -1 : a[2];
      break;
    case kSwitch:
      if (n < 7) { ++st_.bad; return; }
      s.block = unwrap(a + 3);
      if (a[2] < 128) pattern_ = a[2];
      queued_ = -1;
      break;
    case kTempo:
      if (n < 4) { ++st_.bad; return; }
      bpm_ = ((uint32_t)a[1] << 14 | (uint32_t)a[2] << 7 | a[3]) / 120.0;
      break;
    default:
      break;
  }
  for (StateFn f : state_fn_) if (f) f(s);
}

bool Clock::sysex(const uint8_t* d, int len) {
  if (len < 8 || d[0] != 0xF0 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54) return false;
  if (d[4] != kTickCmd && d[4] != kStateCmd) return false;
  if (d[len - 1] != 0xF7 || d[5] != 1) { ++st_.bad; return true; }
  const uint8_t* b = d + 6;
  int n = len - 7;
  if (d[4] == kTickCmd) {
    if (n < 5) { ++st_.bad; return true; }
    ticks(b, n);
  } else {
    state(b, n);
  }
  return true;
}

bool Clock::step_of(int track, int* step, int* len, int* speed) const {
  if (track < 0 || track >= n_tracks_ || !last_[track].on) return false;
  *step = last_[track].step, *len = last_[track].len, *speed = last_[track].speed;
  return true;
}

// Slot 0's
bool on_tick(TickFn f) { return g_slots[0].on_tick(f); }
bool on_state(StateFn f) { return g_slots[0].on_state(f); }
bool sysex(const uint8_t* d, int len) { return g_slots[0].sysex(d, len); }
void reset() { g_slots[0].reset(); }
uint8_t transport() { return g_slots[0].transport(); }
int pattern() { return g_slots[0].pattern(); }
int queued() { return g_slots[0].queued(); }
double bpm() { return g_slots[0].bpm(); }
bool step_of(int track, int* step, int* len, int* speed) { return g_slots[0].step_of(track, step, len, speed); }
int tracks() { return g_slots[0].tracks(); }
Stats stats() { return g_slots[0].stats(); }
}  // namespace clock
}  // namespace core

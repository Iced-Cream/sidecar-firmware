// core/clock/clock.cpp - see clock.h.
#include "clock.h"

namespace core {
namespace clock {
namespace {
TickFn g_tick[kListeners];
StateFn g_state[kListeners];
uint8_t g_transport = kStop;
int g_pattern = -1, g_queued = -1;
double g_bpm = 0;
struct Last { bool on; uint8_t step, len, speed; } g_last[kTracks];
Stats g_st;
bool g_have_blk = false;
uint32_t g_last_blk = 0;
int64_t g_blk_hi = 0;

int64_t unwrap(const uint8_t* b) {
  uint32_t blk = (uint32_t)(b[0] & 0x7F) << 21 | (uint32_t)(b[1] & 0x7F) << 14 | (uint32_t)(b[2] & 0x7F) << 7 | (b[3] & 0x7F);
  const uint32_t span = 1u << 28;
  if (g_have_blk && blk < g_last_blk && g_last_blk - blk > span / 2) g_blk_hi += span;   // wrapped forward
  g_have_blk = true, g_last_blk = blk;
  return g_blk_hi + blk;
}
template <class F, class A> bool add(F* tab, F f) {
  for (int i = 0; i < kListeners; ++i) if (tab[i] == f) return true;
  for (int i = 0; i < kListeners; ++i) if (!tab[i]) { tab[i] = f; return true; }
  return false;
}
// the MIDI clock (upstream lfo.cpp's Clock)
volatile bool g_run = false, g_ticking = false;
volatile int32_t g_pos = -1;          // the latest tick's song position (24 ppqn) since Start, -1 none yet
volatile uint32_t g_last_us = 0;      // when the latest tick came
volatile float g_tick_us = 20833.3f;  // the tempo: 120 BPM until measured
}  // namespace

void realtime(uint8_t status, uint16_t spp, uint32_t now) {
  switch (status) {
    case 0xF8: {
      if (g_ticking) {
        uint32_t d = now - g_last_us;
        if (d > 1000 && d < 250000) g_tick_us = g_tick_us + (g_tick_us > 0 ? ((float)d - g_tick_us) * 0.25f : (float)d);
      }
      g_ticking = true;
      g_last_us = now;
      if (g_run) g_pos = g_pos + 1;
      break;
    }
    case 0xFA: g_run = true, g_pos = -1; break;
    case 0xFB: g_run = true; break;
    case 0xFC: g_run = false; break;
    case 0xF2: g_pos = (int32_t)spp * 6 - 1; break;
    default: break;
  }
}
bool position(uint32_t us, float* ticks) {
  uint32_t since = us - g_last_us;
  bool locked = g_run && g_ticking && since < 250000;
  if (!locked) return false;
  int32_t pos = g_pos;
  if (pos < 0) { *ticks = 0.0f; return true; }
  float f = (float)since / g_tick_us;
  *ticks = (float)(pos % (int32_t)kWrap) + (f < 0.0f ? 0.0f : f > 0.999f ? 0.999f : f);
  return true;
}
float tick_us() { return g_tick_us; }

bool on_tick(TickFn f) { return f && add<TickFn, Tick>(g_tick, f); }
bool on_state(StateFn f) { return f && add<StateFn, State>(g_state, f); }

void reset() {
  g_transport = kStop, g_pattern = -1, g_queued = -1, g_bpm = 0;
  for (Last& l : g_last) l.on = false;
  g_have_blk = false, g_blk_hi = 0;
}

static void ticks(const uint8_t* b, int n) {
  int cnt = b[4];
  int rec = cnt > 0 ? (n - 5) / cnt : 0;   // 4 bytes a tick, 5 from a sender that adds one
  if (cnt < 1 || (rec != 4 && rec != 5) || 5 + rec * cnt != n) { ++g_st.bad; return; }
  Tick t;
  t.block = unwrap(b);
  if (g_transport != kPlay) g_transport = kPlay;   // ticks before the TRANSPORT (or a lost one): playing
  for (int i = 0; i < cnt; ++i) {
    const uint8_t* p = b + 5 + rec * i;
    t.track = p[0], t.step = p[1], t.len = p[2], t.speed = p[3], t.extra = rec == 5 ? p[4] : 0;
    ++g_st.ticks;
    if (t.track >= kTracks) continue;
    g_last[t.track] = {true, t.step, t.len, t.speed};
    for (TickFn f : g_tick) if (f) f(t);
  }
}

static void state(const uint8_t* a, int n) {
  if (n < 1) { ++g_st.bad; return; }
  ++g_st.states;
  State s = {a[0], a + 1, n - 1, -1};
  switch (a[0]) {
    case kTransport:
      if (n < 6 || a[1] > kPause) { ++g_st.bad; return; }
      s.block = unwrap(a + 2);
      g_transport = a[1];
      break;
    case kPattern:
      if (n < 3) { ++g_st.bad; return; }
      if (a[1] < 128) g_pattern = a[1];
      g_queued = a[2] == kNone ? -1 : a[2];
      break;
    case kSwitch:
      if (n < 7) { ++g_st.bad; return; }
      s.block = unwrap(a + 3);
      if (a[2] < 128) g_pattern = a[2];
      g_queued = -1;
      break;
    case kTempo:
      if (n < 4) { ++g_st.bad; return; }
      g_bpm = ((uint32_t)a[1] << 14 | (uint32_t)a[2] << 7 | a[3]) / 120.0;
      break;
    default:
      break;
  }
  for (StateFn f : g_state) if (f) f(s);
}

bool sysex(const uint8_t* d, int len) {
  if (len < 8 || d[0] != 0xF0 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54) return false;
  if (d[4] != kTickCmd && d[4] != kStateCmd) return false;
  if (d[len - 1] != 0xF7 || d[5] != 1) { ++g_st.bad; return true; }
  const uint8_t* b = d + 6;
  int n = len - 7;
  if (d[4] == kTickCmd) {
    if (n < 5) { ++g_st.bad; return true; }
    ticks(b, n);
  } else {
    state(b, n);
  }
  return true;
}

uint8_t transport() { return g_transport; }
int pattern() { return g_pattern; }
int queued() { return g_queued; }
double bpm() { return g_bpm; }
bool step_of(int track, int* step, int* len, int* speed) {
  if (track < 0 || track >= kTracks || !g_last[track].on) return false;
  *step = g_last[track].step, *len = g_last[track].len, *speed = g_last[track].speed;
  return true;
}
Stats stats() { return g_st; }
}  // namespace clock
}  // namespace core

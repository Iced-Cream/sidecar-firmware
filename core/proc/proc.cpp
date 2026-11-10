// core/proc/proc.cpp - see proc.h.
#include "proc.h"

namespace core {
namespace proc {
namespace {
Proc* volatile g_slot[kSlots];
struct Msg { int8_t slot; uint8_t param; uint16_t value; uint8_t m[3]; };   // slot -1: link_reset; -2: MIDI (param: src);
    // -3 a mod point (m[0] id, m[1] src << 4 | dst, m[2] ctl, param, value depth); -4 its depth; -5 its end;
    // slot >= 0: set_param, with m[0] (a status) a note, with m[1] = 1 a default
constexpr uint32_t kQ = 256;
Msg g_q[kQ];
alignas(64) uint32_t g_wr = 0;
alignas(64) uint32_t g_rd = 0;
Stats g_st;
alignas(64) uint32_t g_busy;        // render side: 1 inside a block (begin .. end)
uint32_t g_cleared[kSlots];         // control side: the block count when the slot was cleared while a block ran
bool g_wait[kSlots];
// render side: the mod points, each slot's params' bases and what was last given, and which params a point moved
struct Mod { bool on; uint8_t src, dst, ctl, param; float depth; };
Mod g_mod[kMods];
constexpr int kMask = kParams / 64;
static_assert(kMask == 2, "g_moved's two words");
uint16_t g_base[kSlots][kParams];
uint16_t g_given[kSlots][kParams];
uint64_t g_moved[kSlots][kMask];   // the params a mod point moved last block (bits)
int g_mods_on;

bool push(const Msg& m) {
  uint32_t w = __atomic_load_n(&g_wr, __ATOMIC_RELAXED), r = __atomic_load_n(&g_rd, __ATOMIC_ACQUIRE);
  if (w - r >= kQ) return false;
  g_q[w & (kQ - 1)] = m;
  __atomic_store_n(&g_wr, w + 1, __ATOMIC_RELEASE);
  return true;
}
bool pop(Msg* m) {
  uint32_t r = __atomic_load_n(&g_rd, __ATOMIC_RELAXED), w = __atomic_load_n(&g_wr, __ATOMIC_ACQUIRE);
  if (r == w) return false;
  *m = g_q[r & (kQ - 1)];
  __atomic_store_n(&g_rd, r + 1, __ATOMIC_RELEASE);
  return true;
}
}  // namespace

bool register_slot(int s, Proc* p) {
  if (s < 0 || s >= kSlots || !p || g_slot[s]) return false;
  __atomic_store_n(&g_slot[s], p, __ATOMIC_RELEASE);
  return true;
}
void clear_slot(int s) {
  if (s < 0 || s >= kSlots) return;
  __atomic_store_n(&g_slot[s], (Proc*)nullptr, __ATOMIC_SEQ_CST);
  // a block under way may have read the pointer already: released() waits for it to end
  g_wait[s] = __atomic_load_n(&g_busy, __ATOMIC_SEQ_CST) != 0;
  g_cleared[s] = __atomic_load_n(&g_st.blocks, __ATOMIC_ACQUIRE);
}
bool released(int s) {
  if (s < 0 || s >= kSlots || slot(s)) return false;
  if (!g_wait[s]) return true;
  if (!__atomic_load_n(&g_busy, __ATOMIC_SEQ_CST) || __atomic_load_n(&g_st.blocks, __ATOMIC_ACQUIRE) - g_cleared[s] >= 2) g_wait[s] = false;
  return !g_wait[s];
}
Proc* slot(int s) { return s >= 0 && s < kSlots ? __atomic_load_n(&g_slot[s], __ATOMIC_ACQUIRE) : nullptr; }
bool any() {
  for (int s = 0; s < kSlots; ++s) if (slot(s)) return true;
  return false;
}
bool set_param(int s, int param, int value14) {
  if (!slot(s) || param < 0 || param >= kParams || value14 < 0 || value14 > 16383 || !push({(int8_t)s, (uint8_t)param, (uint16_t)value14, {0, 0, 0}})) {
    ++g_st.dropped;
    return false;
  }
  ++g_st.params;
  return true;
}
bool set_default(int s, int param, int value14) {
  if (!slot(s) || param < 0 || param >= kParams || value14 < 0 || value14 > 16383) return false;
  return push({(int8_t)s, (uint8_t)param, (uint16_t)value14, {0, 1, 0}});
}
bool set_mod(int id, int src, int ctl, int dst, int param, int depth14) {
  if (id < 0 || id >= kMods || src < 0 || src >= kSlots || dst < 0 || dst >= kSlots || ctl < 0 || ctl >= kCtls ||
      param < 0 || param >= kParams || depth14 < 0 || depth14 > 16383 ||
      !push({-3, (uint8_t)param, (uint16_t)depth14, {(uint8_t)id, (uint8_t)(src << 4 | dst), (uint8_t)ctl}})) {
    ++g_st.dropped;
    return false;
  }
  ++g_st.mods;
  return true;
}
bool set_mod_depth(int id, int depth14) {
  if (id < 0 || id >= kMods || depth14 < 0 || depth14 > 16383 || !push({-4, 0, (uint16_t)depth14, {(uint8_t)id, 0, 0}})) {
    ++g_st.dropped;
    return false;
  }
  return true;
}
bool clear_mod(int id) {
  if (id < 0 || id >= kMods || !push({-5, 0, 0, {(uint8_t)id, 0, 0}})) { ++g_st.dropped; return false; }
  return true;
}
void link_reset() {
  if (!push({-1, 0, 0, {0, 0, 0}})) ++g_st.dropped;
}
bool midi(int src, uint8_t status, uint8_t d1, uint8_t d2) {
  if (!any()) return false;
  if (!push({-2, (uint8_t)src, 0, {status, d1, d2}})) {
    ++g_st.dropped;
    return false;
  }
  ++g_st.midi;
  return true;
}

bool note(int s, uint8_t status, uint8_t d1, uint8_t d2) {
  if (!slot(s) || !(status & 0x80) || !push({(int8_t)s, 0, 0, {status, (uint8_t)(d1 & 0x7F), (uint8_t)(d2 & 0x7F)}})) {
    ++g_st.dropped;
    return false;
  }
  ++g_st.notes;
  return true;
}

void begin() {
  __atomic_store_n(&g_busy, 1u, __ATOMIC_SEQ_CST);
  Msg m;
  while (pop(&m)) {
    if (m.slot <= -3) {
      Mod& d = g_mod[m.m[0] % kMods];
      if (m.slot == -3) g_mods_on += !d.on, d = Mod{true, (uint8_t)(m.m[1] >> 4), (uint8_t)(m.m[1] & 15), m.m[2], m.param, 0.0f};
      if (m.slot == -5) g_mods_on -= d.on, d.on = false;
      else d.depth = ((float)m.value - 8192.0f) / 8192.0f;
    } else if (m.slot == -2) {
      for (int s = 0; s < kSlots; ++s) if (Proc* p = slot(s)) p->midi(m.param, m.m[0], m.m[1], m.m[2]);
    } else if (m.slot < 0) {
      for (int s = 0; s < kSlots; ++s) if (Proc* p = slot(s)) p->link_reset();
    } else if (Proc* p = slot(m.slot)) {
      if (m.m[0]) p->midi(kSrcRytm, m.m[0], m.m[1], m.m[2]);   // a note event (set_param's have m[0] = 0)
      else {
        g_base[m.slot][m.param] = g_given[m.slot][m.param] = m.value;
        if (!m.m[1]) p->set_param(m.param, m.value);           // (a default: the block has it already)
      }
    }
  }
}
namespace {
// slot s's modulated params, before it runs: base + the sum of its points' ctl x depth (see proc.h)
void modulate(int s, Proc* p) {
  float off[kParams];
  uint64_t now[kMask] = {};
  for (const Mod& d : g_mod) {
    if (!d.on || d.dst != s) continue;
    Proc* src = slot(d.src);
    float c = src ? src->ctl(d.ctl) : 0.0f;
    uint64_t bit = 1ull << (d.param & 63);
    uint64_t& w = now[d.param >> 6];
    if (!(w & bit)) off[d.param] = 0.0f, w |= bit;
    off[d.param] += c * d.depth * 16383.0f;
  }
  for (int m = 0; m < kMask; ++m) {
    uint64_t all = now[m] | g_moved[s][m];
    for (int j = 0; all; ++j, all >>= 1) {
      if (!(all & 1)) continue;
      int k = 64 * m + j;
      int v = g_base[s][k] + ((now[m] >> j & 1) ? (int)(off[k] + (off[k] < 0 ? -0.5f : 0.5f)) : 0);
      v = v < 0 ? 0 : v > 16383 ? 16383 : v;
      if (v != g_given[s][k]) { g_given[s][k] = (uint16_t)v; p->set_param(k, v); }
    }
    g_moved[s][m] = now[m];
  }
}
}  // namespace

void run(int s, const Block& b0, float* const* slot_in, float* const* slot_out) {
  Proc* p = slot(s);
  if (!p || b0.frames <= 0 || b0.frames > kMaxFrames) return;
  if (g_mods_on || g_moved[s][0] || g_moved[s][1]) modulate(s, p);
  Block b = b0;
  for (int k = 0; k < kSlotIO; ++k) {
    b.sin[k] = slot_in ? slot_in[kSlotIO * s + k] : nullptr;
    b.sout[k] = slot_out ? slot_out[kSlotIO * s + k] : nullptr;
  }
  p->process(b);
}
void end() {
  __atomic_fetch_add(&g_st.blocks, 1u, __ATOMIC_ACQ_REL);
  __atomic_store_n(&g_busy, 0u, __ATOMIC_SEQ_CST);
}
void process(const Block& b, float* const* slot_in, float* const* slot_out) {
  begin();
  if (b.frames > 0 && b.frames <= kMaxFrames)
    for (int s = 0; s < kSlots; ++s) run(s, b, slot_in, slot_out);
  end();
}
Stats stats() { return g_st; }
}  // namespace proc
}  // namespace core

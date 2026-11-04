// core/proc/proc.cpp - see proc.h.
#include "proc.h"

namespace core {
namespace proc {
namespace {
Proc* volatile g_slot[kSlots];
struct Msg { int8_t slot; uint8_t param; uint16_t value; uint8_t m[3]; };   // slot -1: link_reset; -2: MIDI (param: src)
constexpr uint32_t kQ = 256;
Msg g_q[kQ];
alignas(64) uint32_t g_wr = 0;
alignas(64) uint32_t g_rd = 0;
Stats g_st;

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
  if (s >= 0 && s < kSlots) __atomic_store_n(&g_slot[s], (Proc*)nullptr, __ATOMIC_RELEASE);
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

void process(const Block& b) {
  Msg m;
  while (pop(&m)) {
    if (m.slot == -2) {
      for (int s = 0; s < kSlots; ++s) if (Proc* p = slot(s)) p->midi(m.param, m.m[0], m.m[1], m.m[2]);
    } else if (m.slot < 0) {
      for (int s = 0; s < kSlots; ++s) if (Proc* p = slot(s)) p->link_reset();
    } else if (Proc* p = slot(m.slot)) {
      p->set_param(m.param, m.value);
    }
  }
  if (b.frames <= 0 || b.frames > kMaxFrames) return;
  ++g_st.blocks;
  for (int s = 0; s < kSlots; ++s) if (Proc* p = slot(s)) p->process(b);
}
Stats stats() { return g_st; }
}  // namespace proc
}  // namespace core

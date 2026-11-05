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
alignas(64) uint32_t g_busy;        // render side: 1 inside a block (begin .. end)
uint32_t g_cleared[kSlots];         // control side: the block count when the slot was cleared while a block ran
bool g_wait[kSlots];

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

void begin() {
  __atomic_store_n(&g_busy, 1u, __ATOMIC_SEQ_CST);
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
}
void run(int s, const Block& b0, float* const* slot_in, float* const* slot_out) {
  Proc* p = slot(s);
  if (!p || b0.frames <= 0 || b0.frames > kMaxFrames) return;
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

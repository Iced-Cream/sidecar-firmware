// core/devmidi/devmidi.cpp - see devmidi.h.
#include "devmidi/devmidi.h"

#include <string.h>

#include "core_port.h"
#include "platform.h"
#include "proc/proc.h"

namespace core {
namespace devmidi {
namespace {
constexpr int kListeners = 4;
struct Src {
  bool on;
  char name[kNameLen + 1];
  uint32_t held[16][4];   // per channel, the notes held (sent on, not yet off): 128 bits
};
Src g_src[kSrcs];
ForwardFn g_fwd;
ListenFn g_ln[kListeners];
bool g_thru = true;
Stats g_st;

void deliver(int src, uint8_t s, uint8_t a, uint8_t b, bool to_rytm) {
  if (to_rytm && g_thru && g_fwd) g_fwd(s, a, b), ++g_st.forwarded;
  if (proc::any() && proc::midi(src, s, a, b)) ++g_st.to_slots;
  for (ListenFn f : g_ln) if (f) f(src, s, a, b);
}
// every held note's note-off: to the Rytm (rytm), to the slots and listeners (all)
void release(int src, bool rytm, bool all) {
  Src& x = g_src[src];
  for (int ch = 0; ch < 16; ++ch)
    for (int n = 0; n < 128; ++n) {
      if (!(x.held[ch][n >> 5] & 1u << (n & 31))) continue;
      uint8_t s = (uint8_t)(0x80 | ch);
      if (all) deliver(src, s, (uint8_t)n, 0, rytm);
      else if (rytm && g_fwd) g_fwd(s, (uint8_t)n, 0), ++g_st.forwarded;
      ++g_st.released;
      if (all) x.held[ch][n >> 5] &= ~(1u << (n & 31));
    }
}
}  // namespace

void set_forward(ForwardFn f) { g_fwd = f; }
bool on_midi(ListenFn f) {
  if (!f) return false;
  for (ListenFn& g : g_ln) if (g == f) return true;
  for (ListenFn& g : g_ln) if (!g) { g = f; return true; }
  return false;
}
void set_thru(bool on) {
  if (on == g_thru) return;
  if (!on)
    for (int i = 0; i < kSrcs; ++i) if (g_src[i].on) release(i, true, false);   // the Rytm's held notes end
  g_thru = on;
  log("devmidi: thru %s", on ? "on (the USB-A devices' MIDI goes to the Rytm)" : "off");
}
bool thru() { return g_thru; }

void plugged(int src, bool on, const char* nm) {
  if (src < 0 || src >= kSrcs) return;
  Src& x = g_src[src];
  if (!on) {
    if (x.on) release(src, true, true);
    log("devmidi: %d %s gone", src + 1, x.name);
    x.on = false;
    x.name[0] = 0;
    return;
  }
  memset(x.held, 0, sizeof x.held);
  x.on = true;
  sniprintf(x.name, sizeof x.name, "%s", nm && *nm ? nm : "USB MIDI");
  log("devmidi: %d %s: %s", src + 1, x.name, g_thru ? "its MIDI goes to the Rytm" : "thru off");
}

void midi(int src, uint8_t s, uint8_t a, uint8_t b) {
  if (src < 0 || src >= kSrcs || !g_src[src].on || s < 0x80) { ++g_st.ignored; return; }
  bool rt = s == 0xF8 || s == 0xFA || s == 0xFB || s == 0xFC;
  if (s >= 0xF0 && !rt) { ++g_st.ignored; return; }
  ++g_st.messages;
  if (!rt) {
    a &= 0x7F, b &= 0x7F;
    uint8_t k = s & 0xF0, ch = s & 0x0F;
    uint32_t& w = g_src[src].held[ch][a >> 5];
    if (k == 0x90 && b) w |= 1u << (a & 31);
    else if (k == 0x80 || k == 0x90) w &= ~(1u << (a & 31));
  }
  deliver(src, s, a, b, true);
}

bool present(int src) { return src >= 0 && src < kSrcs && g_src[src].on; }
const char* name(int src) { return present(src) ? g_src[src].name : ""; }
namespace {
constexpr uint32_t kOutQ = 256;
uint32_t s_out[kOutQ];
alignas(64) uint32_t s_out_wr = 0;
alignas(64) uint32_t s_out_rd = 0;
uint32_t s_out_n, s_out_dropped;   // (the producer's)
}  // namespace
bool out(int dst, uint8_t status, uint8_t d1, uint8_t d2) {
  if (dst < 0 || dst > kSrcs || status < 0x80 || status >= 0xF0) { ++s_out_dropped; return false; }
  uint32_t w = __atomic_load_n(&s_out_wr, __ATOMIC_RELAXED), r = __atomic_load_n(&s_out_rd, __ATOMIC_ACQUIRE);
  if (w - r >= kOutQ) { ++s_out_dropped; return false; }
  uint32_t cin = status >> 4;
  s_out[w & (kOutQ - 1)] = cin | (uint32_t)dst << 4 | (uint32_t)status << 8 | (uint32_t)(d1 & 0x7F) << 16 | (uint32_t)(d2 & 0x7F) << 24;
  __atomic_store_n(&s_out_wr, w + 1, __ATOMIC_RELEASE);
  ++s_out_n;
  return true;
}
bool take_out(uint32_t* v) {
  uint32_t r = __atomic_load_n(&s_out_rd, __ATOMIC_RELAXED), w = __atomic_load_n(&s_out_wr, __ATOMIC_ACQUIRE);
  if (r == w) return false;
  *v = s_out[r & (kOutQ - 1)];
  __atomic_store_n(&s_out_rd, r + 1, __ATOMIC_RELEASE);
  return true;
}
Stats stats() {
  Stats s = g_st;
  s.out = s_out_n;
  s.out_dropped = s_out_dropped;
  return s;
}

}  // namespace devmidi
}  // namespace core

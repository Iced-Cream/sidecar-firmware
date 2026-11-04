// core/kv/link.cpp - see link.h.
#include "link.h"

#include <string.h>

#include "../hooks.h"
#include "../msamp/cardq.h"
#include "../locks/locks.h"
#include "kv.h"
#include "kv_card.h"
#include <string.h>
#include "../generic/generic.h"

namespace core {
namespace kv {
namespace link {

#ifndef ARDUINO   // Sidecar Mini (Teensy): no record store (its 51 messages are dropped; the Rytm sees no HELLO answer)
namespace {
struct Msg { uint16_t n; uint8_t d[kMsgMax]; };
// a single-producer single-consumer ring (acquire/release on the indices, as pi/engine/spsc.h)
template <uint32_t N>
struct Ring {
  Msg buf[N];
  alignas(64) uint32_t wr = 0;
  alignas(64) uint32_t rd = 0;
  bool push(const uint8_t* m, int n) {
    uint32_t w = __atomic_load_n(&wr, __ATOMIC_RELAXED), r = __atomic_load_n(&rd, __ATOMIC_ACQUIRE);
    if (w - r >= N || n > kMsgMax) return false;
    buf[w & (N - 1)].n = (uint16_t)n;
    memcpy(buf[w & (N - 1)].d, m, (size_t)n);
    __atomic_store_n(&wr, w + 1, __ATOMIC_RELEASE);
    return true;
  }
  Msg* front() {
    uint32_t r = __atomic_load_n(&rd, __ATOMIC_RELAXED), w = __atomic_load_n(&wr, __ATOMIC_ACQUIRE);
    return r == w ? nullptr : &buf[r & (N - 1)];
  }
  void pop() { __atomic_store_n(&rd, __atomic_load_n(&rd, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE); }
};
Ring<64> s_in;    // control -> card: 64 x 160 B (a 4 KB record is 37 WRITE chunks)
Ring<64> s_out;   // card -> control: a 4 KB READ is 37 DATA messages
Stats s_st;
CardBackend s_card;
Store s_store(&s_card);
void out(const uint8_t* m, int n) {
  if (!s_out.push(m, n)) ++s_st.dropped_out;
}
Service s_svc(&s_store, out);
}  // namespace

bool from_host(const uint8_t* m, int n) {
  if (n < 8 || m[4] != kCmd) return false;
  ++s_st.in;
  if (locks::is_lock_op(m[6])) return locks::sysex(m, n);   // the control core's own: the ticks arrive here too
  // the companion's HELLO starts its session: the sets it declared before are gone with it (it declares them again
  // once answered). Here and not at the screen's 35, which may come after the HELLO and would wipe the new sets.
  if (m[6] == kOpHello) locks::reset();
  if (!s_in.push(m, n)) { ++s_st.dropped_in; return false; }
  return true;
}

int card_step(int max_ops) {
  int k = 0;
  while (k < max_ops) {
    Msg* m = s_in.front();
    if (!m) break;
    s_svc.sysex(m->d, m->n);
    s_in.pop();
    ++k;
    ++s_st.ops;
  }
  return k;
}

int host_step(int max_msgs) {
  if (!cardq::worker()) card_step(4);   // no card core (the Mac core, tests): the ops run here
  int k = 0;
  while (k < max_msgs) {
    Msg* m = s_out.front();
    if (!m) break;
    core::to_rytm(m->d, m->n);
    s_out.pop();
    ++k;
    ++s_st.out;
  }
  return k;
}

void hello() { locks::reset(); }
Stats stats() { return s_st; }

static bool store_fn(const uint8_t* d, int len) { return d[4] == kCmd && (from_host(d, len), true); }
// a link reset for another box: the sets go; the Rytm's 35 is not the store session's start (the HELLO is)
static void link_fn(const char* why) { if (!why || strcmp(why, "hello")) hello(); }
bool attach_generic() {
  gen::set_store(store_fn);
  bool ok = gen::on_link(link_fn);
  return locks::attach() && ok;
}
#else
bool from_host(const uint8_t*, int) { return false; }
int card_step(int) { return 0; }
int host_step(int) { return 0; }
void hello() {}
Stats stats() { return Stats{}; }
bool attach_generic() { return false; }
#endif

}  // namespace link
}  // namespace kv
}  // namespace core

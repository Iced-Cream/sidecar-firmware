// pi/engine/spsc.h - a lock-free single-producer single-consumer ring of N (a power of 2) items: one core pushes,
// one core (or interrupt) pops. Acquire/release on the indices; no locks, no allocation.
#pragma once
#include <stdint.h>

template <class T, uint32_t N>
class Spsc {
  static_assert((N & (N - 1)) == 0, "N must be a power of 2");

 public:
  bool push(const T& x) {
    uint32_t w = __atomic_load_n(&wr_, __ATOMIC_RELAXED), r = __atomic_load_n(&rd_, __ATOMIC_ACQUIRE);
    if (w - r >= N) return false;
    buf_[w & (N - 1)] = x;
    __atomic_store_n(&wr_, w + 1, __ATOMIC_RELEASE);
    return true;
  }
  bool pop(T* x) {
    uint32_t r = __atomic_load_n(&rd_, __ATOMIC_RELAXED), w = __atomic_load_n(&wr_, __ATOMIC_ACQUIRE);
    if (r == w) return false;
    *x = buf_[r & (N - 1)];
    __atomic_store_n(&rd_, r + 1, __ATOMIC_RELEASE);
    return true;
  }
  uint32_t size() const { return __atomic_load_n(&wr_, __ATOMIC_ACQUIRE) - __atomic_load_n(&rd_, __ATOMIC_ACQUIRE); }

 private:
  T buf_[N];
  alignas(64) uint32_t wr_ = 0;   // separate cache lines: the producer's and the consumer's index
  alignas(64) uint32_t rd_ = 0;
};

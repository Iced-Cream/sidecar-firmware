// pi/engine/tests/test_spsc.cpp - the ring: FIFO order, full and empty, wrap-around, two threads.
#include <thread>

#include "check.h"
#include "spsc.h"

int main() {
  Spsc<int, 4> q;
  int x;
  CHECK(q.size() == 0 && !q.pop(&x));
  for (int i = 0; i < 4; ++i) CHECK(q.push(i));
  CHECK(!q.push(9) && q.size() == 4);
  for (int i = 0; i < 4; ++i) CHECK(q.pop(&x) && x == i);
  for (int r = 0; r < 10; ++r) { CHECK(q.push(r)); CHECK(q.pop(&x) && x == r); }   // wraps
  static Spsc<uint32_t, 256> big;
  std::thread prod([] { for (uint32_t i = 0; i < 1000000;) if (big.push(i)) ++i; });
  uint32_t want = 0, got;
  while (want < 1000000) if (big.pop(&got)) { CHECK(got == want); ++want; }
  prod.join();
  printf("test_spsc: PASS\n");
  return 0;
}

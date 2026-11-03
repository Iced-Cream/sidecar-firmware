// test_ob_speed.cpp - pi/engine/ob_speed.h (when to enumerate an Overbridge box again that came up below high speed)
// on the Mac: the waits, the budget, a new plug, a box that reaches high speed, the clock's wrap.
#include "../ob_speed.h"
#include "check.h"
#include <cstdio>

using namespace obspeed;

int main() {
  {  // the A4 / issue 11 case: refused, 2 s, refused again, 5 s, again, 10 s, then the budget is spent
    Retry r;
    uint32_t t = 1000;
    CHECK(refused(r, t) && r.at_ms == t + 2000);
    CHECK(!due(r, t + 1999) && due(r, t + 2000) && !due(r, t + 2001));   // once
    t += 2500;
    CHECK(refused(r, t) && r.at_ms == t + 5000 && r.tries == 1);
    CHECK(due(r, t + 5000));
    t += 5500;
    CHECK(refused(r, t) && r.at_ms == t + 10000);
    CHECK(due(r, t + 10000) && r.tries == 3);
    t += 10500;
    CHECK(!refused(r, t) && !r.pending);   // still at full speed after three: stays off
    CHECK(!due(r, t + 60000));
    CHECK(!refused(r, t + 20000));         // a refusal within kForgetMs: still spent
  }
  {  // a replug long after it gave up: the whole budget again
    Retry r;
    for (int k = 0; k < 3; ++k) { CHECK(refused(r, 1000 + k * 100)); CHECK(due(r, 1000000)); }
    CHECK(!refused(r, 2000));
    CHECK(refused(r, 2000 + kForgetMs + 1) && r.tries == 0 && r.at_ms == 2000 + kForgetMs + 1 + 2000);
  }
  {  // the box comes up at high speed: the budget is back and nothing stays pending
    Retry r;
    CHECK(refused(r, 0) && due(r, 2000) && refused(r, 2100));
    high_speed(r);
    CHECK(!r.pending && r.tries == 0 && !due(r, 100000));
    CHECK(refused(r, 3000) && r.at_ms == 5000);
  }
  {  // the ms clock wraps: the wait still counts across it
    Retry r;
    uint32_t t = 0xFFFFFFFFu - 500;
    CHECK(refused(r, t));
    CHECK(!due(r, t + 1000) && due(r, t + 2000));   // t + 2000 has wrapped
  }
  printf("test_ob_speed: ok\n");
  return 0;
}

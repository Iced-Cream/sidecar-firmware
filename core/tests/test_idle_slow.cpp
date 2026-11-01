// core/tests/test_idle_slow.cpp - what the control loop's slow-pass record names (core/engine.h IdleSlow, cardq::here_take,
// pi/engine/cores.cpp): the slowest step and the named steps' total per take, and the control core's
// time on the card: the three ways it holds up for a card operation, and nothing for a request it only posted.
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "msamp/cardq.h"
#include "platform.h"

using namespace core;

static void slept(cardq::Req* r) { usleep(3000); r->got = 1; }

static void noted(cardq::Req* r) { cardq::here_note(1000000); r->got = 1; }

int main() {
  idle_slowest_take();
  IdleSlow s = idle_slowest_take();
  CHECK(s.name[0] == 0 && s.us == 0 && s.sum == 0);

  idle_note("a", 100);
  idle_note("b", 300);
  idle_note("c", 200);
  s = idle_slowest_take();
  CHECK(!strcmp(s.name, "b") && s.us == 300 && s.sum == 600);
  s = idle_slowest_take();
  CHECK(s.name[0] == 0 && s.us == 0 && s.sum == 0);   // a take starts over

  cardq::reset();
  cardq::set_worker(true);
  cardq::here_take();
  static cardq::Req r;
  memset(&r, 0, sizeof r);
  r.kind = cardq::kFnReq, r.fn = slept;
  CHECK(cardq::post(&r));
  CHECK(cardq::here_take() == 0);                     // posted: no time here
  CHECK(cardq::take_back(&r));                        // taken back: the operation's time is this core's
  uint32_t a = cardq::here_take();
  CHECK(a >= 2500);
  CHECK(cardq::here_take() == 0);                     // a take clears it

  r.state = cardq::kFree;
  cardq::run_here(&r);                                // a full queue's way out
  CHECK(cardq::here_take() >= 2500);

  cardq::here_note(70);                               // a file call on this core (the Pi's platform)
  CHECK(cardq::here_take() == 70);

  r.fn = noted;                                       // a taken-back request's own file calls: counted with it, not twice
  r.state = cardq::kFree;
  CHECK(cardq::post(&r) && cardq::take_back(&r));
  CHECK(cardq::here_take() < 900000);

  r.state = cardq::kDone;
  cardq::wait(&r);                                    // nothing to wait for
  CHECK(cardq::here_take() < 1000);
  cardq::reset();
  printf("test_idle_slow: ok\n");
  return 0;
}

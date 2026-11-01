// pi/engine/tests/test_card_pass.cpp - card_pass.h: MIDI between the card requests of a pass, nothing when none waits, n at most.
#include <string>

#include "check.h"
#include "../card_pass.h"

int main() {
  std::string log;
  int left = 3;
  auto serve = [&] {
    if (left == 0) return 0;
    --left, log += 'c';
    return 1;
  };
  auto midi = [&] { log += 'm'; };
  CHECK(card_pass(8, serve, midi) == 3 && log == "cmcmcm");
  log.clear();
  CHECK(card_pass(8, serve, midi) == 0 && log.empty());      // an idle loop: no extra Poll
  left = 20;
  CHECK(card_pass(8, serve, midi) == 8 && left == 12 && log == "cmcmcmcmcmcmcmcm");
  printf("card_pass: PASS\n");
  return 0;
}

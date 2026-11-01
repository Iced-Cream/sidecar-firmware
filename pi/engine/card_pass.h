// pi/engine/card_pass.h - the card queue's turn in core 0's loop (bridge::card_service, kernel.cpp): up to n requests, one at a time, with `midi` after each that
// ran. A burst of 16-32 reads is 45-90 ms, and the loop's one AsampMidiSend and Transport Poll waited behind it, so USB-A and DIN notes did (audio is in an
// IRQ). A pass with no request costs one look, so an idle loop polls as often as before (usbmidi_merge.h counts in Polls). Portable: tests/test_card_pass.cpp.
#pragma once

// serve(): a request run (1), or none waiting (0); the requests run
template <class TServe, class TMidi>
inline int card_pass(int n, TServe serve, TMidi midi) {
  int done = 0;
  for (; done < n && serve() != 0; ++done) midi();
  return done;
}

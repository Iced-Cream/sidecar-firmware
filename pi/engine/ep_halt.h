// pi/engine/ep_halt.h - an xHCI endpoint halted by a transfer error, brought back at task level. On a non-isochronous
// endpoint a transfer error (4: a USB transaction error, three tries without a handshake; 6: a STALL) halts it: the URB
// that failed completes with status 0 and every URB queued behind it, or submitted later, stays on the stopped ring for
// good (a tester's Pi 5, 0.4.0: "xhciep: Transfer error 4 on endpoint 6", the Overbridge OUT, and the voices stopped).
// The completion (IRQ) calls Fail and stops resubmitting. The main loop (task level, IRQs on: the xHCI command waits for
// its event) calls Reset. On a halted endpoint Reset has the caller send CLEAR_FEATURE(ENDPOINT_HALT) to the box first:
// the Reset Endpoint sets the host's data toggle to DATA0, and the box's must follow, or it drops the next packet as a
// repeat (as Linux's usb_clear_halt and usb=mirror's MirrorPipePoll do). On 1, with the IRQ held off, Finish: when it
// returns true the reset skipped every queued URB (DropURBs, circle-xhci-dropurbs.patch), so all of the caller's are
// free; then the caller submits again. A reset the controller refuses (-1) is tried again a second later. X: CXHCIEndpoint (GetEPState, ResetFromHalted, DropURBs).
// Portable: tests/test_ob_usb.cpp.
#pragma once

template <class X>
struct EpHalt {
  static constexpr unsigned kRetryUs = 1000000;
  volatile bool halted = false;
  bool refused = false;
  unsigned resets = 0, refusals = 0, state = 0, dropped = 0, last_us = 0;

  void Fail() { halted = true; }   // the completion of a failed URB (IRQ)

  // 0: nothing to do (not halted, or a refused reset less than a second ago); -1: refused now; 1: call Finish
  template <class Clear>
  int Reset(X* x, unsigned now_us, Clear clear) {
    if (!halted || (refused && now_us - last_us < kRetryUs)) return 0;
    last_us = now_us;
    state = x->GetEPState();   // 1 running (the error did not halt it), 2 halted, 3 stopped, 4 error
    if (state >= 2) clear();   // the box's side (its answer does not matter: a box that did not halt it ignores it)
    refused = state >= 2 && !x->ResetFromHalted();
    if (refused) return ++refusals, -1;
    return ++resets, 1;
  }

  // the IRQ held off, after Reset gave 1
  bool Finish(X* x) {
    bool all = state >= 2;
    dropped = all ? x->DropURBs() : 0;
    halted = false;
    return all;
  }
};

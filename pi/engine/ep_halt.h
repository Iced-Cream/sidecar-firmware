// pi/engine/ep_halt.h - an xHCI endpoint halted by a transfer error, brought back at task level. On a non-isochronous
// endpoint a transfer error (4: a USB transaction error, three tries without a handshake; 6: a STALL) halts it: the URB
// that failed completes with status 0 and every URB queued behind it, or submitted later, stays on the stopped ring for
// good (a tester's Pi 5, 0.4.0: "xhciep: Transfer error 4 on endpoint 6", the Overbridge OUT, and the voices stopped).
//
// The completion (IRQ) calls Fail on an error and Ok on a good transfer, and does not resubmit while `halted`. The main
// loop (task level, IRQs on: the xHCI commands wait for their events) may also call Stall when URBs are pending and none
// completed for kStallUs: an endpoint can go quiet with no error completion. Then Reset:
//   - backs off while the endpoint keeps failing: the resets in a row with no good completion between them wait 0,
//     10 ms, 100 ms, then 1 s (a dying cable or a box that STALLs everything costs a few resets a second, not one a pass);
//   - on a halted endpoint (state 2, 3 or 4) has the caller send CLEAR_FEATURE(ENDPOINT_HALT) to the box first (the
//     Reset Endpoint sets the host's data toggle to DATA0, and the box's must follow, or it drops the next packet as a
//     repeat: Linux's usb_clear_halt does the same), then ResetFromHalted;
//   - reads the state again after it: Circle's ResetFromHalted reports a failed command as success (it returns !code),
//     so a state of 2 or 4 afterwards is a refused reset, tried again after the back-off.
// On 1, with the IRQ held off, Finish: the reset skipped every queued URB (DropURBs, circle-xhci-dropurbs.patch), so all
// of the caller's are free and it submits them again. On 2 the endpoint was not halted on the xHC (state 1): nothing was
// reset or dropped, and the caller only resubmits what is free (not counted as a reset). Log: the first kLogFirst lines of
// a run, then one every kLogEveryUs, with the number left out. X: CXHCIEndpoint (GetEPState, ResetFromHalted, DropURBs).
// Portable: tests/test_ob_usb.cpp.
#pragma once

template <class X>
struct EpHalt {
  static constexpr unsigned kStallUs = 50000, kLogFirst = 3, kLogEveryUs = 10000000;
  volatile bool halted = false;
  volatile unsigned fails = 0;   // resets in a row with no good completion between them
  unsigned resets = 0, refusals = 0, stalls = 0, quiet = 0, state = 0, dropped = 0, last_us = 0, last_log_us = 0;

  void Fail() { halted = true; }   // a failed URB's completion (IRQ)
  void Ok() { fails = 0; }         // a good one (IRQ)
  void Stall() { halted = true, ++stalls; }   // task level: URBs pending, none completed for kStallUs

  static unsigned Wait(unsigned n) { return n == 0 ? 0 : n == 1 ? 10000 : n == 2 ? 100000 : 1000000; }
  bool Due(unsigned now_us) const { return halted && now_us - last_us >= Wait(fails); }

  // 0: nothing to do now (not halted, or backing off); -1: refused; 1: reset, call Finish; 2: not halted on the xHC,
  // call Finish (resubmit only)
  template <class Clear>
  int Reset(X* x, unsigned now_us, Clear clear) {
    if (!Due(now_us)) return 0;
    last_us = now_us;
    if (fails < 1000) fails = fails + 1;
    state = x->GetEPState();   // 1 running, 2 halted, 3 stopped, 4 error
    if (state < 2) return 2;
    clear();                   // the box's side (its answer does not matter: a box that did not halt it ignores it)
    unsigned after = x->ResetFromHalted() ? x->GetEPState() : 4;
    if (after == 2 || after == 4) return ++refusals, -1;
    return ++resets, 1;
  }

  // the IRQ held off, after Reset gave 1 or 2: true when every queued URB was dropped (all of the caller's are free)
  bool Finish(X* x) {
    bool all = state >= 2;
    dropped = all ? x->DropURBs() : 0;
    halted = false;
    return all;
  }

  // a line for this reset? (false: counted in quiet, told with the next line)
  bool Log(unsigned now_us) {
    if (fails <= kLogFirst || now_us - last_log_us >= kLogEveryUs) return last_log_us = now_us, true;
    ++quiet;
    return false;
  }
  unsigned TakeQuiet() {
    unsigned q = quiet;
    quiet = 0;
    return q;
  }
};

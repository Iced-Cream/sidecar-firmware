// pi/engine/ob_freeze.h - when to re-enumerate an Overbridge box whose stream froze while the box stayed on the bus,
// for ob_usb.cpp (CObUsb::PollHalt) and the Mac's unit test (pi/engine/tests/test_ob_freeze.cpp).
//
// Two boxes on the Pi 4's USB-A ports (the VL805 hub): unplugging one can freeze the other (KB T469 SR1, T472 SR2,
// 2026-11-22, either box, the default build df2f029 too). The box that stays is still enumerated, but its isochronous
// IN / OUT stop at the same millisecond (0/s), its control and MIDI transfers time out, the engine renders nothing,
// and the endpoint loop's resubmits ("quiet or failed but not halted: resubmitted", ep_halt.h) never bring it back:
// 41 in a row over 75 s in SR1. The hub did not reset its port. When it does (AO1, T464: the port re-enumerated 6 s
// after the other box's unplug), or when the unplugged box is plugged in again (SR2: the hub re-enumerated the frozen
// box's port), the box streams again at once. So the escalation here is the software version of that: remove the
// device (the hub port disabled, the xHCI slot freed, this driver deleted, as for an unplug) and have its hub
// enumerate it again (port reset, a fresh slot and address). The attach-order slot hold (5 s, core/device) keeps its
// slot meanwhile.
//
// When: the stream's endpoint loop has been resetting or resubmitting with no good completion between (EpHalt::fails
// > 0, the larger of IN and OUT) for kHoldMs. The known short stall two boxes show 4 to 8 s after they start (about
// 846 ms) is well under it, and a box being unplugged is removed by Circle's own hot-plug path long before (its
// endpoints fail together and EpBox holds their resets for the hub). kTries per slot; a slot that streams for
// kForgetMs after its last try has the whole budget again. A spent budget leaves the endpoint loop going, and a
// replug by hand.
//
// Freestanding C++ (stdint.h only), so the timeline can be replayed on the Mac.
#ifndef SIDECAR_OB_FREEZE_H
#define SIDECAR_OB_FREEZE_H

#include <stdint.h>

namespace obfreeze {

static const uint32_t kHoldMs = 4000;
static const uint32_t kForgetMs = 60000;
static const unsigned kTries = 3;

// per stream (CObUsb object): the current failing run
struct Watch
{
	bool run = false;	// a run of resets / resubmits with no good completion is going on
	uint32_t run_ms = 0;	// since when
};

// per slot, outlives the CObUsb that a re-enumeration deletes
struct Budget
{
	unsigned tries = 0;
	bool seen = false;	// a try happened (last_ms is valid)
	uint32_t last_ms = 0;	// the last try
	bool told = false;	// "spent" logged for this run of tries
};

// The main loop, every pass, for a streaming box: fails = its endpoints' resets in a row with no good completion
// (the larger of IN and OUT). true: re-enumerate it now (a try spent; w and b updated).
inline bool pass (Watch &w, Budget &b, unsigned fails, uint32_t now_ms)
{
	if (fails == 0)
	{
		w.run = false;
		if (b.seen && now_ms - b.last_ms > kForgetMs) b = Budget ();
		return false;
	}
	if (!w.run)
	{
		w.run = true;
		w.run_ms = now_ms;
		return false;
	}
	if ((int32_t) (now_ms - w.run_ms) < (int32_t) kHoldMs || b.tries >= kTries) return false;
	b.tries++;
	b.seen = true;
	b.last_ms = now_ms;
	w.run = false;
	return true;
}

// the budget is spent and the freeze still goes on: true once (the caller logs that a replug by hand is needed)
inline bool spent_now (const Watch &w, Budget &b, uint32_t now_ms)
{
	if (b.tries < kTries || b.told || !w.run || (int32_t) (now_ms - w.run_ms) < (int32_t) kHoldMs) return false;
	b.told = true;
	return true;
}

}	// namespace obfreeze

#endif

// pi/engine/ob_speed.h - when to re-enumerate an Overbridge box that came up below high speed, for ob_usb.cpp and the
// Mac's unit test (pi/engine/tests/test_ob_speed.cpp).
//
// Overbridge audio needs a high-speed link. A box can still enumerate at full speed: an A4 restarted with the Pi
// running (2026-11-02), a Rytm plugged in after boot (issue 11), both "found (FS)", then "cannot submit the first
// URBs" and the box gone for good. A replug fixed both. Most likely the port's high-speed handshake (the chirp at
// the port reset) failed because the box's USB was still starting [inferred]. So CObUsb::Configure refuses a box
// below high speed and the main loop has its root port enumerate it again (a new port reset, so a new chirp) a few
// seconds later: the hot-plug path the controller already takes when something is plugged in.
//
// Freestanding C++ (stdint.h only), so the timeline can be replayed on the Mac.
#ifndef SIDECAR_OB_SPEED_H
#define SIDECAR_OB_SPEED_H

#include <stdint.h>

namespace obspeed {

// The waits before each re-enumeration: the box gets longer to finish starting its USB each time. Three in all; a
// box that is still at full speed after that has a cable, port or hub that only runs at full speed, and more
// resets will not change it.
static const unsigned kTries = 3;
static const unsigned kWaitMs[kTries] = {2000, 5000, 10000};

// A refusal this long after the last retried one is a new plug, not one of our retries (each comes back within
// about a second of its re-enumeration): the budget is full again.
static const unsigned kForgetMs = 30000;

struct Retry
{
	unsigned tries = 0;	// re-enumerations spent on this plug
	bool pending = false;	// one is scheduled
	uint32_t at_ms = 0;	// when it is due
	bool seen = false;	// a refusal happened (last_ms is valid)
	uint32_t last_ms = 0;	// the last refusal
};

// Configure refused a box below high speed at now_ms. true: a re-enumeration is scheduled (at r.at_ms); false: the
// budget for this plug is spent and the box stays off.
inline bool refused (Retry &r, uint32_t now_ms)
{
	if (r.seen && now_ms - r.last_ms > kForgetMs) r.tries = 0;
	r.seen = true;
	if (r.tries >= kTries)
	{
		r.pending = false;		// (last_ms stays at the last retried refusal: replugs do not push it on)
		return false;
	}
	r.last_ms = now_ms;
	r.pending = true;
	r.at_ms = now_ms + kWaitMs[r.tries];
	return true;
}

// The main loop, every pass: true once when the scheduled re-enumeration is due (and spends a try).
inline bool due (Retry &r, uint32_t now_ms)
{
	if (!r.pending || (int32_t) (now_ms - r.at_ms) < 0) return false;
	r.pending = false;
	r.tries++;
	return true;
}

// A box came up at high speed: whatever was pending is moot and the next plug starts with the whole budget.
inline void high_speed (Retry &r)
{
	r = Retry ();
}

}	// namespace obspeed

#endif

// pi/engine/tests/test_vl805_probe.cpp - vl805_probe.h: events at 125 us, a 2.048 s stall replayed as the unit shows it
// (all streams quiet, MFINDEX running on), stall detection, the back line's microframe deltas, the MFINDEX-against-clock
// deviation, the history before the stall and the per-second numbers.
#include <cstdio>

#include "../vl805_probe.h"
#include "check.h"

using namespace vl805;

int main ()
{
	{	// mf_ahead wraps at 2^14
		CHECK (mf_ahead (16380, 4) == 8);
		CHECK (mf_ahead (100, 100) == 0);
		CHECK (mf_ahead (5, 4) == kMfMask);
	}
	{	// streaming, then a stall of one wrap: Began after kStallUs, Still every kStillUs, Back with the microframe deltas
		static Probe p;
		uint32_t t = 1000000, mf = 1000;
		for (int i = 0; i < 8000; i++, t += 125, mf = (mf + 1) & kMfMask)	// 1 s, both boxes, IN and OUT
		{
			p.Sample (t, mf);
			for (unsigned k = 0; k < 2; k++)
				for (unsigned d = 0; d < 2; d++) p.OnEvent (k, d, t, mf, 8);
			CHECK (p.Check (t, true) == Probe::None);
		}
		const uint32_t last_t = t - 125, last_mf = (mf - 1) & kMfMask;
		CHECK (p.last_us == last_t && p.last_mf == last_mf);
		CHECK (p.Check (last_t + kStallUs, true) == Probe::None);	// 20 ms: not yet
		CHECK (p.Check (last_t + kStallUs + 1000, false) == Probe::None);	// nothing queued: no stall
		CHECK (p.Check (last_t + kStallUs + 1000, true) == Probe::Began);
		CHECK (p.stalled && p.stalls == 1 && p.stall_last_us == last_t && p.stall_last_mf == last_mf);
		CHECK (p.stall_ep_mf[1][1] == last_mf);
		CHECK (p.Check (last_t + kStallUs + 2000, true) == Probe::None);
		CHECK (p.Check (last_t + kStallUs + 1000 + kStillUs, true) == Probe::Still);
		CHECK (p.Check (last_t + kStallUs + 1100 + kStillUs, true) == Probe::None);
		// MFINDEX ran on through it; the first event comes 2.048 s + 2 uframes after the last
		const uint32_t back_t = last_t + kWrapUs + 250, back_mf = (last_mf + kMfMask + 1 + 2) & kMfMask;
		p.Sample (back_t, back_mf);
		p.OnEvent (0, 0, back_t, back_mf, 8);
		CHECK (p.Check (back_t, true) == Probe::Back);
		CHECK (!p.stalled);
		CHECK (p.BackUs () == kWrapUs + 250);
		CHECK (p.BackMf () == 2);			// back at the MFINDEX it stopped at: the wait for one wrap
		CHECK (p.BackMfByClock () == 2);		// and MFINDEX did run on by the clock
		CHECK (p.Check (back_t + 125, true) == Probe::None);
	}
	{	// the history: the buckets before the stall, oldest first; a ring run dry (1 queued) and a late IRQ show in them
		static Probe p;
		uint32_t t = 5000000;
		for (int i = 0; i < 400; i++, t += 125) p.OnEvent (0, 0, t, i, i == 300 ? 1 : 8);
		p.OnEvent (0, 1, t + 3000, 0, 8);		// 3 ms without an event
		Bucket h[kBuckets];
		const unsigned n = p.History (h, kBuckets);
		CHECK (n >= 5 && n <= kBuckets);
		unsigned ev = 0, pmin = 255, gap = 0;
		for (unsigned i = 0; i < n; i++)
		{
			ev += h[i].events;
			if (h[i].pend_min < pmin) pmin = h[i].pend_min;
			if (h[i].gap_max_us > gap) gap = h[i].gap_max_us;
			if (i > 0) CHECK (h[i].start_us > h[i - 1].start_us);
		}
		CHECK (ev == 401 && pmin == 1 && gap == 3125);
		uint32_t g, d;
		uint8_t pm[kBoxes][kDirs];
		p.TakeSecond (&g, &d, pm);
		CHECK (g == 3125 && pm[0][0] == 1 && pm[0][1] == 8 && pm[1][0] == 255);
		p.TakeSecond (&g, &d, pm);
		CHECK (g == 0 && pm[0][0] == 255);
	}
	{	// the main loop's MFINDEX against the clock: on time, then a counter that stood still for 1 ms
		static Probe p;
		p.Sample (1000000, 100);
		p.Sample (1001000, 108);			// 1 ms, 8 uframes: on time
		CHECK (p.dev_max_us == 0);
		p.Sample (1003000, 116);			// 2 ms, 8 uframes: 1 ms behind
		CHECK (p.dev_max_us == 1000 && p.dev_at_us == 1003000);
		p.Sample (1003000 + kWrapUs, 116);		// a whole wrap later: ambiguous, not judged
		CHECK (p.dev_max_us == 1000);
	}
	printf ("test_vl805_probe: all passed\n");
	return 0;
}

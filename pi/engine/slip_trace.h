// pi/engine/slip_trace.h - the relay's per-block timing around a slip (docs/teensy-mirror.md, TR16): the A4 slips 1-18
// samples through the relay now and then, and no 1 s or 10 s counter tells the slip's seconds from the others (T365/T366).
// This keeps every block event of the last ~100 ms (the box's IN as it arrives, each OUT block filled for the box and its
// completion, each computer OUT block off the link with the OUT ring's level) and freezes them a little after the box-IN
// slip detector (slip_detect.h) fires, so the log shows what the box was given just before it slipped. Portable;
// tests/test_slip_trace.cpp.
//
// Threads: add/trigger in the xHCI IRQ (core 0); ready/line/done in core 0's loop. The ring is only read once frozen (the
// IRQ then writes nothing), and the loop hands it back with done().
#ifndef _slip_trace_h
#define _slip_trace_h

#include <stdint.h>
#include <stddef.h>
#if __has_include(<circle/memorymap.h>)
extern "C" int core_snprintf (char *out, size_t n, const char *fmt, ...);	// platform_pi.cpp (Circle has no snprintf)
#define SLIPTRACE_FMT core_snprintf
#else
#include <stdio.h>
#define SLIPTRACE_FMT snprintf
#endif

struct SlipTrace
{
	enum Kind : uint8_t {
		kIn = 'I',	// a box IN block arrived (v: its counter / 7, mod 1000)
		kFill = 'F',	// an OUT block filled for the box (v: the OUT ring's level, frames)
		kOutDone = 'O',	// an OUT transfer to the box completed (v: OUT transfers still in flight)
		kPc = 'C',	// a computer OUT block arrived off the link (v: the OUT ring's level after it, frames)
		kSlip = '!',	// the box-IN detector's slip (v: its size, 1/100 sample, offset by 10000)
	};
	static constexpr unsigned kN = 2048;	// ~100 ms at ~20.5k events/s (IN, fill, OUT done, computer: ~6.9k/s each)
	static constexpr unsigned kPost = 160;	// events kept after the trigger (~8 ms)
	static constexpr unsigned kPerLine = 40;

	struct Rec { uint32_t t_us; uint16_t v; uint8_t kind; uint8_t pad; };

	Rec r[kN];
	volatile unsigned n = 0;		// events written (the next goes to r[n % kN])
	volatile unsigned state = 0;		// 0 recording, 1 after the trigger (post_left more), 2 frozen (the loop dumps it)
	volatile unsigned post_left = 0;
	uint32_t trig_us = 0;
	int32_t trig_size = 0;
	unsigned trig_ms = 0;
	unsigned frozen_n = 0;			// n when frozen
	unsigned triggers = 0, skipped = 0;	// slips that froze a trace; slips while one was pending or being dumped
	unsigned line_k = 0;			// the next line the loop writes

	void add (Kind k, uint32_t t, unsigned v)
	{
		if (state == 2) return;
		Rec &e = r[n % kN];
		e.t_us = t, e.v = (uint16_t) (v > 0xFFFF ? 0xFFFF : v), e.kind = (uint8_t) k;
		n = n + 1;
		if (state == 1 && --post_left == 0) { frozen_n = n; line_k = 0; state = 2; }
	}
	// the detector saw a slip (size: 1/100 sample) at t: the trace freezes kPost events later; one at a time
	void trigger (uint32_t t, int32_t size, unsigned ms)
	{
		if (state != 0) { skipped++; return; }
		int32_t s = size + 10000;
		add (kSlip, t, s < 0 ? 0 : (unsigned) s);
		trig_us = t, trig_size = size, trig_ms = ms;
		triggers++;
		post_left = kPost;
		state = 1;
	}
	bool ready () const { return state == 2; }
	unsigned events () const { return frozen_n < kN ? frozen_n : kN; }
	unsigned lines () const { return (events () + kPerLine - 1) / kPerLine; }
	// line k of the frozen trace into buf: "k/K" then each event as kind, us since the previous one, '/', value (the first
	// event's time is relative to the trigger); FALSE when k is past the end
	bool line (unsigned k, char *buf, unsigned cap) const
	{
		const unsigned m = events (), first = frozen_n - m;
		if (k * kPerLine >= m || cap < 32) return false;
		unsigned p = (unsigned) SLIPTRACE_FMT (buf, cap, "%u/%u", k + 1, lines ());
		for (unsigned j = k * kPerLine; j < m && j < (k + 1) * kPerLine && p + 20 < cap; j++)
		{
			const Rec &e = r[(first + j) % kN];
			int32_t dt = j == k * kPerLine ? (int32_t) (e.t_us - trig_us)
						    : (int32_t) (e.t_us - r[(first + j - 1) % kN].t_us);
			p += (unsigned) SLIPTRACE_FMT (buf + p, cap - p, " %c%ld/%u", (char) e.kind, (long) dt, (unsigned) e.v);
		}
		return true;
	}
	void done () { state = 0; n = 0; }
};

#endif

// pi/engine/tests/test_slip_trace.cpp - slip_trace.h: make build/test_slip_trace
#include "slip_trace.h"
#include "check.h"
#include <cstdio>
#include <cstring>
#include <string>

int main ()
{
	static SlipTrace t;
	{	// nothing frozen without a trigger; the ring wraps
		for (unsigned i = 0; i < 3 * SlipTrace::kN; i++) t.add (SlipTrace::kIn, i * 50, i % 1000);
		CHECK (!t.ready ());
	}
	{	// a trigger freezes kPost events later: the last kN events, the slip among them, kPost after it
		t.trigger (200000, -183, 4000);
		for (unsigned i = 0; i < SlipTrace::kPost - 1; i++) t.add (SlipTrace::kOutDone, 200000 + i * 10, 3);
		CHECK (!t.ready ());
		t.add (SlipTrace::kPc, 300000, 96);
		CHECK (t.ready ());
		t.add (SlipTrace::kIn, 400000, 1);		// frozen: ignored
		t.trigger (400001, 5, 4001);			// one at a time: counted as skipped
		CHECK (t.skipped == 1 && t.triggers == 1);
		CHECK (t.events () == SlipTrace::kN);
		CHECK (t.lines () == (SlipTrace::kN + SlipTrace::kPerLine - 1) / SlipTrace::kPerLine);
		char b[640];
		std::string all;
		unsigned k = 0;
		while (t.line (k, b, sizeof b)) { CHECK (strlen (b) < 600); all += b; all += '\n'; k++; }
		CHECK (k == t.lines ());
		CHECK (all.find ("/9817") != std::string::npos && all.find (" !") != std::string::npos);	// the slip, its size + 10000
		CHECK (all.find (" C") != std::string::npos);
		size_t last = all.rfind ('\n', all.size () - 2);
		printf ("first: %.80s\nlast: %s", all.c_str (), all.c_str () + last + 1);
		t.done ();
		CHECK (!t.ready () && t.n == 0);
	}
	{	// after done() a new slip can freeze again
		for (unsigned i = 0; i < 10; i++) t.add (SlipTrace::kIn, i, i);
		t.trigger (10, 100, 1);
		for (unsigned i = 0; i < SlipTrace::kPost; i++) t.add (SlipTrace::kFill, 11 + i, 50);
		CHECK (t.ready () && t.events () == 11 + SlipTrace::kPost && t.triggers == 2);
		char b[640];
		CHECK (t.line (0, b, sizeof b) && strncmp (b, "1/", 2) == 0);
		CHECK (strstr (b, " I-10/0") != nullptr);	// the first event's time from the trigger
	}
	puts ("test_slip_trace: ok");
	return 0;
}

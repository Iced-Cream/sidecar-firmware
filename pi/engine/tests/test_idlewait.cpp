// pi/engine/tests/test_idlewait.cpp - idlewait.h, and a model of wfe / sev: every interleaving of a waiter and a signaller
// on the architecture's one-bit event register, so the handshakes in cores.cpp are shown free of a lost wake-up, and the
// order that would lose one is shown to (the model can fail). A model, not the hardware: real timing is the owner's test.
#include "check.h"
#include "idlewait.h"
#include <set>
#include <tuple>

using idlewait::nap;
using idlewait::spend;
using idlewait::wait;

// The event register and a wfe: an event is spent, none blocks (until an interrupt or the stream, counted, not modelled).
struct EventReg
{
	bool bEvent = false;
	unsigned nBlocks = 0, nWfe = 0, nRaiseAfter = 0;	// nRaiseAfter: an exception return right after that many wfe
	void Wfe (void)
	{
		if (bEvent) bEvent = false;
		else nBlocks++;
		if (++nWfe == nRaiseAfter) bEvent = true;
	}
};

static void TestNap (void)
{
	{	// the round's spin lock left the event: a plain wfe falls through (the 1.2 million wakes a second), a nap sleeps
		EventReg Plain;
		Plain.bEvent = true;
		Plain.Wfe ();
		CHECK (Plain.nBlocks == 0);
		EventReg R;
		R.bEvent = true;
		nap ([&] { R.bEvent = true; }, [&] { R.Wfe (); });
		CHECK (R.nWfe == 2 && R.nBlocks == 1 && !R.bEvent);
	}
	{	// nothing pending: the same one sleep, and no event left behind
		EventReg R;
		nap ([&] { R.bEvent = true; }, [&] { R.Wfe (); });
		CHECK (R.nWfe == 2 && R.nBlocks == 1 && !R.bEvent);
	}
	{	// an interrupt between the two: its return sets the event, the nap ends there, no sleep after the handler
		EventReg R;
		R.nRaiseAfter = 1;
		nap ([&] { R.bEvent = true; }, [&] { R.Wfe (); });
		CHECK (R.nBlocks == 0 && !R.bEvent);
	}
}

static void TestSpend (void)
{
	{	// nothing else to run: a sleep of 152 us at a time, the deadline passed by less than one sleep
		unsigned t = 5000, nYield = 0, nSleep = 0;
		unsigned n = spend (5000, 1000, [&] { return t; }, [&] { nYield++; }, [&] { t += 152; nSleep++; });
		CHECK (n == nSleep && n == 7);
		CHECK (nYield == nSleep + 1);			// a yield first and after every sleep
		CHECK (t - 5000 >= 1000 && t - 5000 < 1000 + 152);
	}
	{	// the sevs of rendering wake it early: more passes, no later
		unsigned t = 0, nYield = 0;
		spend (0, 1000, [&] { return t; }, [&] { nYield++; t += 2; }, [&] { t += 40; });
		CHECK (nYield > 20 && t < 1000 + 42);
	}
	{	// a task that takes the whole time: no sleep past the deadline
		unsigned t = 0, nSleep = 0;
		CHECK (spend (0, 1000, [&] { return t; }, [&] { t += 5000; }, [&] { nSleep++; }) == 0);
		CHECK (nSleep == 0);
	}
	{	// the microsecond clock wraps
		unsigned t = ~0u - 100, n0 = t;
		unsigned n = spend (n0, 1000, [&] { return t; }, [] {}, [&] { t += 152; });
		CHECK (n == 7 && t - n0 >= 1000);
	}
}

static void TestWait (void)
{
	{	// done after three wakes; asked after each
		unsigned nWakes = 0;
		CHECK (wait ([&] { return nWakes == 3; }, [] { return false; }, [&] { nWakes++; }, true));
		CHECK (nWakes == 3);
	}
	{	// done already: no sleep
		unsigned nWakes = 0;
		CHECK (wait ([] { return true; }, [] { return true; }, [&] { nWakes++; }, true));
		CHECK (nWakes == 0);
	}
	{	// the 20 ms limit still fires while sleeping
		unsigned t = 0;
		CHECK (!wait ([] { return false; }, [&] { return t > 1000; }, [&] { t += 152; }, true));
		CHECK (t == 1064);
	}
	{	// idlewfe=0: no sleep at all, the limit too
		unsigned t = 0, nWakes = 0;
		CHECK (!wait ([] { return false; }, [&] { return ++t > 9; }, [&] { nWakes++; }, false));
		CHECK (nWakes == 0);
	}
}

// The model. Waiter: 0 reads the word (written: done), then optionally 1 a spin lock's own wfe, which spends a pending
// event, then 2 wfe (no event: blocked; an event clears it and goes back to 0). Signaller: its two steps in the given order.
// Stream: the timer's event may fire at any time. A deadlock: the signaller is finished and the waiter blocked for good.
struct Model
{
	bool bSevFirst, bInnerWfe, bStream;
	std::set<std::tuple<int, int, bool, bool>> Seen;
	bool bStuck = false, bDone = false;

	void Go (int w, int s, bool bWord, bool bEvent)
	{
		if (!Seen.insert (std::make_tuple (w, s, bWord, bEvent)).second) return;
		const bool bBlocked = w == 2 && !bEvent;
		if (w == 3) bDone = true;
		if (s == 2 && bBlocked && !bStream) bStuck = true;
		if (w == 0) Go (bWord ? 3 : (bInnerWfe ? 1 : 2), s, bWord, bEvent);
		if (w == 1) Go (2, s, bWord, false);		// (it waits on its own lock's release: any event gone)
		if (w == 2 && bEvent) Go (0, s, bWord, false);
		if (s < 2)
		{
			const bool bStore = (s == 0) != bSevFirst;
			Go (w, s + 1, bWord || bStore, bEvent || !bStore);
		}
		if (bStream && !bEvent) Go (w, s, bWord, true);
	}
	static Model Run (bool bSevFirst, bool bInnerWfe, bool bStream)
	{
		Model m {bSevFirst, bInnerWfe, bStream, {}};
		m.Go (0, 0, false, false);
		return m;
	}
};

static void TestModel (void)
{
	// store, dsb, sev, then check right before wfe: the worker's s_nGen wait and core 3's s_nDone wait. No stream needed
	Model m = Model::Run (false, false, false);
	CHECK (!m.bStuck && m.bDone);
	// the same with a sev before the store: the bug the order avoids (the model can fail)
	CHECK (Model::Run (true, false, false).bStuck);
	// the control loop: a whole pass between its checks and its wfe, spin locks in it: an inner wfe can spend the sev.
	// That is the FIFO silence in docs/pi-engine.md: stuck without the stream, bound with it
	CHECK (Model::Run (false, true, false).bStuck);
	m = Model::Run (false, true, true);
	CHECK (!m.bStuck && m.bDone);
}

int main (void)
{
	TestNap ();
	TestSpend ();
	TestWait ();
	TestModel ();
	printf ("idlewait: OK\n");
	return 0;
}

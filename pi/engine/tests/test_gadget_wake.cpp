// test_gadget_wake.cpp - pi/circle/gadget_wake.h (the DWC2 gadget's bus suspend and wake-up, circle-gadget-wake.patch) on
// the Mac: the sequences a host can produce, replayed against the gadget's states.
#include "../../circle/gadget_wake.h"
#include "check.h"
#include <cstdio>

using namespace gadgetwake;

enum { Powered, Suspended, ResetDone, EnumDone, Configured };

// the gadget's handlers, as dwusbgadget.cpp has them
struct Gadget
{
	Tracker t;
	int state = Suspended;				// as Initialize leaves it, before the first bus reset

	void suspend (bool bStill = true)	{ if (t.Suspend (state, Powered, Suspended, bStill)) state = Suspended; }
	void wake (void)			{ int n = t.Wake (); if (n != kNone) state = n; }
	void reset (void)			{ t.Reset (); state = ResetDone; }
	void endpoint (void)			{ if (state == Suspended && t.Pending ()) wake (); }	// an EP interrupt
	bool deaf (void) const			{ return state == Suspended; }				// SETUPs dropped
};

int main ()
{
	{	// a Mac sleeps and wakes without a reset: the configured device is back, not deaf
		Gadget g;
		g.reset (); g.state = Configured;
		g.suspend ();
		CHECK (g.deaf ());
		g.wake ();
		CHECK (g.state == Configured);
		CHECK (!g.t.Pending ());
	}
	{	// the wake-up interrupt missed: the first SETUP's interrupt wakes it
		Gadget g;
		g.reset (); g.state = Configured;
		g.suspend ();
		g.endpoint ();
		CHECK (g.state == Configured);
	}
	{	// unplugged while streaming, plugged in again: suspend, then a reset; nothing left to go back to
		Gadget g;
		g.reset (); g.state = Configured;
		g.suspend ();
		g.reset ();
		CHECK (g.state == ResetDone);
		g.wake ();
		CHECK (g.state == ResetDone);
	}
	{	// before any bus reset the suspend at start-up has nothing to go back to: a wake-up changes nothing
		Gadget g;
		g.suspend ();
		g.wake ();
		g.endpoint ();
		CHECK (g.state == Suspended);
	}
	{	// a suspend during enumeration comes back there
		Gadget g;
		g.reset (); g.state = EnumDone;
		g.suspend ();
		g.wake ();
		CHECK (g.state == EnumDone);
	}
	{	// the suspend interrupt runs after the bus resumed (both pending, the wake-up handled first): not suspended
		Gadget g;
		g.reset (); g.state = Configured;
		g.suspend (false);
		CHECK (g.state == Configured);
		CHECK (!g.t.Pending ());
	}
	{	// a second suspend interrupt while suspended keeps the state to go back to
		Gadget g;
		g.reset (); g.state = Configured;
		g.suspend ();
		g.suspend ();
		g.wake ();
		CHECK (g.state == Configured);
	}
	{	// a wake-up that comes with a reset (replug while the host is awake): the reset wins, whichever runs first
		Gadget g;
		g.reset (); g.state = Configured;
		g.suspend ();
		g.wake ();
		g.reset ();
		CHECK (g.state == ResetDone);
	}
	puts ("gadget_wake: ok");
	return 0;
}

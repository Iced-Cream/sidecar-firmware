// pi/circle/gadget_wake.h - what a USB gadget does when the bus sleeps and wakes, for Circle's DWC2 gadget
// (pi/circle/circle-gadget-wake.patch; build.sh copies this file into the Circle tree as
// include/circle/usb/gadget/gadgetwake.h) and the Mac's unit test (pi/engine/tests/test_gadget_wake.cpp). No Circle types.
//
// A bus suspend moves the gadget to its suspended state, where endpoint interrupts are acknowledged and dropped (a
// transfer's late completion must not run on a torn-down stream). A host that resumes without a reset (a Mac waking)
// then talks to a gadget that drops its SETUPs: deaf until the host gives up and resets. So the state before the
// suspend is kept here: a wake-up, or any endpoint activity, goes back to it; a bus reset forgets it.
#ifndef _gadget_wake_h
#define _gadget_wake_h

namespace gadgetwake {

constexpr int kNone = -1;

class Tracker
{
public:
	// The USB suspend interrupt in nState. bStill: DSTS.SuspSts now (FALSE: the bus resumed before this ran). TRUE: go
	// suspended; nSkip1, nSkip2 are the states with nothing to suspend (powered, already suspended).
	bool Suspend (int nState, int nSkip1, int nSkip2, bool bStill)
	{
		if (!bStill || nState == nSkip1 || nState == nSkip2) return false;
		m_nSaved = nState;
		return true;
	}

	// The bus woke: the state to go back to (once), kNone with nothing to go back to
	int Wake (void)
	{
		int n = m_nSaved;
		m_nSaved = kNone;
		return n;
	}

	void Reset (void)		{ m_nSaved = kNone; }	// a bus reset: the host starts over
	bool Pending (void) const	{ return m_nSaved != kNone; }

private:
	int m_nSaved = kNone;
};

}

#endif

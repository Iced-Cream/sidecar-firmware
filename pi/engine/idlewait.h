// pi/engine/idlewait.h - the waits that sleep instead of spinning (portable, header only: tests/test_idlewait.cpp;
// docs/pi-engine.md "Idle sleep"). The instruction itself (wfe) is the caller's: here is only what must hold around it.
#ifndef _idlewait_h
#define _idlewait_h

namespace idlewait {

// Core 0, the end of its loop: nUs of the clock since t0 (the clock wraps, the difference does not care), the other
// tasks run by yield (), and the time they leave in sleep (). What CScheduler::MsSleep does, minus its spin: with nothing
// ready it loops on the clock, and CNetTask, always ready, makes that a spin through the network's Process. yield ()
// comes first and after every sleep, so no task waits longer than one sleep more than it did, and a task that ran past the
// deadline costs no sleep. A sleep (nap below) ends at an interrupt at once and at the event stream within 152 us: the loop
// is late by that much at most, and no wake-up can be lost, because none is expected.
template <class TNow, class TYield, class TSleep>
inline unsigned spend (unsigned t0, unsigned nUs, TNow now, TYield yield, TSleep sleep)
{
	unsigned nSleeps = 0;
	for (;;)
	{
		yield ();
		if (now () - t0 >= nUs) return nSleeps;
		sleep ();
		nSleeps++;
	}
}

// Core 0's one sleep: drop the event left over, then wait for a new one. Every CSpinLock::Acquire ends in a store-exclusive
// and the network's round takes one (CTransportLayer::Process, always): what that leaves in the event register made a plain
// wfe return at once, 1.2 million times a second. set_event () is sevl and wfe_ () one wfe: the first wfe clears the
// register whatever it held, the second sleeps. Only here: core 0 waits for no event, so nothing is lost. The cores that
// check a word before their wfe must keep a plain wfe: for them the pending event is the wake-up.
template <class TSetEvent, class TWfe>
inline void nap (TSetEvent set_event, TWfe wfe_)
{
	set_event ();
	wfe_ ();
	wfe_ ();
}

// Core 3, a block rendering: until done (), false when expired () comes first. expired () is asked after every wake, so
// sleep () must end on its own (the event stream). bSleep false: the spin it replaces. The caller's done () reads what the
// other core publishes before its sev: check, then sleep, with nothing between (the model in the test).
template <class TDone, class TExpired, class TSleep>
inline bool wait (TDone done, TExpired expired, TSleep sleep, bool bSleep)
{
	while (!done ())
	{
		if (expired ()) return false;
		if (bSleep) sleep ();
	}
	return true;
}

}	// namespace idlewait

#endif

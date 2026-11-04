// pi/engine/cores.h - the engine's cores: cores 1 and 2 idle (in wfe); core 3 runs the control loop - the Rytm's MIDI
// bytes into the host link's router (bridge.h), the USB-A MIDI devices, the console's commands, the generic tick and
// the card's idle saves. Core 0 is the kernel's: USB, the OUT fill, the network, the console, the stats, the card queue.
#ifndef _cores_h
#define _cores_h
#include <circle/memory.h>
#include <circle/multicore.h>
#include <circle/types.h>

class CEngineCores : public CMultiCoreSupport
{
public:
	struct TStats
	{
		u64 nTicks;			// generic-timer ticks since the last TakeStats
		u64 nBusy[4];			// ticks working, per core
		u64 nSleep[4];			// ticks asleep in a counted wfe, per core
		unsigned nWakes[4];		// how many of those wfe returned
		unsigned nMaxPassUs, nMidiBytes, nSysEx, nNotes;
	};

	CEngineCores (CMemorySystem *pMemorySystem) : CMultiCoreSupport (pMemorySystem) {}
	static void Configure (unsigned nFifoTarget);	// the links' FIFO target (fifo=), kept for /status and the TCP link
	void Run (unsigned nCore) override;
	static void TakeStats (TStats *pStats);		// core 0
	static unsigned FifoTarget (void);
	// The control loop's longest gap between two pass starts since the last Output::Reset (a new stream), with where that
	// pass's time went: the sections midi keys cmds tick render idle (us each). at_frames: frames pulled since the Reset
	// when the gap ended; gaps_over_2ms: how many gaps were that long. step_us, step_sum, here_us: of its longest section,
	// tick or idle (none: 0): its slowest named step, all its named steps, the control core's own time on the card
	// (cardq::here_take). Racy reads from core 0 (a diagnostic).
	struct TStall { unsigned gap_us, at_frames, gaps_over_2ms, sec_us[6], step_us, step_sum, here_us; };
	static TStall Stall (void);
	static const char *StallStep (void);		// that slowest step's name, "" none. Kept here, /log's 64 KB are gone in 30 s
	// The last kGaps gaps over 2 ms since the Reset, oldest first: {gap us, frames pulled at its end, us of that pass spent
	// in its wfe (gap minus it: the pass itself), cntkctl_el1 then (the event stream's bits), DAIF (interrupt masks),
	// the pass's longest section (0 midi, 1 keys, 2 cmds, 3 tick, 4 render, 5 idle) and its us, then as TStall its slowest
	// step's name ("" none) and us, the named steps' us, the card's us on the control core}.
	static const unsigned kGaps = 8;
	struct TGap { unsigned gap_us, at_frames, wfe_us, kctl, daif, sec, sec_us; const char *step; unsigned step_us, step_sum, here_us; };
	static unsigned Gaps (TGap *p);			// returns how many (0..kGaps)
	// The SysEx messages from the Rytm that took core 3 over 1 ms, oldest first: {command (byte 4), the next byte (a
	// voice, a track..), length, us}.
	static const unsigned kSlowSx = 8;
	struct TSlowSx { unsigned cmd, arg, len, us; };
	static unsigned SlowSysex (TSlowSx *p);		// returns how many (0..kSlowSx)
	static boolean WaitPolled (void);		// :ctlwait=poll - the control loop spins instead of wfe (an A/B for the gaps)
	// Idle sleep (docs/pi-engine.md "Idle sleep"). idlewfe=0 on the cmdline, :idlewfe=0 on the console: core 3 spins while
	// the render cores work and core 0's loop waits in MsSleep, as before; the render cores' and the control loop's own wfe
	// were always there and stay.
	static void SetIdleWfe (boolean bOn);
	static boolean IdleWfe (void);
	static void EventStream (void);			// the calling core: a wfe sleeps 152 us at most
	static void Sleep (void);			// core 0: one counted sleep (idlewait::nap)
	// The transport's default FIFO target (kernel.cpp UseLink: usb FifoUsb, tcp FifoTcp), unless a fifo= (the
	// cmdline's, or the console's at any time) set it: that one stays until the next fifo=.
	static void LinkFifoTarget (unsigned nFrames);
	static void FixFifoTarget (void);		// the cmdline gave fifo=

private:
	void Worker (unsigned nCore);
	void Control (void);
	void Command (const char *pText);
};

#endif

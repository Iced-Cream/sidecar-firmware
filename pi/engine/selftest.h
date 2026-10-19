// pi/engine/selftest.h - the Rytm-less self-test (cmdline selftest=1, or the console's :selftest=on|off): a transport
// whose clock is the generic timer instead of the Rytm's IN stream. On core 0 it pulls 7-frame blocks from the
// output stage at 48 kHz (6857 blocks/s, as the Overbridge OUT stream would), feeds OUT MAIN L to the console's 'w'
// capture, and plays synthetic MIDI bytes on channel 1 (note 48, velocity 100, a note-off and a note-on every
// 0.5 s) through the real path: g_MidiIn -> the control core's MIDI parser -> the note adapter -> the engine core
// (voice 0's machine as :notemach=0:NAME sets it). It sends nothing to USB; the engine's bytes for the host are
// dropped and counted, as the USB transport does. It refuses to start, and switches off, while the Rytm is
// connected (its stream or its USB MIDI, kernel.cpp): two consumers of the output stage, or two producers of
// g_MidiIn, would break the single-consumer/single-producer queues.
#ifndef _selftest_h
#define _selftest_h
#include <circle/types.h>

#include "transport.h"

class CSelfTest : public CTransport
{
public:
	static const unsigned Rate = 48000, FramesPerBlock = 7, Note = 48, Velocity = 100;

	void Start (void);				// core 0, only while the Rytm does not stream
	void Halt (void)				{ m_bOn = FALSE; }
	void SetQuiet (boolean bQuiet)			{ m_bQuiet = bQuiet; }	// no notes (and no trig log lines) during a dump

	const char *Name (void) const override		{ return "selftest"; }
	boolean Running (void) const override		{ return m_bOn; }
	void Poll (void) override;
	void Stop (void) override			{ m_bOn = FALSE; }	// nothing went to USB: nothing to stop
	void LogStats (CLogger *pLogger) override;
	unsigned MIDIOutDropped (void) const override	{ return m_nDropped; }

private:
	void SendNote (void);

	boolean m_bOn = FALSE, m_bQuiet = FALSE;
	u64 m_nFreq = 1, m_nT0 = 0, m_nSinceT0 = 0;	// the pacer: blocks pulled since m_nT0 (generic timer ticks)
	u64 m_nNoteAt = 0;
	unsigned m_nBlocks = 0, m_nNotes = 0, m_nSlips = 0, m_nDropped = 0;
	u64 m_nLastLog = 0;
};

#endif

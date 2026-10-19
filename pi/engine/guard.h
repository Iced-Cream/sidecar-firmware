// pi/engine/guard.h - the voice guard (spec "Errors and safety"): each voice's render is timed; three blocks in a
// row over the budget mute it, as does a fault while it renders (the exception handler jumps back into its render
// core's loop). A muted voice is silent, shown as !<MACHINE> in the stats, until :unmute. Anything fatal (an
// exception elsewhere, a render core that does not finish a block) asks core 0 to stop the stream and reboot.
// The bookkeeping (Leave, Mute, Recover, Unmute) is inline and portable: tests/test_guard.cpp runs it on the Mac.
#ifndef _guard_h
#define _guard_h
#include <circle/exceptionhandler.h>
#include <circle/logger.h>
#include <circle/types.h>

class CGuard
{
public:
	static const unsigned Voices = 8;

	void SetBudget (unsigned nTicks)	{ m_nBudget = nTicks; }
	unsigned Budget (void) const		{ return m_nBudget; }
	void Enter (unsigned nCore, unsigned nVoice);			// a render core, before the voice renders
	void Leave (unsigned nCore, unsigned nVoice, u32 nTicks)	// after, with its time
	{
		m_bArmed[nCore] = FALSE;
		if (nTicks > m_nMaxTicks[nVoice]) m_nMaxTicks[nVoice] = nTicks;
		if (nTicks > m_nBudget)
		{
			if (++m_nOver[nVoice] >= 3) Mute (nVoice, "over budget");
		}
		else
		{
			m_nOver[nVoice] = 0;
		}
	}
	boolean Muted (unsigned nVoice) const	{ return (m_nMuted >> nVoice) & 1; }
	u32 MutedMask (void) const		{ return m_nMuted; }
	u32 TakeNews (void)			{ return __atomic_exchange_n (&m_nNews, 0, __ATOMIC_ACQ_REL); }
	const char *Why (unsigned nVoice) const	{ return m_pWhy[nVoice]; }
	u32 MaxTicks (unsigned nVoice) const	{ return m_nMaxTicks[nVoice]; }
	boolean Armed (unsigned nCore) const	{ return m_bArmed[nCore]; }
	void **JumpBuffer (unsigned nCore)	{ return m_JmpBuf[nCore]; }
	void NoteFault (unsigned nCore, u64 nPC) { m_nFaultPC[nCore] = nPC; m_nFaults[nCore]++; }
	void NoteResumed (unsigned nCore, boolean bTaskLevel) { m_bResumedOK[nCore] = bTaskLevel; }	// after Recover
	unsigned Recover (unsigned nCore)	// after the jump: the voice that faulted, muted
	{
		m_bArmed[nCore] = FALSE;
		unsigned v = m_nCurrent[nCore];
		Mute (v, "crashed");
		return v;
	}
	void Fatal (const char *pWhy)		{ if (m_pFatal == 0) m_pFatal = pWhy; }
	const char *FatalWhy (void) const	{ return m_pFatal; }
	void LogNews (CLogger *pLogger);	// core 0: each new mute once
	void Unmute (void)			// the control core, between blocks; also ends a pending :overtest
	{
		m_nOverBlocks = 0;		// a muted voice's remaining test blocks did not run (it was not rendered)
		for (unsigned v = 0; v < Voices; v++) m_nOver[v] = 0, m_nMaxTicks[v] = 0;
		__atomic_store_n (&m_nMuted, 0, __ATOMIC_RELEASE);
	}
	// tests (console): the voice's next render faults (a write to an unmapped address) / runs 300 us longer for
	// nBlocks blocks / runs 30 ms long once (a render core that misses the 20 ms block limit: fatal)
	void ArmFault (unsigned nVoice)		{ m_nFaultTest = nVoice + 1; }
	void ArmOverrun (unsigned nVoice, unsigned nBlocks) { m_nOverVoice = nVoice; m_nOverBlocks = nBlocks; }
	void ArmHang (unsigned nVoice)		{ m_nHangTest = nVoice + 1; }

private:
	void Mute (unsigned nVoice, const char *pWhy)
	{
		m_pWhy[nVoice] = pWhy;
		__atomic_or_fetch (&m_nMuted, 1u << nVoice, __ATOMIC_RELEASE);
		__atomic_or_fetch (&m_nNews, 1u << nVoice, __ATOMIC_RELEASE);
	}

	unsigned m_nBudget = 3375;		// 25 % of a 12-frame block at 54 MHz (cmdline voicebudget=)
	volatile u32 m_nMuted = 0, m_nNews = 0;
	const char *m_pWhy[Voices] = {0};
	u8 m_nOver[Voices] = {0};
	u32 m_nMaxTicks[Voices] = {0};
	volatile boolean m_bArmed[4] = {FALSE, FALSE, FALSE, FALSE};
	volatile unsigned m_nCurrent[4] = {0, 0, 0, 0};
	void *m_JmpBuf[4][5];			// __builtin_setjmp's five words, one buffer per core
	volatile u64 m_nFaultPC[4] = {0, 0, 0, 0};
	volatile unsigned m_nFaults[4] = {0, 0, 0, 0};
	volatile boolean m_bResumedOK[4] = {FALSE, FALSE, FALSE, FALSE};	// the worker back in EL1t, IRQs as before
	const char *volatile m_pFatal = 0;
	volatile unsigned m_nFaultTest = 0, m_nOverVoice = 0, m_nOverBlocks = 0, m_nHangTest = 0;
};

extern CGuard g_Guard;

class CEngineExceptionHandler : public CExceptionHandler
{
public:
	void Throw (unsigned nException, TAbortFrame *pFrame) override;
};

#endif

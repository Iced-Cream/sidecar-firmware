// pi/engine/guard.h - the engine's exception handler: an exception on any core leaves its breadcrumb (crumb.h, kept over
// the reboot) and asks core 0 to stop the stream and reboot (FatalWhy, polled by core 0's loop). A core other than 0
// then waits in wfe; core 0 itself stops the Overbridge URBs and panics, and the watchdog reboots.
#ifndef _guard_h
#define _guard_h
#include <circle/exceptionhandler.h>
#include <circle/types.h>

class CGuard
{
public:
	void Fatal (const char *pWhy)		{ if (m_pFatal == 0) m_pFatal = pWhy; }
	const char *FatalWhy (void) const	{ return m_pFatal; }

private:
	const char *volatile m_pFatal = 0;
};

extern CGuard g_Guard;

class CEngineExceptionHandler : public CExceptionHandler
{
public:
	void Throw (unsigned nException, TAbortFrame *pFrame) override;
};

#endif

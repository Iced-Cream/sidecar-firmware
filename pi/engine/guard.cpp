// pi/engine/guard.cpp - see guard.h.
#include "guard.h"

#include <circle/multicore.h>

#include "crumb.h"
#include "ob_usb.h"

CGuard g_Guard;

void CEngineExceptionHandler::Throw (unsigned nException, TAbortFrame *pFrame)
{
	unsigned nCore = CMultiCoreSupport::ThisCore ();
	// the breadcrumb (crumb.h): which core, why, where - kept over the reboot
	crumb::Fault (nCore, (u32) pFrame->esr_el1, pFrame->elr_el1, pFrame->far_el1, "an exception");
	g_Guard.Fatal ("an exception");
	if (nCore != 0)
	{
		for (;;) asm volatile ("wfe");		// core 0 stops the stream and reboots
	}
	CObUsb::EmergencyStop ();			// core 0 itself: no more URBs; the watchdog reboots after the panic
	CExceptionHandler::Throw (nException, pFrame);
}

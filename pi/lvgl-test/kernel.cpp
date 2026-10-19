// pi/lvgl-test/kernel.cpp - see kernel.h. The status shows what this kernel can know without the engine: a USB MIDI
// device (umidi1, the Rytm's class-compliant MIDI port) as "Rytm connected", core 0's busy share; the rest stays "-".
#include "kernel.h"
#include <circle/string.h>
#include <circle/startup.h>

LOGMODULE ("kernel");

static const char RebootMagic[] = "RYTM-REBOOT";

static void RebootOnMagic (void)
{
	reboot ();
}

CKernel::CKernel (void)
:	m_Options (),
	m_Serial (&m_Interrupt),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE),			// TRUE: plug-and-play (the touch panel, the Rytm)
	m_Display (m_Options.GetWidth (), m_Options.GetHeight ())	// cmdline width= height=, else the firmware's mode
{
	m_ActLED.Blink (3);
}

boolean CKernel::Initialize (void)
{
	if (!m_Serial.Initialize (921600)) return FALSE;
	if (!m_Logger.Initialize (&m_Serial)) return FALSE;
	if (!m_Interrupt.Initialize ()) return FALSE;
	if (!m_Timer.Initialize ()) return FALSE;
	if (!m_USBHCI.Initialize ()) return FALSE;
	m_Display.Initialize ();					// FALSE: no display, the kernel still runs (log)
	return TRUE;
}

void CKernel::Run (void)
{
	m_Serial.RegisterMagicReceivedHandler (RebootMagic, RebootOnMagic);
	LOGNOTE ("display test: %s %ux%u (compiled " __DATE__ " " __TIME__ ")", m_Display.IsOn () ? "display" : "NO display",
		 m_Display.GetWidth (), m_Display.GetHeight ());

	unsigned nWindow = CTimer::GetClockTicks (), nBusy = 0, nLog = 0;
	CString Note;
	sidecar_ui_status_t Status = {};
	for (unsigned c = 0; c < SIDECAR_UI_CORES; c++) Status.load[c] = -1;
	for (;;)
	{
		unsigned t0 = CTimer::GetClockTicks ();
		boolean bUpdated = m_USBHCI.UpdatePlugAndPlay ();
		m_Display.Update (bUpdated);
		unsigned t1 = CTimer::GetClockTicks ();
		if (t1 - t0 > 50) nBusy += t1 - t0;			// the passes that did work (a redraw, an enumeration)

		if (t1 - nWindow >= CLOCKHZ / 2)
		{
			Status.rytm = m_DeviceNameService.GetDevice ("umidi1", FALSE) != 0;
			Status.load[0] = (int) (100ull * nBusy / (t1 - nWindow));
			Note.Format ("display test kernel - %ux%u - input: %s", m_Display.GetWidth (), m_Display.GetHeight (),
				     m_Display.GetInput ());
			Status.note = Note;
			m_Display.SetStatus (Status);
			if (++nLog % 20 == 0)				// every 10 s
				LOGNOTE ("input %s, umidi1 %s, core 0 busy %d%%, longest update %u us", m_Display.GetInput (),
					 Status.rytm ? "yes" : "no", Status.load[0], m_Display.TakeMaxUpdateUs ());
			nWindow = t1;
			nBusy = 0;
		}
	}
}

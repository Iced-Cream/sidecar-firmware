//
// spike1: Circle on Pi 4. Logs to the serial console (UART0, GPIO14/15, 115200)
// and to the HDMI screen when one is connected:
// a banner, the boot timestamp (microseconds on Circle's clock when the kernel
// starts running, i.e. since the clock started counting) and one line per second.
//
#include "kernel.h"

static const char From[] = "spike1";

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Tee (&m_Serial),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer)
{
	m_ActLED.Blink (3);	// alive (no-op in QEMU)
}

CKernel::~CKernel (void)
{
}

boolean CKernel::Initialize (void)
{
	boolean bOK = m_Serial.Initialize (115200);

	if (bOK && m_Screen.Initialize ())
	{
		m_Tee.SetSecond (&m_Screen);	// no screen (or QEMU): serial only
	}

	if (bOK)	bOK = m_Logger.Initialize (&m_Tee);
	if (bOK)	bOK = m_Interrupt.Initialize ();
	if (bOK)	bOK = m_Timer.Initialize ();

	return bOK;
}

void CKernel::Run (void)
{
	unsigned nBootUs = m_Timer.GetClockTicks ();

	m_Logger.Write (From, LogNotice, "==== rytm-pi spike1: Circle on Raspberry Pi %u ====", (unsigned) RASPPI);
	m_Logger.Write (From, LogNotice, "Compile time: " __DATE__ " " __TIME__);
	m_Logger.Write (From, LogNotice, "boot timestamp: %u us (clock at Run() entry; HZ=%u)", nBootUs, (unsigned) HZ);

	unsigned nLastTicks = m_Timer.GetClockTicks ();
	unsigned nSecond = 0;
	while (1)
	{
		m_Timer.MsDelay (1000);

		unsigned nNow = m_Timer.GetClockTicks ();
		m_Logger.Write (From, LogNotice, "tick %u: clock %u us (+%u us), uptime %u s",
				++nSecond, nNow, nNow - nLastTicks, m_Timer.GetUptime ());
		nLastTicks = nNow;
	}
}

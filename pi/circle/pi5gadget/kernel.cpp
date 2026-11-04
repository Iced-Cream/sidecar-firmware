// pi/circle/pi5gadget/kernel.cpp - kernel.h
//
// cmdline.txt (all optional): usbinit=N the way to start with (default 4); walk=S seconds on each way while no USB reset
// has come (default 6, 0: stay); testsecs=S reboot after S seconds (default 300, 0: never).
// The serial console: r reboot, d the registers, 0..6 that way now, n the next way, s stay on this way.
#include "kernel.h"

#include <circle/bcmpropertytags.h>
#include <circle/devicetreeblob.h>
#include <circle/machineinfo.h>
#include <circle/startup.h>
#include <circle/string.h>
#include <circle/usb/usb.h>
#include <circle/util.h>

static const char From[] = "pi5gadget";

// the ways, in the order they are tried: Linux's first, the power cycle last
static const unsigned s_Walk[] = {4, 3, 0, 2, 1, 5, 6};
static const unsigned WalkSteps = sizeof s_Walk / sizeof s_Walk[0];
static const unsigned WatchdogSeconds = 15;		// a hang (a way that stops the bus) reboots the Pi

const void *CTestGadget::GetDescriptor (u16 wValue, u16 wIndex, size_t *pLength)
{
	if ((wValue >> 8) == DESCRIPTOR_STRING && ((wValue & 0xFF) == 1 || (wValue & 0xFF) == 2))
	{
		return ToStringDescriptor ("Sidecar", pLength);
	}
	return CUSBMIDIGadget::GetDescriptor (wValue, wIndex, pLength);
}

// IRQ level: the packet back, its header byte is the byte before the 3 MIDI bytes (as pi/engine/gadget.cpp)
void CTestGadget::PacketHandler (unsigned nCable, u8 *pPacket, unsigned nLength, unsigned nDevice, void *pParam)
{
	CTestGadget *pThis = (CTestGadget *) pParam;
	pThis->m_nInPackets++;
	CUSBMIDIDevice *pMIDI = pThis->GetMIDIDevice ();
	if (pMIDI == 0 || !pMIDI->SendEventPackets (pPacket - 1, CUSBMIDIDevice::EventPacketSize)) pThis->m_nEchoFail++;
}

boolean CTestGadget::Bind (void)
{
	UpdatePlugAndPlay ();
	CUSBMIDIDevice *pMIDI = GetMIDIDevice ();
	if (pMIDI != m_pBound)
	{
		m_pBound = pMIDI;
		if (pMIDI != 0)
		{
			pMIDI->RegisterPacketHandler (PacketHandler, this);
			CLogger::Get ()->Write (From, LogNotice, "CONFIGURED by the computer (%s speed): MIDI echo on",
						GetNegotiatedUSBSpeed () == HighSpeed ? "high" : "full");
		}
	}
	return m_pBound != 0;
}

CKernel::CKernel (void)
:	m_Serial (&m_Interrupt),
	m_Timer (&m_Interrupt),
	m_Logger (LogTrace, &m_Timer),			// Circle's own trace of the gadget: SETUPs, suspends, enumerations
	m_Gadget (&m_Interrupt),
	m_nInit (4),
	m_nInitSecs (0),
	m_bWalk (FALSE)
{
}

boolean CKernel::Initialize (void)
{
	boolean bOK = m_Interrupt.Initialize ();
	if (bOK) bOK = m_Serial.Initialize (921600);
	if (bOK) bOK = m_Logger.Initialize (&m_Serial);
	if (bOK) bOK = m_Timer.Initialize ();
	if (!bOK) return FALSE;

	m_Logger.Write (From, LogNotice, "==== Pi 5 USB-C bring-up kernel ==== built " __DATE__ " " __TIME__);
	m_Watchdog.Start (WatchdogSeconds);
	LogBoard ();

	m_nInit = m_Options.GetAppOptionDecimal ("usbinit", 4);
	m_bWalk = m_Options.GetAppOptionDecimal ("walk", 6) != 0;
	if (!m_Gadget.Initialize ())
	{
		m_Logger.Write (From, LogError, "the gadget did not start");
		m_bWalk = FALSE;
		return TRUE;				// Run () still logs and reboots
	}
	m_Logger.Write (From, LogNotice, "gadget up: usbinit=%u, waiting for a USB reset from the host", m_nInit);
	m_Gadget.DumpRegisters ("after the init");
	return TRUE;
}

// what the firmware told us: did the dwc2 overlay apply, the bootloader's date, the supply
void CKernel::LogBoard (void)
{
	const CDeviceTreeBlob *pDTB = CMachineInfo::Get ()->GetDTB ();
	if (pDTB == 0)
	{
		m_Logger.Write (From, LogNotice, "DT: none");
		return;
	}
	static const char *const Props[][2] =
	{
		{"/axi/usb@480000", "status"}, {"/axi/usb@480000", "dr_mode"}, {"/chosen/bootloader", "build-timestamp"},
		{"/chosen/power", "max_current"}, {"/chosen", "bootargs"},
	};
	for (unsigned i = 0; i < sizeof Props / sizeof Props[0]; i++)
	{
		const TDeviceTreeNode *pNode = pDTB->FindNode (Props[i][0]);
		const TDeviceTreeProperty *pProp = pNode ? pDTB->FindProperty (pNode, Props[i][1]) : 0;
		if (pProp == 0)
		{
			m_Logger.Write (From, LogNotice, "DT %s %s: -", Props[i][0], Props[i][1]);
		}
		else if (pDTB->GetPropertyValueLength (pProp) == 4)
		{
			m_Logger.Write (From, LogNotice, "DT %s %s: %u", Props[i][0], Props[i][1], pDTB->GetPropertyValueWord (pProp, 0));
		}
		else
		{
			m_Logger.Write (From, LogNotice, "DT %s %s: %s", Props[i][0], Props[i][1],
					(const char *) pDTB->GetPropertyValue (pProp));
		}
	}
}

// every 10 s: the state, and the firmware's throttled flags (the Pi 5 runs here on what the hub gives)
void CKernel::LogStatus (void)
{
	CBcmPropertyTags Tags;
	TPropertyTagSimple Throttled;
	Throttled.nValue = 0;
	unsigned nThrottled = Tags.GetTag (PROPTAG_GET_THROTTLED, &Throttled, sizeof Throttled, 4) ? Throttled.nValue : ~0U;
	unsigned nIRQs;
	u64 nTicks;
	m_Gadget.GetIRQStats (&nIRQs, &nTicks);
	m_Logger.Write (From, LogNotice, "t=%us usbinit=%u | USB resets %u | IRQs %u | MIDI in %u, echo fail %u | throttled 0x%X",
			m_Timer.GetUptime (), m_nInit, m_Gadget.GetUSBResetCount (), nIRQs, m_Gadget.GetInPackets (),
			m_Gadget.GetEchoFails (), nThrottled);
}

void CKernel::TryInit (unsigned nInit)
{
	if (m_Gadget.GetMIDIDevice () != 0)
	{
		m_Logger.Write (From, LogWarning, "configured: the core stays as it is");
		return;
	}
	CString Before;
	Before.Format ("after %u s of usbinit=%u", m_Timer.GetUptime () - m_nInitSecs, m_nInit);
	m_Gadget.DumpRegisters (Before);
	m_nInit = nInit;
	m_Logger.Write (From, LogNotice, "==== trying usbinit=%u ====", nInit);
	boolean bOK = m_Gadget.Reconnect (nInit);
	m_nInitSecs = m_Timer.GetUptime ();
	m_Logger.Write (From, bOK ? LogNotice : LogError, "usbinit=%u %s", nInit, bOK ? "set up" : "failed");
	m_Gadget.DumpRegisters ("just connected");
}

void CKernel::Command (char c)
{
	switch (c)
	{
	case 'r':
		m_Logger.Write (From, LogNotice, "reboot asked");
		m_Timer.MsDelay (100);
		reboot ();

	case 'd':
		m_Gadget.DumpRegisters ("asked");
		break;

	case 's':
		m_bWalk = FALSE;
		m_Logger.Write (From, LogNotice, "staying on usbinit=%u", m_nInit);
		break;

	case '0': case '1': case '2': case '3': case '4': case '5': case '6':
		m_bWalk = FALSE;
		TryInit (c - '0');
		break;

	case 'n':
		{
			m_bWalk = FALSE;
			unsigned i = 0;
			while (i < WalkSteps && s_Walk[i] != m_nInit) i++;
			TryInit (s_Walk[(i + 1) % WalkSteps]);
		}
		break;

	default:
		break;
	}
}

void CKernel::Run (void)
{
	const unsigned nTestSecs = m_Options.GetAppOptionDecimal ("testsecs", 300);
	const unsigned nDwell = m_Options.GetAppOptionDecimal ("walk", 6);
	const unsigned nStartInit = m_nInit;
	unsigned nStep = 0, nLastSec = 0, nLastResets = 0;
	boolean bConfigured = FALSE;

	m_nInitSecs = m_Timer.GetUptime ();
	LogStatus ();
	for (;;)
	{
		boolean bNow = m_Gadget.Bind ();
		if (bNow != bConfigured)
		{
			bConfigured = bNow;
			m_Logger.Write (From, LogNotice, "%s", bNow ? "RESULT: enumerated" : "not configured any more");
		}

		unsigned nResets = m_Gadget.GetUSBResetCount ();
		if (nResets != nLastResets)
		{
			if (nLastResets == 0)
			{
				m_Logger.Write (From, LogNotice, "RESULT: the host sees us: first USB reset with usbinit=%u", m_nInit);
				m_bWalk = FALSE;
			}
			nLastResets = nResets;
		}

		char c;
		if (m_Serial.Read (&c, 1) == 1) Command (c);

		unsigned nSec = m_Timer.GetUptime ();
		if (nSec != nLastSec)
		{
			nLastSec = nSec;
			m_Watchdog.Start (WatchdogSeconds);
			if (nSec % 10 == 0) LogStatus ();
		}

		if (m_bWalk && nSec - m_nInitSecs >= nDwell)
		{
			if (s_Walk[nStep] == nStartInit) nStep++;
			if (nStep < WalkSteps)
			{
				TryInit (s_Walk[nStep++]);
			}
			else
			{
				m_bWalk = FALSE;
				m_Logger.Write (From, LogNotice, "RESULT: no way got a USB reset");
				TryInit (nStartInit);
			}
		}

		if (nTestSecs != 0 && nSec >= nTestSecs)
		{
			m_Logger.Write (From, LogNotice, "%u s are up: rebooting", nTestSecs);
			LogStatus ();
			m_Timer.MsDelay (200);
			reboot ();
		}

		m_Timer.MsDelay (1);
	}
}

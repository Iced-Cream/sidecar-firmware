//
// spike2: spike1 plus Circle's USB host (xHCI, VL805) with plug-and-play. When the Rytm is
// connected (OVERBRIDGE mode) the factory hook creates CRytmObDevice, which logs the
// descriptors, selects alt 3 and runs the interrupt endpoints; this loop prints its counters.
//
#include "kernel.h"
#include <circle/startup.h>
#include <circle/string.h>
#include <circle/util.h>
#include "rytmobdevice.h"

static const char From[] = "spike2";

// The flasher (Circle's tools/flashy, --reboot:RYTM-REBOOT) sends this string; the kernel then
// reboots into the serial bootloader on the SD card, so a new kernel can be uploaded without
// touching the Pi.
static const char RebootMagic[] = "RYTM-REBOOT";

static void RebootOnMagic (void)
{
	reboot ();
}

// Audio capture: 'w' on the serial port records the next 10 s of IN MAIN L/R (see
// CRytmObDevice::CaptureStart) and dumps it as
//   WAV-BEGIN <bytes> <rate> <channels> <gaps>
//   <base64 of the int32 LE interleaved PCM, 76 characters a line>
//   WAV-END <crc32 of the PCM bytes, 8 hex digits>
// (pi/circle/capture.py reads it). Log lines may come between whole base64 lines.
static const char CaptureKey = 'w';
static const unsigned CaptureTimeoutSecs = CRytmObDevice::CaptureSecs + 5;
static const unsigned B64LineBytes = 57;		// 76 characters

static u32 s_CrcTable[256];

static void CrcInit (void)
{
	for (u32 i = 0; i < 256; i++)
	{
		u32 c = i;
		for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		s_CrcTable[i] = c;
	}
}

// zlib's crc32 (reflected, init and final xor 0xFFFFFFFF): pass the running value, start with 0
static u32 Crc32 (u32 nCrc, const u8 *p, unsigned n)
{
	nCrc = ~nCrc;
	while (n--) nCrc = s_CrcTable[(nCrc ^ *p++) & 0xFF] ^ (nCrc >> 8);
	return ~nCrc;
}

static unsigned Base64 (char *pOut, const u8 *p, unsigned n)
{
	static const char Tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char *q = pOut;
	for (; n >= 3; n -= 3, p += 3)
	{
		*q++ = Tab[p[0] >> 2];
		*q++ = Tab[(p[0] & 3) << 4 | p[1] >> 4];
		*q++ = Tab[(p[1] & 15) << 2 | p[2] >> 6];
		*q++ = Tab[p[2] & 63];
	}
	if (n > 0)
	{
		*q++ = Tab[p[0] >> 2];
		if (n == 1)
		{
			*q++ = Tab[(p[0] & 3) << 4];
			*q++ = '=';
		}
		else
		{
			*q++ = Tab[(p[0] & 3) << 4 | p[1] >> 4];
			*q++ = Tab[(p[1] & 15) << 2];
		}
		*q++ = '=';
	}
	return q - pOut;
}

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Serial (&m_Interrupt),		// interrupt driven: receives the reboot magic
	m_Tee (&m_Serial),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE),	// TRUE: plug-and-play
	m_bDumping (FALSE), m_pDump (0), m_nDumpLen (0), m_nDumpPos (0), m_nCrc (0), m_bDumpEnd (FALSE),
	m_nLineLen (0), m_nLineOff (0), m_nCaptureStart (0)
{
	m_ActLED.Blink (3);
}

CKernel::~CKernel (void)
{
}

boolean CKernel::Initialize (void)
{
	boolean bOK = m_Interrupt.Initialize ();	// first: the serial device uses its interrupt

	if (bOK)	bOK = m_Serial.Initialize (921600);

	if (bOK && m_Screen.Initialize ())
	{
		m_Tee.SetSecond (&m_Screen);	// no screen: serial only
	}

	if (bOK)	bOK = m_Logger.Initialize (&m_Tee);
	if (bOK)	bOK = m_Timer.Initialize ();
	if (bOK)	bOK = m_USBHCI.Initialize ();

	return bOK;
}

// Non-blocking: 'w' arms the capture. The reboot magic is matched in the UART interrupt,
// before the bytes reach this buffer, so reading here does not disturb it.
void CKernel::PollSerial (void)
{
	char Buf[16];
	int n = m_Serial.Read (Buf, sizeof Buf);
	for (int i = 0; i < n; i++)
	{
		if (Buf[i] != CaptureKey) continue;
		if (m_bDumping || CRytmObDevice::CaptureActive ())
		{
			m_Logger.Write (From, LogWarning, "capture: busy, 'w' ignored");
		}
		else if (!CRytmObDevice::CaptureStart ())
		{
			m_Logger.Write (From, LogError, "capture: no Overbridge stream (or no buffer)");
			m_Logger.Write (From, LogError, "WAV-ERROR no-stream");
		}
		else
		{
			m_nCaptureStart = m_Timer.GetTicks ();
			m_Logger.Write (From, LogNotice, "capture: started, %u s of MAIN L/R at %u Hz",
					CRytmObDevice::CaptureSecs, CRytmObDevice::CaptureRate);
		}
	}
}

void CKernel::StartDump (void)
{
	m_pDump = CRytmObDevice::CaptureData ();
	m_nDumpLen = CRytmObDevice::CaptureBytes;
	m_nDumpPos = 0;
	m_nCrc = 0;
	m_bDumpEnd = FALSE;
	m_bDumping = TRUE;

	CString Line;
	Line.Format ("WAV-BEGIN %u %u %u %u\n", m_nDumpLen, CRytmObDevice::CaptureRate,
		     CRytmObDevice::CaptureChannels, CRytmObDevice::CaptureGaps ());
	m_nLineLen = Line.GetLength ();
	memcpy (m_Line, (const char *) Line, m_nLineLen);
	m_nLineOff = 0;
}

// Writes whole lines into the serial TX buffer until it is full (or the dump ends). A line
// is always finished before the Run loop logs anything, so log lines fall between lines.
boolean CKernel::DumpStep (void)
{
	while (1)
	{
		while (m_nLineOff < m_nLineLen)
		{
			int n = m_Serial.Write (m_Line + m_nLineOff, m_nLineLen - m_nLineOff);
			if (n < 0) n = 0;
			m_nLineOff += n;
			if (m_nLineOff < m_nLineLen)		// TX buffer full
			{
				if (m_nLineOff == 0) return FALSE;	// between lines: back to the loop
				m_Timer.MsDelay (1);		// mid-line: finish it first (< 1 ms of TX)
			}
		}

		if (m_nDumpPos < m_nDumpLen)
		{
			unsigned n = m_nDumpLen - m_nDumpPos;
			if (n > B64LineBytes) n = B64LineBytes;
			m_nCrc = Crc32 (m_nCrc, m_pDump + m_nDumpPos, n);
			m_nLineLen = Base64 (m_Line, m_pDump + m_nDumpPos, n);
			m_Line[m_nLineLen++] = '\n';
			m_nLineOff = 0;
			m_nDumpPos += n;
		}
		else if (!m_bDumpEnd)
		{
			CString Line;
			Line.Format ("WAV-END %08x\n", m_nCrc);
			m_nLineLen = Line.GetLength ();
			memcpy (m_Line, (const char *) Line, m_nLineLen);
			m_nLineOff = 0;
			m_bDumpEnd = TRUE;
		}
		else
		{
			m_bDumping = FALSE;
			return TRUE;
		}
	}
}

void CKernel::Run (void)
{
	m_Serial.RegisterMagicReceivedHandler (RebootMagic, RebootOnMagic);
	CrcInit ();
	if (!CRytmObDevice::CaptureAlloc ())
	{
		m_Logger.Write (From, LogError, "capture: cannot allocate %u B", CRytmObDevice::CaptureBytes);
	}

	m_Logger.Write (From, LogNotice, "==== rytm-pi spike2: Overbridge USB stub (waiting for 1935:0b48) ====");
	m_Logger.Write (From, LogNotice, "boot timestamp: %u us", m_Timer.GetClockTicks ());

	unsigned nLast = 0;
	unsigned nStart = m_Timer.GetTicks ();
	boolean bScreenLog = TRUE;
	while (1)
	{
		m_USBHCI.UpdatePlugAndPlay ();		// enumerates the Rytm when it appears

		unsigned nNow = m_Timer.GetTicks ();
		// Once the HDMI console is full, every line scrolls the whole frame buffer with the
		// screen's spin lock held (IRQs off), about 20-25 ms: that starved the xHCI completions
		// (seen on a Pi 4, 2026-10-14: 0 gaps for 6 s, then a gap per scroll). After the boot
		// log the screen is dropped and only the serial port logs.
		if (bScreenLog && nNow - nStart >= 5 * HZ)
		{
			m_Logger.Write (From, LogNotice, "screen log off (scrolling stalls USB); serial only from here");
			m_Tee.SetSecond (0);
			bScreenLog = FALSE;
		}
		PollSerial ();

		if (CRytmObDevice::CaptureActive ()
		    && nNow - m_nCaptureStart >= CaptureTimeoutSecs * HZ)
		{
			CRytmObDevice::CaptureStop ();
			m_Logger.Write (From, LogError, "capture: timed out at %u of %u frames (stream stopped?)",
					CRytmObDevice::CaptureFramesDone (), CRytmObDevice::CaptureFrames);
			m_Logger.Write (From, LogError, "WAV-ERROR timeout");
		}
		if (CRytmObDevice::CaptureFull () && !m_bDumping)
		{
			CRytmObDevice::CaptureClearFull ();	// the data stays until the next 'w'
			m_Logger.Write (From, LogNotice, "capture: done in %u ms, %u gaps (%u blocks lost); dumping",
					(nNow - m_nCaptureStart) * 1000 / HZ, CRytmObDevice::CaptureGaps (),
					CRytmObDevice::CaptureLost ());
			StartDump ();
		}
		if (m_bDumping && DumpStep ())
		{
			m_Logger.Write (From, LogNotice, "capture: dump sent");
		}

		if (nNow - nLast >= HZ)
		{
			nLast = nNow;
			if (m_bDumping)
			{
				// let the TX buffer (2 KB, 22 ms at 921600) drain so the line is not cut
				m_Timer.MsDelay (25);
			}
			CRytmObDevice::LogAllStats (&m_Logger);
		}
		m_Timer.MsDelay (m_bDumping ? 1 : 10);
	}
}

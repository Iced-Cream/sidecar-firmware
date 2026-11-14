// pi/engine/teensy_flash.cpp - see teensy_flash.h, teensy_hex.h and docs/teensy-flash.md
#include "teensy_flash.h"
#include "teensy_hex.h"
#include "lib/sha256.h"
#include <circle/usb/usbfunction.h>
#include <circle/usb/usbhostcontroller.h>
#include <circle/usb/usbdevice.h>
#include <circle/usb/usbrequest.h>
#include <circle/usb/usb.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>
#include <circle/synchronize.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <stdio.h>
#include <stdarg.h>
#include "sidecar_gadget.h"	// CIRCLE_GADGET_MIRROR (circle-gadget-mirror.patch), as teensy_target.cpp
#ifdef CIRCLE_GADGET_MIRROR
#include "teensy_target.h"
#endif

static const char From[] = "teensyflash";
using namespace teensyhex;

// ---- the bootloader's driver: 16C0:0478, claimed by the patched factory (circle-usbdevicefactory4.patch) ---------------

class CTeensyBoot : public CUSBFunction
{
public:
	CTeensyBoot (CUSBFunction *pParent) : CUSBFunction (pParent), m_nResult (0) {}
	~CTeensyBoot (void)
	{
		if (s_pThis == this) { s_pThis = 0; CLogger::Get ()->Write (From, LogNotice, "the Teensy left its bootloader"); }
	}
	boolean Configure (void) override
	{
		if (s_pThis != 0) return FALSE;
		s_pThis = this;
		CLogger::Get ()->Write (From, LogNotice, "a Teensy in its bootloader (16C0:0478, bcdDevice %04x)",
					(unsigned) GetDevice ()->GetDeviceDescriptor ()->bcdDevice);
		return TRUE;
	}
	static CTeensyBoot *Get (void)	{ return s_pThis; }

	// one report (kReport bytes, in a DMA buffer that stays put until the completion) as SET_REPORT; FALSE: not sent
	boolean Submit (u8 *pBuf)
	{
		TSetupData *pS = new TSetupData;
		pS->bmRequestType = kReqType, pS->bRequest = kReq, pS->wValue = kValue, pS->wIndex = kIndex;
		pS->wLength = (u16) kReport;
		CleanAndInvalidateDataCacheRange ((uintptr) pBuf, kReport);
		CUSBRequest *pURB = new CUSBRequest (GetEndpoint0 (), pBuf, kReport, pS);
		pURB->SetCompletionRoutine (Stub, pS, this);
		m_nResult = 0;
		if (GetHost ()->SubmitAsyncRequest (pURB)) return TRUE;
		delete pURB;
		delete pS;
		return FALSE;
	}
	int Result (void) const	{ return m_nResult; }	// the last Submit: 0 pending, 1 done, -1 refused

private:
	static void Stub (CUSBRequest *pURB, void *pParam, void *pContext)	// xHCI IRQ
	{
		CTeensyBoot *p = (CTeensyBoot *) pContext;
		if (p == s_pThis) p->m_nResult = pURB->GetStatus () ? 1 : -1;
		delete pURB;
		delete (TSetupData *) pParam;
	}
	volatile int m_nResult;
	static CTeensyBoot *volatile s_pThis;
};

CTeensyBoot *volatile CTeensyBoot::s_pThis = 0;

CUSBFunction *SidecarTeensyBootCreate (CUSBFunction *pParent)
{
	if (CTeensyBoot::Get () != 0) return 0;
	return new CTeensyBoot (pParent);
}

// ---- the write ------------------------------------------------------------------------------------------------------

namespace {

enum TState { Idle, Uploading, Ready, Entering, Writing, Booting, Checking, Done, Failed };
const char *const StateName[] = {"idle", "uploading", "ready", "entering", "writing", "booting", "checking", "done", "failed"};

const unsigned kEnterSecs = 15;		// the relay -> its bootloader (it starts the request once the link is started)
const unsigned kBackSecs = 20;		// the boot report -> the relay back on the link
const unsigned kLogSecs = 20;		// the relay back -> its log read (after its relaydelay= and Start)
const unsigned kRetryMs = 10;		// a refused block again after
const unsigned kDmaBufs = 4;		// a block whose URB never came back keeps its buffer; the next block takes another

u8 *s_pImage;				// the program (kCodeSize), from the first upload on
Parser s_Parser;
volatile TState s_State = Idle;
uint64_t s_nTotal, s_nNext;
uint32_t s_nCrc;
core::lib::Sha256 s_Sha;
char s_ShaHex[65];
uint32_t s_nBlocks, s_nSent, s_nBlock, s_nRetries;
char s_Expect[24], s_Build[24], s_Error[160];
u64 s_nDeadline, s_nBlockDeadline, s_nRetryAt, s_nStartedAt, s_nDoneAt;
unsigned s_nLogReads0;
u8 *s_pDma[kDmaBufs];
unsigned s_nDma;
boolean s_bInFlight;
CTask *s_pTask;

u64 NowMs (void) { return CTimer::GetClockTicks64 () / (CLOCKHZ / 1000); }

void Fail (const char *pFormat, const char *pArg = "")
{
	snprintf (s_Error, sizeof s_Error, pFormat, pArg);
	s_State = Failed;
	s_nDoneAt = NowMs ();
	CLogger::Get ()->Write (From, LogError, "teensyflash failed: %s", s_Error);
}

boolean RelayUp (void)
{
#ifdef CIRCLE_GADGET_MIRROR
	return CTeensyTarget::Get () != 0;
#else
	return FALSE;
#endif
}

u8 *Dma (void)
{
	if (s_pDma[s_nDma] == 0)
	{
		u8 *p = new u8[kReport + 64];
		s_pDma[s_nDma] = (u8 *) (((uintptr) p + 63) & ~(uintptr) 63);
	}
	return s_pDma[s_nDma];
}

boolean SendBlock (void)
{
	CTeensyBoot *pBoot = CTeensyBoot::Get ();
	if (pBoot == 0) return FALSE;
	u8 *p = Dma ();
	block_report (s_pImage, s_nBlock, p);
	if (!pBoot->Submit (p)) return FALSE;
	s_bInFlight = TRUE;
	return TRUE;
}

void Step (void)
{
	u64 now = NowMs ();
	switch (s_State)
	{
	case Entering:
		if (CTeensyBoot::Get () != 0)
		{
			s_State = Writing;
			s_nBlock = s_Parser.next (0);
			s_nSent = 0;
			s_nBlockDeadline = now + retry_ms (0);
			s_nRetryAt = now;
			s_bInFlight = FALSE;
			CLogger::Get ()->Write (From, LogNotice, "writing %u blocks", s_nBlocks);
		}
		else if (now >= s_nDeadline) Fail ("the Teensy did not come up in its bootloader within %s s (power, cable, or relaydelay=?)", "15");
		break;

	case Writing:
		if (CTeensyBoot::Get () == 0) { Fail ("the Teensy left its bootloader during the write%s", ""); break; }
		if (s_bInFlight)
		{
			CTeensyBoot *pBoot = CTeensyBoot::Get ();
			int r = pBoot->Result ();
			if (r == 0)
			{
				if (now >= s_nBlockDeadline)	// the URB stays with the controller: its buffer is not reused
				{
					s_nDma = (s_nDma + 1) % kDmaBufs;
					char b[16];
					snprintf (b, sizeof b, "%u", s_nBlock);
					Fail ("block %s was not answered in time (press the program button, then flash again)", b);
				}
				break;
			}
			s_bInFlight = FALSE;
			if (r < 0)		// refused (busy erasing): again shortly
			{
				s_nRetries++;
				s_nRetryAt = now + kRetryMs;
				if (now >= s_nBlockDeadline)
				{
					char b[16];
					snprintf (b, sizeof b, "%u", s_nBlock);
					Fail ("block %s was refused until its time ran out", b);
				}
				break;
			}
			s_nSent++;
			s_nBlock = s_Parser.next (s_nBlock + 1);
			s_nBlockDeadline = now + retry_ms (s_nSent);
			s_nRetryAt = now;
			if (s_nBlock >= kBlocks)		// all written: start the program
			{
				u8 *p = Dma ();
				boot_report (p);
				CTeensyBoot::Get ()->Submit (p);	// its answer may never come: the Teensy resets
				s_nDma = (s_nDma + 1) % kDmaBufs;	// (that buffer may stay with the controller)
				s_State = Booting;
				s_nDeadline = now + kBackSecs * 1000;
				CLogger::Get ()->Write (From, LogNotice, "%u blocks written (%u retries): the program starts", s_nSent, s_nRetries);
				break;
			}
		}
		if (now >= s_nRetryAt && !SendBlock ())
		{
			if (now >= s_nBlockDeadline) Fail ("the bootloader's request could not be sent%s", "");
			else s_nRetryAt = now + kRetryMs;
		}
		break;

	case Booting:
		if (CTeensyBoot::Get () == 0 && RelayUp ())
		{
#ifdef CIRCLE_GADGET_MIRROR
			s_nLogReads0 = CTeensyTarget::LogReads ();
			CTeensyTarget::RequestLog ();
#endif
			s_State = Checking;
			s_nDeadline = now + kLogSecs * 1000;
		}
		else if (now >= s_nDeadline)
			Fail (CTeensyBoot::Get () != 0 ? "the Teensy stayed in its bootloader after the write%s"
						       : "the relay did not come back on the link (1209:0002)%s", "");
		break;

	case Checking:
#ifdef CIRCLE_GADGET_MIRROR
		if (!RelayUp ()) { Fail ("the relay went away again before its log was read%s", ""); break; }
		if (CTeensyTarget::LogReads () != s_nLogReads0)
		{
			static char Log[2048];
			unsigned n = CTeensyTarget::Log (Log, sizeof Log);
			if (!find_build (Log, n, s_Build, sizeof s_Build))
				{ Fail ("the relay is back but its log has no start line (read %s)", n ? "some text" : "nothing"); break; }
			if (s_Expect[0] && strcmp (s_Build, s_Expect) != 0)
			{
				char b[64];
				snprintf (b, sizeof b, "%s, expected %s", s_Build, s_Expect);
				Fail ("the relay is back with build %s", b);
				break;
			}
			s_State = Done;
			s_nDoneAt = now;
			CLogger::Get ()->Write (From, LogNotice, "teensyflash done: the relay is back with build %s (%u blocks, %llu ms)",
						s_Build, s_nSent, (unsigned long long) (now - s_nStartedAt));
		}
		else if (now >= s_nDeadline) Fail ("the relay is back but its log was not read (relaydelay=?)%s", "");
#else
		Fail ("this engine has no relay driver (built without CIRCLE_GADGET_MIRROR)%s", "");
#endif
		break;

	default:
		break;
	}
}

class CFlashTask : public CTask
{
public:
	CFlashTask (void) { SetName ("teensyflash"); }
	void Run (void) override
	{
		for (;;)
		{
			Step ();
			CScheduler::Get ()->MsSleep (s_State == Writing ? 1 : 20);
		}
	}
};

boolean Busy (void) { return s_State == Entering || s_State == Writing || s_State == Booting || s_State == Checking; }

void Res (fsweb::Result *r, int nStatus, const char *pFormat, ...) __attribute__ ((format (printf, 3, 4)));
void Res (fsweb::Result *r, int nStatus, const char *pFormat, ...)
{
	va_list ap;
	va_start (ap, pFormat);
	r->status = nStatus;
	vsnprintf (r->msg, sizeof r->msg, pFormat, ap);
	va_end (ap);
}

}	// namespace

namespace teensy_flash {

const uint64_t kMaxHex = 24u << 20;	// the hex of a full 7936 KB program is ~22 MB

void Put (uint64_t nOff, uint64_t nTotal, uint32_t nCrc, const uint8_t *pData, unsigned nData, fsweb::Result *r)
{
	if (Busy ()) { Res (r, 409, "a write is running (GET /teensy/state)"); return; }
	if (nTotal == 0 || nTotal > kMaxHex) { Res (r, 400, "total= must be 1..%llu bytes", (unsigned long long) kMaxHex); return; }
	if (nOff == 0)
	{
		if (s_pImage == 0) s_pImage = new u8[kCodeSize];
		if (s_pImage == 0) { Res (r, 500, "no memory for the program"); return; }
		s_Parser.begin (s_pImage);
		s_nTotal = nTotal;
		s_nNext = 0;
		s_nCrc = 0;
		s_Sha.init ();
		s_ShaHex[0] = 0;
		s_Error[0] = 0;
		s_State = Uploading;
	}
	if (s_State != Uploading || nTotal != s_nTotal) { Res (r, 409, "expected off=0 (a new upload)"); return; }
	if (nOff != s_nNext) { Res (r, 409, "expected %llu", (unsigned long long) s_nNext); return; }
	if (nOff + nData > nTotal) { Res (r, 400, "the chunk runs past total="); s_State = Idle; return; }
	s_nCrc = core::lib::crc32 (s_nCrc, pData, nData);
	s_Sha.update (pData, nData);
	s_nNext += nData;
	if (!s_Parser.feed ((const char *) pData, nData))
	{
		Res (r, 400, "not a Teensy 4.1 program: %s (line %u)", s_Parser.err (), s_Parser.errline ());
		s_State = Idle;
		return;
	}
	if (s_nNext < nTotal) { Res (r, 200, "ok %llu", (unsigned long long) s_nNext); return; }
	if (s_nCrc != nCrc) { Res (r, 400, "CRC-32 %08x, the client says %08x: upload again", (unsigned) s_nCrc, (unsigned) nCrc); s_State = Idle; return; }
	if (!s_Parser.finish ())
	{
		Res (r, 400, "not a Teensy 4.1 program: %s (line %u)", s_Parser.err (), s_Parser.errline ());
		s_State = Idle;
		return;
	}
	u8 Sum[32];
	s_Sha.final (Sum);
	core::lib::hex (Sum, 32, s_ShaHex);
	s_nBlocks = s_Parser.count ();
	s_State = Ready;
	CLogger::Get ()->Write (From, LogNotice, "program uploaded: %llu bytes of hex, sha256 %s, %u blocks", (unsigned long long) nTotal, s_ShaHex, s_nBlocks);
	Res (r, 200, "done %llu bytes, %u blocks, sha256 %s", (unsigned long long) nTotal, s_nBlocks, s_ShaHex);
}

void Start (const char *pExpect, fsweb::Result *r)
{
	if (Busy ()) { Res (r, 409, "a write is running (GET /teensy/state)"); return; }
	if (s_State != Ready && s_State != Done && s_State != Failed) { Res (r, 409, "no program uploaded (POST /teensy/put)"); return; }
	if (s_ShaHex[0] == 0) { Res (r, 409, "no program uploaded (POST /teensy/put)"); return; }
	if (s_pTask == 0) s_pTask = new CFlashTask;
	strncpy (s_Expect, pExpect ? pExpect : "", sizeof s_Expect - 1);
	s_Expect[sizeof s_Expect - 1] = 0;
	s_Build[0] = 0;
	s_Error[0] = 0;
	s_nSent = s_nRetries = 0;
	s_nStartedAt = NowMs ();
	s_nDoneAt = 0;
	s_nDeadline = s_nStartedAt + kEnterSecs * 1000;
	if (CTeensyBoot::Get () != 0)
	{
		s_State = Entering;		// already in its bootloader (a recovery): Step goes straight to the write
		Res (r, 200, "started: the Teensy is in its bootloader, %u blocks, sha256 %s", s_nBlocks, s_ShaHex);
	}
	else if (RelayUp ())
	{
#ifdef CIRCLE_GADGET_MIRROR
		CTeensyTarget::RequestBootloader ();
#endif
		s_State = Entering;
		Res (r, 200, "started: the relay is told to enter its bootloader, %u blocks, sha256 %s", s_nBlocks, s_ShaHex);
	}
	else { Res (r, 409, "no Teensy on the USB-A ports (neither the relay 1209:0002 nor its bootloader 16C0:0478)"); return; }
	CLogger::Get ()->Write (From, LogNotice, "teensyflash: %s", r->msg);
}

int StateJson (char *pOut, unsigned nCap)
{
	TState st = s_State;
	u64 now = NowMs ();
	u64 ms = s_nStartedAt == 0 ? 0 : (s_nDoneAt ? s_nDoneAt : now) - s_nStartedAt;
	char Err[sizeof s_Error];
	unsigned q = 0;
	for (const char *c = s_Error; *c && q < sizeof Err - 2; c++) Err[q++] = (*c == '"' || *c == '\\') ? '\'' : *c;
	Err[q] = 0;
	int n = snprintf (pOut, nCap,
		"{\"state\":\"%s\",\"hex\":{\"bytes\":%llu,\"received\":%llu,\"sha256\":\"%s\",\"blocks\":%u},"
		"\"sent\":%u,\"retries\":%u,\"ms\":%llu,\"expect\":\"%s\",\"build\":\"%s\",\"error\":\"%s\","
		"\"bootloader\":%s,\"relay\":%s}",
		StateName[st], (unsigned long long) s_nTotal, (unsigned long long) s_nNext, s_ShaHex, s_nBlocks,
		s_nSent, s_nRetries, (unsigned long long) ms, s_Expect, s_Build, Err,
		CTeensyBoot::Get () != 0 ? "true" : "false", RelayUp () ? "true" : "false");
	return n > 0 && (unsigned) n < nCap ? n : -1;
}

}	// namespace teensy_flash

//
// ob_follow.cpp - the A4's Overbridge stream beside the Rytm's: see ob_follow.h. The sequence on the wire is CObUsb's (alt 0, then alt 3 on interfaces 1
// and 2, OUT paced by IN), which is also what the A4's firmware took from a simulated host in an emulator, with no vendor request.
//
#include "ob_follow.h"
#include <circle/usb/usbhostcontroller.h>
#include <circle/usb/usbdevice.h>
#include <circle/usb/usb.h>
#include <circle/usb/xhciendpoint.h>
#include <circle/koptions.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/new.h>

#include "ob_block.h"
#include "a4_in.h"
#include "device/a4.h"

using core::device::kA4InChannels;
using core::device::kA4OutChannels;

static const char From[] = "a4ob";

static const unsigned kInBlk = ob::block_bytes (kA4InChannels);		// 256
static const unsigned kOutBlk = ob::block_bytes (kA4OutChannels);	// 200
static const unsigned kBuf = (kInBlk > kOutBlk ? kInBlk : kOutBlk) + 64;

static u8 *s_InBuf[CObFollow::URBs], *s_OutBuf[CObFollow::URBs];	// DMA, above MEM_KERNEL_END: heap blocks, 64 B aligned, once (ob_usb.cpp)

CObFollow *CObFollow::s_pThis = 0;
volatile boolean CObFollow::s_bOn = FALSE;

CUSBFunction *CObFollow::Create (CUSBFunction *pParent)
{
	const TUSBDeviceDescriptor *pDesc = pParent->GetDevice ()->GetDeviceDescriptor ();
	if (pDesc->idVendor != core::device::kA4Vid || pDesc->idProduct != core::device::kA4Pid) return 0;
	if (CKernelOptions::Get ()->GetAppOptionDecimal ("a4", 0) == 0) return 0;
	if (s_pThis != 0)
	{
		CLogger::Get ()->Write (From, LogWarning, "an A4 already streams");
		return 0;
	}
	return new CObFollow (pParent);
}

CObFollow::CObFollow (CUSBFunction *pFunction)
:	CUSBFunction (pFunction),
	m_pInDesc (0), m_pOutDesc (0), m_pIn (0), m_pOut (0), m_pInURB {}, m_pOutURB {}, m_bInBusy {}, m_bOutBusy {},
	m_nInPending (0), m_nOutPending (0), m_bStreaming (FALSE), m_nCredit (0), m_nOutCounter (0), m_nExpectIn (0), m_bHaveIn (FALSE),
	m_nInBlocks (0), m_nInGaps (0), m_nInLost (0), m_nInBad (0), m_nInErrors (0), m_nOutErrors (0), m_nSubmitFail (0),
	m_nPrevIn (0), m_nSecs (0)
{
	s_pThis = this;
}

CObFollow::~CObFollow (void)
{
	s_pThis = 0;
	m_bStreaming = FALSE;
	// URBs still queued on the (gone) endpoint are leaked, not freed under the controller
	for (unsigned i = 0; i < URBs; i++)
	{
		if (!m_bInBusy[i]) delete m_pInURB[i];
		if (!m_bOutBusy[i]) delete m_pOutURB[i];
	}
	delete m_pOut;
	delete m_pIn;
	m_pOut = m_pIn = 0;
}

boolean CObFollow::Configure (void)
{
	CLogger::Get ()->Write (From, LogNotice, "Analog Four MKII (1935:0b47) claimed");
	const TUSBConfigurationDescriptor *pCfg = GetDevice ()->GetConfigurationDescriptor ();
	if (pCfg != 0)
	{
		const u8 *p = (const u8 *) pCfg, *pEnd = p + pCfg->wTotalLength;
		int nIface = -1, nAlt = -1;
		for (; p + 2 <= pEnd && p[0] >= 2 && p + p[0] <= pEnd; p += p[0])
		{
			if (p[1] == DESCRIPTOR_INTERFACE && p[0] >= sizeof (TUSBInterfaceDescriptor))
			{
				nIface = ((const TUSBInterfaceDescriptor *) p)->bInterfaceNumber;
				nAlt = ((const TUSBInterfaceDescriptor *) p)->bAlternateSetting;
			}
			else if (p[1] == DESCRIPTOR_ENDPOINT && p[0] >= sizeof (TUSBEndpointDescriptor) && nAlt == ob::kAlt)
			{
				const TUSBEndpointDescriptor *e = (const TUSBEndpointDescriptor *) p;
				unsigned nMps = e->wMaxPacketSize & 0x7FF;
				if ((e->bmAttributes & 3) != 3) continue;
				if (nIface == ob::kInIface && e->bEndpointAddress == ob::kInEp && nMps == kInBlk) m_pInDesc = e;
				if (nIface == ob::kOutIface && e->bEndpointAddress == ob::kOutEp && nMps == kOutBlk) m_pOutDesc = e;
			}
		}
	}
	if (m_pInDesc == 0 || m_pOutDesc == 0)
	{
		CLogger::Get ()->Write (From, LogError, "no interrupt endpoints 0x83/0x03 (%u/%u B) at alt 3", kInBlk, kOutBlk);
		return FALSE;
	}

	CUSBHostController *pHost = GetHost ();
	for (u8 nIface = ob::kInIface; nIface <= ob::kOutIface; nIface++)		// alt 0 first, as CObUsb::Configure
	{
		pHost->ControlMessage (GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE, 0, nIface, 0, 0);
	}
	CTimer::Get ()->MsDelay (100);
	for (u8 nIface = ob::kInIface; nIface <= ob::kOutIface; nIface++)
	{
		if (pHost->ControlMessage (GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE, ob::kAlt, nIface, 0, 0) < 0)
		{
			CLogger::Get ()->Write (From, LogError, "SET_INTERFACE %u alt %u failed", (unsigned) nIface, (unsigned) ob::kAlt);
			return FALSE;
		}
	}

	m_pIn = new CUSBEndpoint (GetDevice (), m_pInDesc);
	m_pOut = new CUSBEndpoint (GetDevice (), m_pOutDesc);
	if (!m_pIn->GetXHCIEndpoint ()->IsValid () || !m_pOut->GetXHCIEndpoint ()->IsValid ())	// the controller's answer on the second stream's bandwidth
	{
		CLogger::Get ()->Write (From, LogError, "the host controller refused the endpoints: no second stream");
		return FALSE;
	}

	if (s_InBuf[0] == 0)
	{
		for (unsigned i = 0; i < URBs; i++)
		{
			s_InBuf[i] = (u8 *) (((uintptr) new u8[kBuf + 64] + 63) & ~(uintptr) 63);
			s_OutBuf[i] = (u8 *) (((uintptr) new u8[kBuf + 64] + 63) & ~(uintptr) 63);
			memset (s_OutBuf[i], 0, kOutBlk);
		}
		CKernelOptions *pOpt = CKernelOptions::Get ();
		a4in::Init (pOpt->GetAppOptionDecimal ("a4ch", core::device::kA4Main), pOpt->GetAppOptionDecimal ("a4att", 6));
	}
	for (unsigned i = 0; i < URBs; i++)
	{
		m_pInURB[i] = new CUSBRequest (m_pIn, s_InBuf[i], kInBlk);
		m_pInURB[i]->SetCompletionRoutine (InStub, (void *) (uintptr) i, this);
		m_pOutURB[i] = new CUSBRequest (m_pOut, s_OutBuf[i], kOutBlk);
		m_pOutURB[i]->SetCompletionRoutine (OutStub, (void *) (uintptr) i, this);
	}

	EnterCritical (IRQ_LEVEL);
	s_bOn = TRUE;
	m_bStreaming = TRUE;
	boolean bOK = TRUE;
	for (unsigned i = 0; i < URBs && bOK; i++) bOK = SubmitIn (i);
	LeaveCritical ();
	if (!bOK)
	{
		CLogger::Get ()->Write (From, LogError, "cannot submit the first URBs");
		return FALSE;
	}
	CLogger::Get ()->Write (From, LogNotice, "alt 3 selected: its MAIN into the PRE pair, its OUT silent");
	return TRUE;
}

boolean CObFollow::SubmitIn (unsigned i)
{
	CUSBRequest *pURB = m_pInURB[i];
	pURB->SetStatus (0);
	pURB->SetResultLen (0);
	m_bInBusy[i] = TRUE;
	m_nInPending++;
	if (GetHost ()->SubmitAsyncRequest (pURB)) return TRUE;
	m_bInBusy[i] = FALSE;
	m_nInPending--;
	m_nSubmitFail++;
	return FALSE;
}

boolean CObFollow::SubmitOut (unsigned i)
{
	CUSBRequest *pURB = m_pOutURB[i];
	pURB->SetStatus (0);
	pURB->SetResultLen (0);
	m_bOutBusy[i] = TRUE;
	m_nOutPending++;
	if (GetHost ()->SubmitAsyncRequest (pURB)) return TRUE;
	m_bOutBusy[i] = FALSE;
	m_nOutPending--;
	m_nSubmitFail++;
	return FALSE;
}

void CObFollow::InStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	if (pContext != s_pThis) return;		// a late event after unplug
	((CObFollow *) pContext)->InDone ((unsigned) (uintptr) pParam, pURB);
}

void CObFollow::OutStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	if (pContext != s_pThis) return;
	((CObFollow *) pContext)->OutDone ((unsigned) (uintptr) pParam, pURB);
}

// The xHCI interrupt, core 0.
void CObFollow::InDone (unsigned i, CUSBRequest *pURB)
{
	m_bInBusy[i] = FALSE;
	m_nInPending--;
	const u8 *pBlk = s_InBuf[i];
	boolean bOK = pURB->GetStatus () != 0;
	unsigned nLen = bOK ? pURB->GetResultLength () : 0;
	InvalidateDataCacheRange ((uintptr) pBlk, kBuf);
	if (!bOK) m_nInErrors++;
	else if (nLen < kInBlk || !ob::block_from_device (pBlk)) m_nInBad++;
	else
	{
		u16 c = ob::block_counter (pBlk);
		if (m_bHaveIn && c != m_nExpectIn)		// a lost block: the loop takes it up, as it takes up the crystals
		{
			m_nInGaps++;
			m_nInLost += (u16) (c - m_nExpectIn) / ob::kFramesPerBlock;
		}
		m_bHaveIn = TRUE;
		m_nExpectIn = c + ob::kFramesPerBlock;
		m_nInBlocks++;
		a4in::In (pBlk);
		m_nCredit++;
	}
	if (m_bStreaming) SubmitIn (i);
	PumpOut ();
}

void CObFollow::OutDone (unsigned i, CUSBRequest *pURB)
{
	m_bOutBusy[i] = FALSE;
	m_nOutPending--;
	if (pURB->GetStatus () == 0) m_nOutErrors++;
	PumpOut ();
}

// One silent block for each block that came in
void CObFollow::PumpOut (void)
{
	if (m_nCredit > 2 * URBs) m_nCredit = 2 * URBs;
	while (m_bStreaming && m_nCredit != 0)
	{
		unsigned i = 0;
		while (i < URBs && m_bOutBusy[i]) i++;
		if (i == URBs) break;
		ob::write_header (s_OutBuf[i], m_nOutCounter);
		if (!SubmitOut (i)) break;
		m_nOutCounter += ob::kFramesPerBlock;
		m_nCredit--;
	}
}

void CObFollow::LogStats (CLogger *pLogger)
{
	CObFollow *p = s_pThis;
	if (p == 0 || !p->m_bStreaming) return;
	EnterCritical (IRQ_LEVEL);					// IN URBs a failed submit left idle, as CObUsb::Log
	for (unsigned i = 0; i < URBs; i++)
	{
		if (!p->m_bInBusy[i]) p->SubmitIn (i);
	}
	p->PumpOut ();
	LeaveCritical ();
	if (++p->m_nSecs < 10) return;
	unsigned nIn = p->m_nInBlocks;
	a4in::Stats s = a4in::Read ();
	pLogger->Write (From, LogNotice, "IN %u/s %u gap %u lost %u bad %u err %u | OUT err %u sf %u | fill %d follow %d ppm under %u low %u high %u full %u",
			(nIn - p->m_nPrevIn) / p->m_nSecs, nIn, (unsigned) p->m_nInGaps, (unsigned) p->m_nInLost, (unsigned) p->m_nInBad, (unsigned) p->m_nInErrors,
			(unsigned) p->m_nOutErrors, (unsigned) p->m_nSubmitFail, s.fill, s.ppm, s.under, s.low, s.high, s.full);
	p->m_nPrevIn = nIn;
	p->m_nSecs = 0;
}

void CObFollow::Stop (void)
{
	CObFollow *p = s_pThis;
	if (p == 0) return;
	p->m_bStreaming = FALSE;
	unsigned t0 = CTimer::GetClockTicks ();
	while ((p->m_nInPending || p->m_nOutPending) && CTimer::GetClockTicks () - t0 < 50000) {}
	CUSBHostController *pHost = p->GetHost ();
	for (u8 nIface = ob::kInIface; nIface <= ob::kOutIface; nIface++)
	{
		pHost->ControlMessage (p->GetEndpoint0 (), REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE, 0, nIface, 0, 0);
	}
}

void CObFollow::EmergencyStop (void)
{
	if (s_pThis != 0) s_pThis->m_bStreaming = FALSE;
}

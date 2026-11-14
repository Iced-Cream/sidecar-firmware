// pi/engine/teensy_target.cpp - see teensy_target.h, relay_proto.h and docs/teensy-mirror.md
#include "teensy_target.h"
#include "ob_usb.h"
#include "sidecar_gadget.h"
#include <circle/usb/usbhostcontroller.h>
#include <circle/usb/usbdevice.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/koptions.h>
#include <circle/util.h>

static const char From[] = "teensy";

#ifdef CIRCLE_GADGET_MIRROR

using namespace relay;

CTeensyTarget *CTeensyTarget::s_pThis = 0;

static const unsigned kDmaSize = 576;			// a link packet (512) and EP0's answers, 64-byte lines
static u8 *DmaBuf (void)
{
	u8 *p = new u8[kDmaSize + 64];
	return (u8 *) (((uintptr) p + 63) & ~(uintptr) 63);
}

static u64 NowTicks (void) { return CTimer::GetClockTicks64 (); }

// The hook the patched CUSBDeviceFactory calls for "ven1209-2" (circle-usbdevicefactory3.patch, weak reference there)
CUSBFunction *SidecarRelayCreate (CUSBFunction *pParent)
{
	if (CTeensyTarget::Get () != 0) return 0;		// one relay at a time
	return new CTeensyTarget (pParent);
}

CTeensyTarget::CTeensyTarget (CUSBFunction *pFunction)
:	CUSBFunction (pFunction),
	m_pEv (0), m_pAudioIn (0), m_pAudioOut (0), m_pCtlBuf (0), m_nVersion (0), m_pBox (0), m_nCfg (0), m_nStr (0),
	m_bHave (FALSE), m_bKeep (FALSE), m_bBoxUp (FALSE), m_bConnected (FALSE), m_bPcConfigured (FALSE), m_bUploadDue (FALSE),
	m_nBoxGoneAt (0), m_CtlState (CtlIdle), m_CtlSeq (0), m_nCtlAnswer (0), m_nTakenDue (0), m_nLastLog (0)
{
	for (unsigned c = 0; c < kChannels; c++)
	{
		m_pChanIn[c] = m_pChanOut[c] = 0;
		m_pChanOutBuf[c] = 0;
		m_bChanOutBusy[c] = FALSE;
		m_nCreditDue[c] = 0;
		m_nAtBox[c] = 0;
		for (unsigned k = 0; k < ChanInURBs; k++) { m_pChanInBuf[c][k] = 0; m_pChanInURB[c][k] = 0; }
	}
	for (unsigned k = 0; k < AudioOutBufs; k++) { m_pAudioOutBuf[k] = 0; m_bAudioOutBusy[k] = FALSE; m_pAudioOutURB[k] = 0; m_nAudioOutLen[k] = 0; }
	for (unsigned k = 0; k < EvURBs; k++) m_pEvURB[k] = 0;
	for (unsigned k = 0; k < AudioInURBs; k++) m_pAudioInURB[k] = 0;
	memset (&m_Map, 0, sizeof m_Map);
	memset (&m_Stats, 0, sizeof m_Stats);
	m_Stream.init (CKernelOptions::Get ()->GetAppOptionDecimal ("mirrormargin", 8));
	m_nHoldSecs = CKernelOptions::Get ()->GetAppOptionDecimal ("mirrorhold", 10);
	// mirrorname=off | TEXT (an underscore for a space; a space comes first): the default " (SC)", as the gadget's
	const char *pName = CKernelOptions::Get ()->GetAppOptionString ("mirrorname", 0);
	if (pName == 0) strcpy (m_Name, " (SC)");
	else if (strcmp (pName, "off") == 0) m_Name[0] = 0;
	else
	{
		unsigned n = 0;
		m_Name[n++] = ' ';
		for (; *pName && n < sizeof m_Name - 1; pName++) m_Name[n++] = *pName == '_' ? ' ' : *pName;
		m_Name[n] = 0;
	}
}

CTeensyTarget::~CTeensyTarget (void)
{
	if (s_pThis == this)
	{
		CObUsb::SetMirrorTarget (1 - CObUsb::GadgetSlot (), 0);
		s_pThis = 0;			// completions still on their way see it gone (they test s_pThis)
		CLogger::Get ()->Write (From, LogNotice, "the mirror relay went away: the box it showed is not shown");
	}
	delete m_pEv;
	delete m_pAudioIn;
	delete m_pAudioOut;
	for (unsigned c = 0; c < kChannels; c++) { delete m_pChanIn[c]; delete m_pChanOut[c]; }
	// the DMA buffers stay allocated: a URB Circle has not handed back may still point at them
}

boolean CTeensyTarget::Configure (void)
{
	const TUSBDeviceDescriptor *pDev = GetDevice ()->GetDeviceDescriptor ();
	m_nVersion = pDev->bcdDevice;
	if (GetDevice ()->GetSpeed () != USBSpeedHigh)
	{
		CLogger::Get ()->Write (From, LogError, "the mirror relay came up below high speed: not used (another cable or port?)");
		return FALSE;
	}
	if ((m_nVersion >> 8) != (kVersion >> 8))
	{
		CLogger::Get ()->Write (From, LogError, "the mirror relay speaks %u.%02u, this engine %u.%02u: not used",
					m_nVersion >> 8, m_nVersion & 0xFF, kVersion >> 8, kVersion & 0xFF);
		return FALSE;
	}
	const TUSBConfigurationDescriptor *pCfg = GetDevice ()->GetConfigurationDescriptor ();
	if (pCfg == 0) return FALSE;
	const u8 *p = (const u8 *) pCfg, *end = p + pCfg->wTotalLength;
	boolean bIface = FALSE;
	for (; p + 2 <= end && p[0] >= 2 && p + p[0] <= end; p += p[0])
	{
		if (p[1] == DESCRIPTOR_INTERFACE && p[0] >= 9)
			bIface = p[2] == 0 && p[3] == 0 && p[5] == kIfaceClass && p[6] == kIfaceSub && p[7] == kIfaceProto;
		else if (p[1] == DESCRIPTOR_ENDPOINT && p[0] >= 7 && bIface)
		{
			const TUSBEndpointDescriptor *e = (const TUSBEndpointDescriptor *) p;
			u8 a = e->bEndpointAddress;
			CUSBEndpoint *pEP = new CUSBEndpoint (GetDevice (), e);
			if (a == kEvEp) m_pEv = pEP;
			else if (a == kAudioToPiEp) m_pAudioIn = pEP;
			else if (a == kAudioFromPiEp) m_pAudioOut = pEP;
			else if ((a & 0x7F) >= kChanFirstEp && (a & 0x7F) < kChanFirstEp + kChannels)
				((a & 0x80) ? m_pChanIn : m_pChanOut)[(a & 0x7F) - kChanFirstEp] = pEP;
			else delete pEP;
		}
	}
	boolean bAll = m_pEv && m_pAudioIn && m_pAudioOut;
	for (unsigned c = 0; c < kChannels; c++) bAll = bAll && m_pChanIn[c] && m_pChanOut[c];
	if (!bAll)
	{
		CLogger::Get ()->Write (From, LogError, "the mirror relay's interface is not the link relay_proto.h describes");
		return FALSE;
	}
	if (!CUSBFunction::Configure ()) return FALSE;

	m_pCtlBuf = DmaBuf ();
	for (unsigned k = 0; k < EvURBs; k++) m_pEvBuf[k] = DmaBuf ();
	for (unsigned k = 0; k < AudioInURBs; k++) m_pAudioInBuf[k] = DmaBuf ();
	for (unsigned k = 0; k < AudioOutBufs; k++) m_pAudioOutBuf[k] = DmaBuf ();
	for (unsigned c = 0; c < kChannels; c++)
	{
		m_pChanOutBuf[c] = DmaBuf ();
		for (unsigned k = 0; k < ChanInURBs; k++) m_pChanInBuf[c][k] = DmaBuf ();
	}
	s_pThis = this;
	// the IN URBs: a fixed pool, resubmitted from their completions (no allocation while streaming, as ob_usb.cpp's)
	for (unsigned k = 0; k < EvURBs; k++)
	{
		m_pEvURB[k] = new CUSBRequest (m_pEv, m_pEvBuf[k], kEvMps);
		m_pEvURB[k]->SetCompletionRoutine (EvStub, (void *) (uintptr) k, this);
		Resubmit (m_pEvURB[k]);
	}
	for (unsigned k = 0; k < AudioInURBs; k++)
	{
		m_pAudioInURB[k] = new CUSBRequest (m_pAudioIn, m_pAudioInBuf[k], kLinkMps);
		m_pAudioInURB[k]->SetCompletionRoutine (AudioInStub, (void *) (uintptr) k, this);
		Resubmit (m_pAudioInURB[k]);
	}
	for (unsigned c = 0; c < kChannels; c++)
		for (unsigned k = 0; k < ChanInURBs; k++)
		{
			m_pChanInURB[c][k] = new CUSBRequest (m_pChanIn[c], m_pChanInBuf[c][k], kLinkMps);
			m_pChanInURB[c][k]->SetCompletionRoutine (ChanInStub, (void *) (uintptr) (c * ChanInURBs + k), this);
			Resubmit (m_pChanInURB[c][k]);
		}
	CLogger::Get ()->Write (From, LogNotice, "mirror relay %u.%02u claimed: the box %s mirrors through it", m_nVersion >> 8,
				m_nVersion & 0xFF, CObUsb::GadgetSlot () == 0 ? "in slot 1 (not the Rytm)" : "in slot 0 (the Rytm)");
	CObUsb::SetMirrorTarget (1 - CObUsb::GadgetSlot (), this);	// SetBox comes from there
	return TRUE;
}

void CTeensyTarget::Resubmit (CUSBRequest *pURB)
{
	pURB->SetStatus (0);
	pURB->SetResultLen (0);
	if (!GetHost ()->SubmitAsyncRequest (pURB)) m_Stats.nVendorFail++;	// the link is going away
}

int CTeensyTarget::Vendor (boolean bIn, u8 uchRequest, u16 wValue, u16 wIndex, const void *pData, unsigned nLength)
{
	if (nLength > kDmaSize) return -1;
	if (!bIn && nLength) memcpy (m_pCtlBuf, pData, nLength);
	int n = GetHost ()->ControlMessage (GetEndpoint0 (), (bIn ? REQUEST_IN : REQUEST_OUT) | REQUEST_VENDOR, uchRequest,
					    wValue, wIndex, m_pCtlBuf, (u16) nLength);
	if (n < 0) m_Stats.nVendorFail++;
	return n;
}

// ---- IMirrorTarget: the box's side ---------------------------------------------------------------------------------

void CTeensyTarget::SetBox (CObUsb *pBox)
{
	m_pBox = pBox;
	if (pBox == 0 && m_bBoxUp) MirrorBoxUp (FALSE);
}

boolean CTeensyTarget::Mirror (void) const
{
	// opt-in as the gadget's: the USB-C mode OVERBRIDGE (or usb=mirror on the card) shows a box maker's IDs; a relay
	// alone does not turn it on
	CSidecarGadget *g = CSidecarGadget::Get ();
	if (g != 0) return g->Mirror ();
	return strcmp (CKernelOptions::Get ()->GetAppOptionString ("usb", "sidecar"), "mirror") == 0;
}

boolean CTeensyTarget::MirrorShown (void) const { return m_bConnected && m_bPcConfigured; }

boolean CTeensyTarget::MirrorSetDescriptors (const u8 *pDev, const u8 *pCfg, unsigned nCfg)
{
	Map m;
	if (pDev[0] != 18 || pDev[1] != DESCRIPTOR_DEVICE || pDev[7] != 64 || !make_map (pCfg, nCfg, ob::kInEp, ob::kOutEp, &m))
	{
		CLogger::Get ()->Write (From, LogError, "usb=mirror: the box's descriptors are not ones the relay can carry");
		return FALSE;
	}
	m_bKeep = m_bHave && nCfg == m_nCfg && memcmp (pCfg, m_Cfg, nCfg) == 0 && memcmp (pDev, m_Dev, 18) == 0;
	if (m_bKeep) return TRUE;
	if (m_bConnected) Disconnect ();
	memcpy (m_Dev, pDev, 18);
	memcpy (m_Cfg, pCfg, nCfg);
	m_nCfg = nCfg;
	m_Map = m;
	m_nStr = 0;
	m_bHave = FALSE;
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: %04x:%04x through the relay, %u B configuration, %u endpoints, "
				"channels %u in / %u out", (unsigned) (pDev[8] | pDev[9] << 8), (unsigned) (pDev[10] | pDev[11] << 8),
				nCfg, m.l.n, m.n_out, m.n_in);
	return TRUE;
}

void CTeensyTarget::MirrorSetString (u8 uchIndex, const u8 *pDesc, unsigned nLength)
{
	if (m_bKeep || m_nStr == mirror::kMaxStrings || nLength < 2 || nLength > sizeof m_Str[0].Data) return;
	m_Str[m_nStr].uchIndex = uchIndex;
	m_Str[m_nStr].nLength = (u8) nLength;
	memcpy (m_Str[m_nStr].Data, pDesc, nLength);
	m_nStr++;
}

void CTeensyTarget::MirrorBoxUp (boolean bUp)
{
	EnterCritical ();
	m_bBoxUp = bUp && m_nCfg != 0;
	m_Stream.box_restart ();
	if (!m_bBoxUp && (m_CtlState == CtlAtBox || m_CtlState == CtlNew))	// a request at the box that went: STALL
	{
		m_nCtlAnswer = -1;
		m_CtlState = CtlAnswered;
	}
	for (unsigned c = 0; c < kChannels; c++)	// a computer packet at the box that went is never taken: credit it back
	{
		m_nCreditDue[c] += m_nAtBox[c];
		m_nAtBox[c] = 0;
	}
	LeaveCritical ();
	if (bUp && m_nCfg)
	{
		m_bHave = TRUE;
		if (!m_bConnected) m_bUploadDue = TRUE;	// MirrorPoll uploads and connects (core 0's loop, not inside Configure)
	}
	else m_nBoxGoneAt = NowTicks ();
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: box %s", m_bBoxUp ? "up" : m_bConnected ? "gone (requests are STALLed; "
				"off the computer's bus after mirrorhold)" : "gone");
}

boolean CTeensyTarget::Upload (void)
{
	if (Vendor (FALSE, kReqConnect, 0, 0, 0, 0) < 0) return FALSE;
	if (Vendor (FALSE, kReqSetDevice, 0, 0, m_Dev, 18) < 0) return FALSE;
	if (Vendor (FALSE, kReqSetConfig, 0, 0, m_Cfg, m_nCfg) < 0) return FALSE;
	for (unsigned k = 0; k < m_nStr; k++)
		if (Vendor (FALSE, kReqSetString, m_Str[k].uchIndex, 0, m_Str[k].Data, m_Str[k].nLength) < 0) return FALSE;
	m_Stream.in_ch = m_Stream.out_ch = 0;
	m_bPcConfigured = FALSE;
	if (Vendor (FALSE, kReqConnect, 1, 0, 0, 0) < 0) return FALSE;
	m_bConnected = TRUE;
	m_Stats.nConnects++;
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: the relay shows the box to its computer (%u strings)", m_nStr);
	return TRUE;
}

void CTeensyTarget::Disconnect (void)
{
	Vendor (FALSE, kReqConnect, 0, 0, 0, 0);
	m_bConnected = FALSE;
	m_bPcConfigured = FALSE;
	m_Stream.in_ch = m_Stream.out_ch = 0;
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: the relay's computer side left the bus");
}

// xHCI IRQ: the box's answer to a forwarded request
void CTeensyTarget::MirrorControlDone (unsigned nSeq, const u8 *pData, int nLength)
{
	if (m_CtlState != CtlAtBox || (u8) nSeq != m_CtlSeq) { m_Stats.nLate++; return; }
	m_nCtlAnswer = nLength;
	if (nLength > 0) memcpy (m_CtlData, pData, (unsigned) nLength <= sizeof m_CtlData ? nLength : sizeof m_CtlData);
	m_CtlState = CtlAnswered;
}

// xHCI IRQ: one IN packet of a box pipe for the computer
boolean CTeensyTarget::MirrorPipeIn (u8 uchAddr, const u8 *pData, unsigned nLength)
{
	unsigned c = ChanOfIn (uchAddr);
	if (c >= kChannels || !MirrorShown () || nLength > kLinkMps || m_bChanOutBusy[c]) { m_Stats.nPipeInDrop++; return FALSE; }
	if (nLength) memcpy (m_pChanOutBuf[c], pData, nLength);
	CleanAndInvalidateDataCacheRange ((uintptr) m_pChanOutBuf[c], kDmaSize);
	CUSBRequest *pURB = new CUSBRequest (m_pChanOut[c], m_pChanOutBuf[c], nLength);
	pURB->SetCompletionRoutine (ChanOutStub, (void *) (uintptr) c, this);
	m_bChanOutBusy[c] = TRUE;
	if (!GetHost ()->SubmitAsyncRequest (pURB)) { m_bChanOutBusy[c] = FALSE; delete pURB; m_Stats.nPipeInDrop++; return FALSE; }
	m_Stats.nPipeIn++;
	return TRUE;			// the box's next IN request waits for kEvInTaken (the computer has this one)
}

// xHCI IRQ: the box took the computer's pipe packet: the computer may send the next (one credit)
void CTeensyTarget::MirrorPipeOutDone (u8 uchAddr)
{
	unsigned c = ChanOfOut (uchAddr);
	if (c >= kChannels) return;
	if (m_nAtBox[c]) m_nAtBox[c]--;
	m_nCreditDue[c]++;
}

boolean CTeensyTarget::MirrorOutExtra (u8 *pBlock, u16 nCounter)
{
	return MirrorShown () && m_Stream.out_extra (pBlock, nCounter);
}

// xHCI IRQ: an IN block of the box (12-channel layout), cut to the computer's alternate and sent on link 0x02
void CTeensyTarget::FromBoxBlock (const u8 *pBlock)
{
	if (!MirrorShown ()) return;
	unsigned k = 0;
	while (k < AudioOutBufs && m_bAudioOutBusy[k]) k++;
	if (k == AudioOutBufs) { m_Stats.nAudioOutDrop++; return; }	// the link is behind by 16 blocks (2.3 ms)
	unsigned n = m_Stream.in_block (pBlock, m_pAudioOutBuf[k]);
	if (n == 0) return;
	CleanAndInvalidateDataCacheRange ((uintptr) m_pAudioOutBuf[k], kDmaSize);
	if (m_pAudioOutURB[k] == 0 || m_nAudioOutLen[k] != n)	// the computer's channels changed (its SET_INTERFACE): once
	{
		delete m_pAudioOutURB[k];
		m_pAudioOutURB[k] = new CUSBRequest (m_pAudioOut, m_pAudioOutBuf[k], n);
		m_pAudioOutURB[k]->SetCompletionRoutine (AudioOutStub, (void *) (uintptr) k, this);
		m_nAudioOutLen[k] = n;
	}
	m_bAudioOutBusy[k] = TRUE;
	CUSBRequest *pURB = m_pAudioOutURB[k];
	pURB->SetStatus (0);
	pURB->SetResultLen (0);
	if (!GetHost ()->SubmitAsyncRequest (pURB)) { m_bAudioOutBusy[k] = FALSE; m_Stats.nAudioOutDrop++; }
}

void CTeensyTarget::ToBox (s32 (*pFrames)[12], const s16 (*)[8], unsigned nFrames)
{
	if (MirrorShown ()) m_Stream.to_box (pFrames, nFrames);
}

// ---- the link's completions (xHCI IRQ) -----------------------------------------------------------------------------

void CTeensyTarget::EvStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CTeensyTarget *p = (CTeensyTarget *) pContext;
	unsigned k = (unsigned) (uintptr) pParam;
	if (p != s_pThis) return;			// the relay went: Circle frees its URBs with the device
	if (pURB->GetStatus () && pURB->GetResultLength () >= sizeof (Event))
	{
		InvalidateDataCacheRange ((uintptr) p->m_pEvBuf[k], kDmaSize);
		Event e;
		memcpy (&e, p->m_pEvBuf[k], sizeof e);
		p->OnEvent (e);
	}
	p->Resubmit (pURB);
}

void CTeensyTarget::OnEvent (const Event &e)
{
	m_Stats.nEvents++;
	switch (e.type)
	{
	case kEvBus:
		if (e.a == kBusReset || e.a == kBusOff)
		{
			m_bPcConfigured = FALSE;
			m_Stream.in_ch = m_Stream.out_ch = 0;
		}
		break;
	case kEvConfig:
		m_bPcConfigured = e.a != 0;
		m_Stream.in_ch = m_Stream.out_ch = 0;
		for (unsigned c = 0; c < m_Map.n_out; c++)	// each OUT pipe takes one packet at a time; MIDI a few
		{
			const mirror::Ep &ep = m_Map.l.ep[m_Map.ep_of_out[c]];
			m_nCreditDue[c] += ep.role == mirror::kPipeOut ? 1 : kMaxCredits;
		}
		break;
	case kEvAlt:
		m_Stream.set_alt (m_Map.l, m_Cfg, m_nCfg, e.a, e.b);
		break;
	case kEvSetup:
		m_Stats.nSetups++;
		m_CtlSeq = e.a;
		memcpy (m_CtlSetup, e.data, 8);
		m_CtlState = CtlNew;			// MirrorPoll forwards it (its OUT data stage read there)
		break;
	case kEvAbort:
		m_Stats.nAborts++;
		if (e.a == m_CtlSeq) m_CtlState = CtlIdle;	// a late answer from the box is for nobody (MirrorControlDone)
		break;
	case kEvInTaken:
		if (m_pBox != 0) for (unsigned i = 0; i < e.b; i++) m_pBox->PipeInNext (e.a);
		break;
	default:
		break;
	}
}

void CTeensyTarget::AudioInStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CTeensyTarget *p = (CTeensyTarget *) pContext;
	unsigned k = (unsigned) (uintptr) pParam;
	if (p != s_pThis) return;
	if (pURB->GetStatus ())
	{
		InvalidateDataCacheRange ((uintptr) p->m_pAudioInBuf[k], kDmaSize);
		CObUsb *pBox = p->m_pBox;
		if (pBox != 0 && p->MirrorShown ()) p->m_Stream.out_block (p->m_pAudioInBuf[k], pURB->GetResultLength (), pBox->NextOutCounter ());
	}
	p->Resubmit (pURB);
}

void CTeensyTarget::AudioOutStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CTeensyTarget *p = (CTeensyTarget *) pContext;
	if (p == s_pThis) p->m_bAudioOutBusy[(unsigned) (uintptr) pParam] = FALSE;	// its URB kept for the next block
	(void) pURB;
}

void CTeensyTarget::ChanInStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CTeensyTarget *p = (CTeensyTarget *) pContext;
	unsigned c = (unsigned) (uintptr) pParam / ChanInURBs, k = (unsigned) (uintptr) pParam % ChanInURBs;
	if (p != s_pThis) return;
	if (pURB->GetStatus () && p->m_Map.ep_of_out[c] >= 0)
	{
		InvalidateDataCacheRange ((uintptr) p->m_pChanInBuf[c][k], kDmaSize);
		const mirror::Ep &ep = p->m_Map.l.ep[p->m_Map.ep_of_out[c]];
		CObUsb *pBox = p->m_pBox;
		if (ep.role == mirror::kPipeOut && pBox != 0 && p->m_bBoxUp
		    && pBox->PipeOut (ep.addr, p->m_pChanInBuf[c][k], pURB->GetResultLength ()))
		{
			p->m_Stats.nPipeOut++;			// its credit comes back with MirrorPipeOutDone
			p->m_nAtBox[c]++;
		}
		else
		{
			// dropped: no box, a halted pipe, or USB MIDI (not relayed yet: docs/teensy-mirror.md "Gaps"); the
			// computer may send the next one
			if (ep.role == mirror::kMidiOut) p->m_Stats.nChanInMidi++;
			else p->m_Stats.nChanInDrop++;
			p->m_nCreditDue[c]++;
		}
	}
	p->Resubmit (pURB);
}

void CTeensyTarget::ChanOutStub (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CTeensyTarget *p = (CTeensyTarget *) pContext;
	if (p == s_pThis) p->m_bChanOutBusy[(unsigned) (uintptr) pParam] = FALSE;
	delete pURB;
}

unsigned CTeensyTarget::ChanOfOut (u8 uchAddr) const
{
	for (unsigned c = 0; c < m_Map.n_out; c++) if (m_Map.l.ep[m_Map.ep_of_out[c]].addr == uchAddr) return c;
	return kChannels;
}

unsigned CTeensyTarget::ChanOfIn (u8 uchAddr) const
{
	for (unsigned c = 0; c < m_Map.n_in; c++) if (m_Map.l.ep[m_Map.ep_of_in[c]].addr == uchAddr) return c;
	return kChannels;
}

// ---- core 0's loop ----------------------------------------------------------------------------------------------

boolean CTeensyTarget::IsNameRequest (const u8 *s, u8 uchType) const
{
	return m_Name[0] != 0 && mirror::le16 (m_Dev + 8) == mirror::kNameVendor && s[0] == uchType && s[1] == mirror::kNameRequest
		&& mirror::le16 (s + 2) == 0 && mirror::le16 (s + 4) == 0;
}

void CTeensyTarget::ControlPoll (void)
{
	if (m_CtlState == CtlNew)
	{
		u8 Setup[8];
		EnterCritical ();
		memcpy (Setup, m_CtlSetup, 8);
		u8 nSeq = m_CtlSeq;
		LeaveCritical ();
		u16 wLength = (u16) mirror::le16 (Setup + 6);
		u8 Data[512];
		boolean bOK = m_pBox != 0 && m_bBoxUp && wLength <= sizeof Data;
		if (bOK && !(Setup[0] & 0x80) && wLength)
		{
			int n = Vendor (TRUE, kReqCtlData, nSeq, 0, 0, wLength);
			bOK = n == (int) wLength;
			if (bOK) memcpy (Data, m_pCtlBuf, wLength);
			if (bOK && IsNameRequest (Setup, 0x40) && wLength == mirror::kNameLen)
				mirror::name_store (Data, mirror::kNameLen, m_Name);	// the box keeps its plain name
		}
		EnterCritical ();
		if (m_CtlState != CtlNew || m_CtlSeq != nSeq) bOK = FALSE;	// aborted meanwhile, or a newer one
		else if (bOK) m_CtlState = CtlAtBox;	// before the submit: the answer can come in the xHCI IRQ at once
		LeaveCritical ();
		if (bOK)
		{
			TSetupData S;
			memcpy (&S, Setup, sizeof S);
			if (m_pBox->Control (&S, Data, nSeq)) return;
		}
		EnterCritical ();
		if (m_CtlSeq == nSeq && (m_CtlState == CtlNew || m_CtlState == CtlAtBox)) { m_nCtlAnswer = -1; m_CtlState = CtlAnswered; }
		LeaveCritical ();
	}
	if (m_CtlState == CtlAnswered)
	{
		u8 Setup[8], Data[512];
		EnterCritical ();
		u8 nSeq = m_CtlSeq;
		int n = m_nCtlAnswer;
		memcpy (Setup, m_CtlSetup, 8);
		if (n > 0) memcpy (Data, m_CtlData, (unsigned) n);
		m_CtlState = CtlIdle;
		LeaveCritical ();
		if (n > 0 && IsNameRequest (Setup, 0xC0))
		{
			u8 Name[mirror::kNameLen];		// the computer sees the name with the suffix
			unsigned l = (unsigned) n < sizeof Name ? (unsigned) n : sizeof Name;
			memset (Name, 0, sizeof Name);
			memcpy (Name, Data, l);
			mirror::name_show (Name, l, m_Name);
			memcpy (Data, Name, l);
		}
		if (n < 0) m_Stats.nStalls++;
		m_Stats.nAnswers++;
		Vendor (FALSE, kReqCtlAnswer, nSeq, n < 0 ? kAnsStall : 0, Data, n > 0 ? (unsigned) n : 0);
	}
}

void CTeensyTarget::MirrorPoll (void)
{
	CObUsb *pBox = m_pBox;
	if (pBox != 0 && !m_bHave && Mirror ())		// the box was up before the relay or the mode: read it now
	{
		pBox->Wanted ();
		pBox->AttachLate ();
	}
	if (m_bUploadDue && m_bHave && Mirror ())
	{
		m_bUploadDue = FALSE;
		if (!Upload ()) CLogger::Get ()->Write (From, LogError, "usb=mirror: the relay refused the box's descriptors");
	}
	if (m_bConnected && !Mirror ()) Disconnect ();			// the mode turned to CLASS COMPLIANT
	if (m_bConnected && !m_bBoxUp && NowTicks () - m_nBoxGoneAt >= (u64) m_nHoldSecs * CLOCKHZ) Disconnect ();
	if (!m_bConnected) return;
	ControlPoll ();
	for (unsigned c = 0; c < m_Map.n_out; c++)
	{
		unsigned n = m_nCreditDue[c];
		if (n == 0) continue;
		if (n > kMaxCredits) n = kMaxCredits;
		if (Vendor (FALSE, kReqCredit, m_Map.l.ep[m_Map.ep_of_out[c]].addr, n, 0, 0) >= 0)
		{
			EnterCritical ();
			m_nCreditDue[c] -= n;
			LeaveCritical ();
			m_Stats.nCredits += n;
		}
	}
	if (pBox != 0) pBox->PipePoll ();				// a halted pipe: cleared and resubmitted
	u64 nNow = NowTicks ();
	if (nNow - m_nLastLog >= 10 * (u64) CLOCKHZ)
	{
		m_nLastLog = nNow;
		LogStats (CLogger::Get ());
	}
}

void CTeensyTarget::LogStats (CLogger *pLogger)
{
	CTeensyTarget *p = s_pThis;
	if (p == 0) return;
	const TStats &s = p->m_Stats;
	const mirror::StreamStats &a = p->m_Stream.st;
	pLogger->Write (From, LogNotice, "relay %s box %s pc %s | in ch %u blocks %u sent %u drop %u | out ch %u blocks %u bad %u "
			"under %u over %u extra %u lost %u late %u anchor %u | ctl %u answered %u stall %u abort %u late %u | "
			"pipe in %u drop %u out %u drop %u credits %u midi %u | ev %u vendor fail %u connects %u",
			p->m_bConnected ? "shown" : "off", p->m_bBoxUp ? "up" : "none", p->m_bPcConfigured ? "configured" : "-",
			p->m_Stream.in_ch, a.in_blocks, a.in_sent, s.nAudioOutDrop, p->m_Stream.out_ch, a.out_blocks, a.out_bad,
			a.out_under, a.out_over, a.out_extra, p->m_Stream.extra.dropped, p->m_Stream.extra.late, a.anchor,
			s.nSetups, s.nAnswers, s.nStalls, s.nAborts, s.nLate, s.nPipeIn, s.nPipeInDrop, s.nPipeOut, s.nChanInDrop,
			s.nCredits, s.nChanInMidi, s.nEvents, s.nVendorFail, s.nConnects);
}

#endif	// CIRCLE_GADGET_MIRROR

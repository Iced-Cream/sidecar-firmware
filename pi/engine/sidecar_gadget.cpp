// pi/engine/sidecar_gadget.cpp - sidecar_gadget.h
#include "sidecar_gadget.h"

#include <circle/string.h>
#include <circle/usb/usb.h>
#include <circle/util.h>
#include <circle/koptions.h>

#include "ob_block.h"
#include "ob_usb.h"

void sidecar_tone_table (int period, double amp, int32_t *out);	// tones.cpp (hosted: libm)

static const char From[] = "sidecar";

CSidecarGadget *CSidecarGadget::s_pThis = 0;

// ---- descriptors -----------------------------------------------------------------------------------------------

enum { StrManufacturer = 1, StrProduct, StrSerial, StrAudio, StrMIDI, StrOutCh = 16, StrInCh = 32 };
static const char *const OutNames[12] =
	{"MAIN L", "MAIN R", "MAIN FX L", "MAIN FX R", "BD", "SD", "RS", "BT", "LT", "MT", "CH", "CY"};
static const char *const InNames[12] =
	{"MAIN L", "MAIN R", "BD", "SD", "RS", "BT", "LT", "MT", "CH", "CY", "AUDIO IN L", "AUDIO IN R"};

static TUSBDeviceDescriptor s_Device =
{
	sizeof (TUSBDeviceDescriptor), DESCRIPTOR_DEVICE, 0x200,
	0xEF, 0x02, 0x01,				// interface association
	64, USB_GADGET_VENDOR_ID, USB_GADGET_DEVICE_ID_BASE + 3,	// 1209:0004 (P1 MIDI 0001, P2 probe 0002; 0003 a P3 build under a test name: CoreMIDI keeps a device's first name)
	0x0300, StrManufacturer, StrProduct, StrSerial, 1
};

static u8 s_Config[400];
static unsigned s_nConfig, s_nOffOut, s_nOffFb, s_nOffIn, s_nOffMIDIOut, s_nOffMIDIIn;

static void Put (const u8 *p, unsigned n)	{ memcpy (s_Config + s_nConfig, p, n); s_nConfig += n; }
#define D(...)	do { const u8 d_[] = {__VA_ARGS__}; Put (d_, sizeof d_); } while (0)
#define U16(v)	(u8) ((v) & 0xFF), (u8) (((v) >> 8) & 0xFF)
#define U32(v)	(u8) ((v) & 0xFF), (u8) (((v) >> 8) & 0xFF), (u8) (((v) >> 16) & 0xFF), (u8) (((v) >> 24) & 0xFF)

// UAC2 (Audio Device Class 2.0 4.7-4.10, the Teensy T1 layout): interfaces 0 AC, 1 AS OUT (EP 0x01, feedback 0x82),
// 2 AS IN (EP 0x83); one internal fixed 48 kHz clock 0x10; IT 0x01 (USB) -> OT 0x02, IT 0x03 -> OT 0x04 (USB); raw
// channels (bmChannelConfig 0) named by strings. USB MIDI 1.0: interfaces 3 AC, 4 MS (bulk 0x04 / 0x85), one cable.
static void BuildConfig (unsigned nIn, unsigned nOut, unsigned nMaxIn, unsigned nMaxOut)
{
	const unsigned Clock = 0x10, AcCs = 9 + 8 + 17 + 12 + 17 + 12, MsCs = 7 + 6 + 6 + 9 + 9 + 9 + 5 + 9 + 5;
	s_nConfig = 0;
	D (9, DESCRIPTOR_CONFIGURATION, 0, 0, 5, 1, 0, 0x80, 250);
	D (8, 11, 0, 3, 0x01, 0x00, 0x20, StrAudio);
	D (9, DESCRIPTOR_INTERFACE, 0, 0, 0, 0x01, 0x01, 0x20, 0);
	D (9, 0x24, 0x01, U16 (0x0200), 0x08, U16 (AcCs), 0x00);
	D (8, 0x24, 0x0A, Clock, 0x01, 0x05, 0, 0);
	D (17, 0x24, 0x02, 0x01, U16 (0x0101), 0, Clock, (u8) nOut, U32 (0), StrOutCh, U16 (0), 0);
	D (12, 0x24, 0x03, 0x02, U16 (0x0301), 0, 0x01, Clock, U16 (0), 0);
	D (17, 0x24, 0x02, 0x03, U16 (0x0201), 0, Clock, (u8) nIn, U32 (0), StrInCh, U16 (0), 0);
	D (12, 0x24, 0x03, 0x04, U16 (0x0101), 0, 0x03, Clock, U16 (0), 0);
	D (9, DESCRIPTOR_INTERFACE, 1, 0, 0, 0x01, 0x02, 0x20, 0);
	D (9, DESCRIPTOR_INTERFACE, 1, 1, 2, 0x01, 0x02, 0x20, 0);
	D (16, 0x24, 0x01, 0x01, 0x00, 0x01, U32 (1), (u8) nOut, U32 (0), StrOutCh);
	D (6, 0x24, 0x02, 0x01, 3, 24);
	s_nOffOut = s_nConfig;
	D (7, DESCRIPTOR_ENDPOINT, 0x01, 0x05, U16 (nMaxOut), 2);
	D (8, 0x25, 0x01, 0, 0, 0, 0, 0);
	s_nOffFb = s_nConfig;
	D (7, DESCRIPTOR_ENDPOINT, 0x82, 0x11, U16 (4), 4);
	D (9, DESCRIPTOR_INTERFACE, 2, 0, 0, 0x01, 0x02, 0x20, 0);
	D (9, DESCRIPTOR_INTERFACE, 2, 1, 1, 0x01, 0x02, 0x20, 0);
	D (16, 0x24, 0x01, 0x04, 0x00, 0x01, U32 (1), (u8) nIn, U32 (0), StrInCh);
	D (6, 0x24, 0x02, 0x01, 3, 24);
	s_nOffIn = s_nConfig;
	D (7, DESCRIPTOR_ENDPOINT, 0x83, 0x05, U16 (nMaxIn), 2);
	D (8, 0x25, 0x01, 0, 0, 0, 0, 0);
	// MIDI
	D (8, 11, 3, 2, 0x01, 0x03, 0x00, StrMIDI);
	D (9, DESCRIPTOR_INTERFACE, 3, 0, 0, 0x01, 0x01, 0x00, 0);
	D (9, 0x24, 0x01, U16 (0x0100), U16 (9), 1, 4);
	D (9, DESCRIPTOR_INTERFACE, 4, 0, 2, 0x01, 0x03, 0x00, 0);
	D (7, 0x24, 0x01, U16 (0x0100), U16 (MsCs));
	D (6, 0x24, 0x02, 0x01, 1, StrMIDI);		// IN jack, embedded (from the computer)
	D (6, 0x24, 0x02, 0x02, 2, 0);			// IN jack, external
	D (9, 0x24, 0x03, 0x01, 3, 1, 2, 1, StrMIDI);	// OUT jack, embedded (to the computer), from jack 2
	D (9, 0x24, 0x03, 0x02, 4, 1, 1, 1, 0);		// OUT jack, external, from jack 1
	s_nOffMIDIOut = s_nConfig;
	D (9, DESCRIPTOR_ENDPOINT, 0x04, 0x02, U16 (512), 0, 0, 0);
	D (5, 0x25, 0x01, 1, 1);
	s_nOffMIDIIn = s_nConfig;
	D (9, DESCRIPTOR_ENDPOINT, 0x85, 0x02, U16 (512), 0, 0, 0);
	D (5, 0x25, 0x01, 1, 3);
	s_Config[2] = s_nConfig & 0xFF;
	s_Config[3] = s_nConfig >> 8;
}

const void *CSidecarGadget::String (const char *p, size_t *pLength)
{
	size_t n = 2;
	for (u8 *q = m_StringBuffer + 2; *p && n < sizeof m_StringBuffer - 1; p++, n += 2) { *q++ = (u8) *p; *q++ = 0; }
	m_StringBuffer[0] = (u8) n;
	m_StringBuffer[1] = DESCRIPTOR_STRING;
	*pLength = n;
	return m_StringBuffer;
}

const void *CSidecarGadget::GetDescriptor (u16 wValue, u16 wIndex, size_t *pLength)
{
	unsigned i = wValue & 0xFF;
#ifdef CIRCLE_GADGET_MIRROR
	if (m_bShowMirror)			// the box's own, as read from it (ob_usb.cpp)
	{
		switch (wValue >> 8)
		{
		case DESCRIPTOR_DEVICE:
			if (i) return nullptr;
			*pLength = sizeof m_MirDev;
			return m_MirDev;
		case DESCRIPTOR_CONFIGURATION:
			if (i) return nullptr;
			*pLength = m_nMirCfg;
			return m_MirCfg;
		case DESCRIPTOR_STRING:
			for (unsigned k = 0; k < m_nMirStr; k++)
				if (m_MirStr[k].uchIndex == i) { *pLength = m_MirStr[k].nLength; return m_MirStr[k].Data; }
			if (i == 0)
			{
				static const u8 Lang[] = {4, DESCRIPTOR_STRING, 0x09, 0x04};
				*pLength = sizeof Lang;
				return Lang;
			}
			return nullptr;
		default:
			return nullptr;
		}
	}
#endif
	switch (wValue >> 8)
	{
	case DESCRIPTOR_DEVICE:
		if (i) break;
		*pLength = sizeof s_Device;
		return &s_Device;
	case DESCRIPTOR_CONFIGURATION:
		if (i) break;
		*pLength = s_nConfig;
		return s_Config;
	case DESCRIPTOR_STRING:
		if (i == 0)
		{
			static const u8 Lang[] = {4, DESCRIPTOR_STRING, 0x09, 0x04};
			*pLength = sizeof Lang;
			return Lang;
		}
		// "Sidecar | Expander (Pi)" (the Teensy: "Sidecar | Expander (Mini)"): CoreAudio and CoreMIDI name the device by it, so the
		// test tools pick it by its exact name (teensy/uac2/coreaudio_pick.py, UAC2_NAME)
		if (i == StrManufacturer) return String ("Sidecar", pLength);
		if (i == StrProduct || i == StrAudio || i == StrMIDI) return String (SIDECAR_NAME, pLength);
#if RASPPI >= 5
		if (i == StrSerial) return String ("PI5", pLength);
#else
		if (i == StrSerial) return String ("PI4", pLength);
#endif
		if (i >= StrOutCh && i < StrOutCh + ChOut) return String (OutNames[i - StrOutCh], pLength);
		if (i >= StrInCh && i < StrInCh + ChIn) return String (InNames[i - StrInCh], pLength);
		break;
	default:
		break;
	}
	return nullptr;
}

// ---- endpoints -------------------------------------------------------------------------------------------------

CSidecarIsoEP::CSidecarIsoEP (const TUSBEndpointDescriptor *pDesc, CSidecarGadget *pGadget, TRole Role)
:	CDWUSBGadgetEndpoint (pDesc, pGadget), m_pSidecar (pGadget), m_Role (Role), m_bRunning (FALSE)
{
}

// a new configuration: every interface at alt 0 again (a reset after a bus suspend passes no SET_INTERFACE). Stopped too: a wake's OnResume may
// have started the stream, and Start would then pass the next SET_INTERFACE by (alt 1 / 1, no packets)
void CSidecarIsoEP::OnActivate (void)
{
	m_pSidecar->m_uchOutAlt = m_pSidecar->m_uchInAlt = 0;
	Stop ();
}

void CSidecarIsoEP::OnResume (void)
{
	if (m_Role == RoleOut && m_pSidecar->m_uchOutAlt) m_pSidecar->OnSetInterface (1, m_pSidecar->m_uchOutAlt);
	else if (m_Role == RoleIn && m_pSidecar->m_uchInAlt) m_pSidecar->OnSetInterface (2, m_pSidecar->m_uchInAlt);
}

void CSidecarIsoEP::Arm (void)
{
	switch (m_Role)
	{
	case RoleOut:
		BeginTransfer (TransferDataOut, m_Buffer, (CSidecarGadget::MaxOut + 3) & ~3u);	// whole words for the DMA
		break;
	case RoleIn:
		BeginTransfer (TransferDataIn, m_Buffer, m_pSidecar->FillIn (m_Buffer));
		break;
	case RoleFeedback: {
		u32 v = m_pSidecar->Feedback (GetFrameNumber ());
		m_Buffer[0] = v; m_Buffer[1] = v >> 8; m_Buffer[2] = v >> 16; m_Buffer[3] = v >> 24;
		BeginTransfer (TransferDataIn, m_Buffer, 4);
		} break;
	}
}

void CSidecarIsoEP::Start (void)
{
	if (m_bRunning) return;
	m_bRunning = TRUE;
	Arm ();
}

void CSidecarIsoEP::Stop (void)
{
	if (!m_bRunning) return;
	m_bRunning = FALSE;
	StopIsoTransfer ();
}

void CSidecarIsoEP::OnTransferComplete (boolean bIn, size_t nLength)
{
	if (m_Role == RoleOut) m_pSidecar->TakeOut (m_Buffer, nLength);
	else if (m_Role == RoleIn) m_pSidecar->InSent ();
	else m_pSidecar->m_Stats.nFbSent++;
	if (m_bRunning) Arm ();
}

CSidecarMIDIEP::CSidecarMIDIEP (const TUSBEndpointDescriptor *pDesc, CSidecarGadget *pGadget)
:	CDWUSBGadgetEndpoint (pDesc, pGadget), m_pSidecar (pGadget), m_bActive (FALSE), m_bBusy (FALSE), m_bHeld (FALSE)
{
}

void CSidecarMIDIEP::OnActivate (void)
{
	m_bActive = TRUE;
	m_bBusy = FALSE;
	m_bHeld = FALSE;
	if (GetDirection () == DirectionIn) m_pSidecar->HostUp (TRUE);	// the computer configured us: the queue starts empty
	if (GetDirection () == DirectionOut) BeginTransfer (TransferDataOut, m_Buffer, BufSize);
	else Kick ();
}

void CSidecarMIDIEP::OnDeactivate (void)
{
	m_bActive = FALSE;
	m_bBusy = FALSE;
	m_bHeld = FALSE;
	if (GetDirection () == DirectionIn) m_pSidecar->HostUp (FALSE);	// reset or deconfigured: idle again
	CancelTransfer ();
}

void CSidecarMIDIEP::Kick (void)
{
	if (!m_bActive || m_bBusy || GetDirection () != DirectionIn) return;
	unsigned n = m_pSidecar->MIDIForHost (m_Buffer, InMax);
	if (n == 0) return;
	m_bBusy = TRUE;
	BeginTransfer (TransferDataIn, m_Buffer, n);
}

void CSidecarMIDIEP::OnTransferComplete (boolean bIn, size_t nLength)
{
	if (!bIn)
	{
		m_pSidecar->MIDIFromHost (m_Buffer, nLength);
		// usbmidi=os: an OS file comes faster than the Rytm takes it; instead of dropping packets (nMidiFull), the
		// endpoint waits (the computer's transfer is NAKed) until core 0's loop has sent enough (Poll: Resume)
		if (m_pSidecar->OsUpdate () && !m_pSidecar->ToRytmRoom ()) m_bHeld = TRUE;
		else if (m_bActive) BeginTransfer (TransferDataOut, m_Buffer, BufSize);
		return;
	}
	m_bBusy = FALSE;
	Kick ();
}

void CSidecarMIDIEP::Resume (void)
{
	if (!m_bHeld) return;
	m_bHeld = FALSE;
	if (m_bActive) BeginTransfer (TransferDataOut, m_Buffer, BufSize);
}

// ---- the gadget ------------------------------------------------------------------------------------------------

static const unsigned TonePeriod[CSidecarGadget::ChIn - 2] = {480, 400, 320, 300, 240, 200, 192, 160, 150, 128};

CSidecarGadget::CSidecarGadget (CInterruptSystem *pInterrupt)
:	CDWUSBGadget (pInterrupt, HighSpeed),
	m_pIso {0, 0, 0, 0}, m_pMIDIOut (0), m_pMIDIIn (0),
	m_uchOutAlt (0), m_uchInAlt (0), m_bMac (TRUE), m_bTest (FALSE), m_bFake (FALSE), m_bRytmMIDI (FALSE),
	m_bMirror (FALSE),
	m_nOsUpdate (OsUpdateOff), m_bLinkTcp (FALSE), m_bUsbReboot (FALSE),
	m_nInHead (0), m_nInTail (0), m_nSrcFrames (0), m_nSrcTick (0),
	m_bSrcRytm (FALSE), m_nInPos (0), m_bInPrimed (FALSE), m_bInRunning (FALSE),
	m_nOutHead (0), m_nOutTail (0), m_bOutPriming (TRUE), m_nLevelQ8 (0), m_nSinkPos (0), m_nLastConsume (0),
	m_nLoopHead (0), m_nLoopTail (0), m_bLoopPriming (TRUE),
	m_bWinStarted (FALSE), m_bWinRytm (FALSE), m_nWinUFrames (0), m_nLastFrameNo (0), m_nWinQ16 (0),
	m_nFbBase (6 << 16),
	m_ToRytmFilter (true), m_ToHostFilter (false), m_bToRytmSysEx (FALSE),
	m_nLastIRQs (0), m_nLastTicks (0), m_nLastStamp (0)
{
	s_pThis = this;
	m_nOsUpdate = ModeFrom (usbmidi_os::from_cmdline (CKernelOptions::Get ()->GetAppOptionString ("usbmidi"),
							   CKernelOptions::Get ()->GetAppOptionString ("osupdate")));
	m_bUsbReboot = CKernelOptions::Get ()->GetAppOptionDecimal ("usbreboot", 0) == 1;
#ifdef CIRCLE_GADGET_MIRROR
	m_bMirror = strcmp (CKernelOptions::Get ()->GetAppOptionString ("usb", "sidecar"), "mirror") == 0;
	memset (m_MirDev, 0, sizeof m_MirDev);
	m_nMirCfg = 0;
	memset (&m_MirLayout, 0, sizeof m_MirLayout);
	m_nMirStr = 0;
	for (unsigned k = 0; k < mirror::kMaxEps; k++) m_pMirEP[k] = 0;
	memset (m_MirAlt, 0, sizeof m_MirAlt);
	m_nMirInCh = m_nMirOutCh = 0;
	m_bMirHave = m_bMirBoxUp = m_bShowMirror = m_bSwitching = m_bSwitchTo = m_bMirKeep = FALSE;
	m_nSwitchAt = m_nBoxGoneAt = 0;
	m_nHoldSecs = CKernelOptions::Get ()->GetAppOptionDecimal ("mirrorhold", 10);	// seconds; 0..600
	if (m_nHoldSecs > 600) m_nHoldSecs = 600;
	{
		// mirrorname=off | TEXT (an underscore for a space; a space comes first): the default " via Sidecar"
		const char *pName = CKernelOptions::Get ()->GetAppOptionString ("mirrorname", 0);
		if (pName == 0) strcpy (m_MirName, " via Sidecar");
		else if (strcmp (pName, "off") == 0) m_MirName[0] = 0;
		else
		{
			unsigned n = 0;
			m_MirName[n++] = ' ';
			for (; *pName && n < sizeof m_MirName / 2; pName++) m_MirName[n++] = *pName == '_' ? ' ' : *pName;
			m_MirName[n] = 0;
		}
	}
	m_nMirInHead = m_nMirInTail = 0;
	memset (&m_CtlSetup, 0, sizeof m_CtlSetup);
	m_bCtlPending = FALSE;
	m_nCtlSeq = 0;
	memset (&m_MirStats, 0, sizeof m_MirStats);
#endif
	UpdateOsPass ();
	memset (&m_Stats, 0, sizeof m_Stats);
#if RASPPI >= 5
	memset (m_nOutLast, 0, sizeof m_nOutLast);
#endif
	m_Stats.nRingMin = m_Stats.nInMin = ~0u;
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (m_nCntFrq));
	asm volatile ("mrs %0, cntpct_el0" : "=r" (m_nT0));
	s_Device.idVendor = USB_GADGET_VENDOR_ID;
	BuildConfig (ChIn, ChOut, MaxIn, MaxOut);
	for (unsigned c = 0; c < ChIn; c++)
	{
		m_nPhase[c] = 0;
		m_pTone[c] = 0;
	}
	for (unsigned c = 0; c + 2 < ChIn; c++)
	{
		m_pTone[c] = new s32[TonePeriod[c]];
		sidecar_tone_table (TonePeriod[c], 0.25, m_pTone[c]);
	}
}

boolean CSidecarGadget::Start (void)
{
	m_nLastStamp = Now ();
	return Initialize ();				// usb=mirror too: the Sidecar device until the box has been read
}

u64 CSidecarGadget::Now (void)
{
	u64 t;
	asm volatile ("isb; mrs %0, cntpct_el0" : "=r" (t));
	return t;
}

u64 CSidecarGadget::TimerQ16 (void)
{
	return (u64) ((unsigned __int128) (Now () - m_nT0) * (48000u << 16) / m_nCntFrq);
}

void CSidecarGadget::AddEndpoints (void)
{
#ifdef CIRCLE_GADGET_MIRROR
	if (m_bShowMirror) { MirrorAddEndpoints (); return; }
#endif
	m_pIso[EPOut] = new CSidecarIsoEP ((const TUSBEndpointDescriptor *) &s_Config[s_nOffOut], this, CSidecarIsoEP::RoleOut);
	m_pIso[EPFeedback] = new CSidecarIsoEP ((const TUSBEndpointDescriptor *) &s_Config[s_nOffFb], this,
					       CSidecarIsoEP::RoleFeedback);
	m_pIso[EPIn] = new CSidecarIsoEP ((const TUSBEndpointDescriptor *) &s_Config[s_nOffIn], this, CSidecarIsoEP::RoleIn);
	m_pMIDIOut = new CSidecarMIDIEP ((const TUSBEndpointDescriptor *) &s_Config[s_nOffMIDIOut], this);
	m_pMIDIIn = new CSidecarMIDIEP ((const TUSBEndpointDescriptor *) &s_Config[s_nOffMIDIIn], this);
}

void CSidecarGadget::OnSuspend (void)
{
	// task level (UpdatePlugAndPlay, SoftConnect): the endpoints go, so does the computer. The pointers are taken with IRQs
	// masked first: the xHCI IRQ reaches the endpoints through them (FromRytmMIDI, MirrorAudioIn)
	CSidecarIsoEP *pIso[4];
	CSidecarMIDIEP *pMIDIOut, *pMIDIIn;
#ifdef CIRCLE_GADGET_MIRROR
	CMirrorEP *pMir[mirror::kMaxEps];
#endif
	EnterCritical ();
	HostUp (FALSE);
	for (unsigned i = 0; i < 4; i++) { pIso[i] = m_pIso[i]; m_pIso[i] = 0; }
	pMIDIOut = m_pMIDIOut;
	pMIDIIn = m_pMIDIIn;
	m_pMIDIOut = m_pMIDIIn = 0;
#ifdef CIRCLE_GADGET_MIRROR
	for (unsigned k = 0; k < mirror::kMaxEps; k++) { pMir[k] = m_pMirEP[k]; m_pMirEP[k] = 0; }
#endif
	LeaveCritical ();
	for (unsigned i = 1; i < 4; i++) delete pIso[i];
	delete pMIDIOut;
	delete pMIDIIn;
#ifdef CIRCLE_GADGET_MIRROR
	for (unsigned k = 0; k < mirror::kMaxEps; k++) delete pMir[k];
#endif
}

int CSidecarGadget::OnClassOrVendorRequest (const TSetupData *pSetup, u8 *pData)
{
#ifdef CIRCLE_GADGET_MIRROR
	if (m_bShowMirror) return -1;			// every one was deferred (OnControlRequest); this is a leftover: STALL
#endif
	if (!(pSetup->bmRequestType & REQUEST_IN)) return 0;		// SET CUR: one fixed rate, accepted
	unsigned n = 0;
	if ((pSetup->wIndex & 0xFF) == 0 && (pSetup->wIndex >> 8) == 0x10)
	{
		u8 uchCS = pSetup->wValue >> 8;
		if (pSetup->bRequest == 1 && uchCS == 1) { const u8 v[] = {U32 (48000)}; memcpy (pData, v, n = 4); }
		else if (pSetup->bRequest == 1 && uchCS == 2) { pData[0] = 1; n = 1; }
		else if (pSetup->bRequest == 2 && uchCS == 1)
		{
			const u8 v[] = {U16 (1), U32 (48000), U32 (48000), U32 (0)};
			memcpy (pData, v, n = 14);
		}
	}
	if (n == 0) { m_Stats.nBadReq++; return -1; }
	m_Stats.nReq++;
	return n > pSetup->wLength ? pSetup->wLength : n;
}

void CSidecarGadget::OutReset (void)
{
	m_nOutHead = m_nOutTail = 0;
	m_bOutPriming = TRUE;
	m_nLevelQ8 = 0;
	m_nLoopHead = m_nLoopTail = 0;
	m_bLoopPriming = TRUE;
}

boolean CSidecarGadget::OnSetInterface (u8 uchInterface, u8 uchAlt)
{
#ifdef CIRCLE_GADGET_MIRROR
	if (m_bShowMirror) return MirrorSetInterface (uchInterface, uchAlt);
#endif
	if (uchInterface == 0 || uchInterface == 3 || uchInterface == 4) return uchAlt == 0;
	if (uchAlt > 1) return FALSE;
	if (uchInterface == 1)
	{
		m_uchOutAlt = uchAlt;
		OutReset ();
		m_bWinStarted = FALSE;
		if (uchAlt) { m_pIso[EPOut]->Start (); m_pIso[EPFeedback]->Start (); }
		else { m_pIso[EPOut]->Stop (); m_pIso[EPFeedback]->Stop (); }
		return TRUE;
	}
	if (uchInterface == 2)
	{
		m_uchInAlt = uchAlt;
		if (uchAlt)
		{
			// the IN ring trimmed to its target: the stream starts with a short delay, not the backlog
			while (m_nInHead - m_nInTail > InTarget) m_nInTail++;
			m_bInPrimed = FALSE;
			m_bInRunning = FALSE;
			m_nInPos = (TimerQ16 () >> 16) - Nominal;
			m_pIso[EPIn]->Start ();
		}
		else m_pIso[EPIn]->Stop ();
		return TRUE;
	}
	return FALSE;
}

int CSidecarGadget::OnGetInterface (u8 uchInterface)
{
#ifdef CIRCLE_GADGET_MIRROR
	if (m_bShowMirror) return uchInterface < m_MirLayout.ifaces ? m_MirAlt[uchInterface] : -1;
#endif
	if (uchInterface == 1) return m_uchOutAlt;
	if (uchInterface == 2) return m_uchInAlt;
	return uchInterface <= 4 ? 0 : -1;
}

// ---- audio: the engine's side ----------------------------------------------------------------------------------

void CSidecarGadget::InPush (const s32 *pFrame)
{
	if (m_nInHead - m_nInTail >= Ring)
	{
		if (m_uchInAlt && m_bInRunning && !m_bTest) m_Stats.nInOver++;	// lost while the computer reads them
		m_nInTail++;					// the oldest goes (no reader yet: InSent trims)
	}
	memcpy (m_InRing[m_nInHead % Ring], pFrame, sizeof m_InRing[0]);
	m_nInHead++;
}

void CSidecarGadget::FromRytmBlock (const u8 *pBlock)
{
#ifdef CIRCLE_GADGET_MIRROR
	if (m_bShowMirror) { MirrorAudioIn (pBlock); return; }
#endif
	for (int f = 0; f < ob::kFramesPerBlock; f++)
	{
		s32 Frame[12];
		for (int c = 0; c < 12; c++) Frame[c] = ob::block_sample (pBlock, ob::kChannels, f, c) >> 8;	// 24-bit
		InPush (Frame);
	}
	m_nSrcFrames += ob::kFramesPerBlock;
	m_nSrcTick = Now ();
	m_bSrcRytm = TRUE;
}

void CSidecarGadget::FromEngine (const s32 (*pFrames)[12], unsigned n)
{
#ifdef CIRCLE_GADGET_MIRROR
	if (m_bShowMirror) return;			// usb=mirror, the box shown: its own blocks, or nothing
#endif
	EnterCritical ();
	if (Now () - m_nSrcTick > m_nCntFrq / 50 || !m_bSrcRytm)	// the Rytm, when it streams, wins
	{
		for (unsigned f = 0; f < n; f++)
		{
			s32 Frame[12];
			for (int c = 0; c < 12; c++) Frame[c] = pFrames[f][c] >> 8;
			InPush (Frame);
		}
		m_nSrcFrames += n;
		m_nSrcTick = Now ();
		m_bSrcRytm = FALSE;
	}
	LeaveCritical ();
}

// One OUT block toward the Rytm (or the self-test): the computer's frames added.
void CSidecarGadget::ToRytm (s32 (*F)[12], unsigned n, boolean bTask)
{
	if (bTask) EnterCritical ();
	for (unsigned f = 0; f < n; f++)
	{
		if (m_bTest || !m_uchOutAlt) continue;
		unsigned nLevel = m_nOutHead - m_nOutTail;
		if (m_bOutPriming)
		{
			if (nLevel < OutTarget) continue;
			m_bOutPriming = FALSE;
		}
		if (nLevel == 0)
		{
			m_Stats.nOutUnder++;
			m_bOutPriming = TRUE;
			continue;
		}
		const s32 *pMac = m_OutRing[m_nOutTail++ % Ring];
		if (!m_bMac) continue;
		for (unsigned c = 0; c < ChOut; c++)
		{
			s32 a = pMac[c] < 0 ? -pMac[c] : pMac[c];
			if ((unsigned) a > m_Stats.nMacPeak) m_Stats.nMacPeak = a;
			s64 v = (s64) F[f][c] + ((s64) pMac[c] << 8);
			F[f][c] = v > 0x7FFFFFFF ? 0x7FFFFFFF : v < -0x7FFFFFFFLL - 1 ? (s32) 0x80000000 : (s32) v;
		}
	}
	m_nLastConsume = Now ();
	if (bTask) LeaveCritical ();
}

// ---- audio: the computer's side (gadget IRQ) -------------------------------------------------------------------

void CSidecarGadget::TakeOut (const u8 *p, size_t nLength)
{
	m_Stats.nOutPkts++;
	if (nLength % (ChOut * 3)) m_Stats.nOutBad++;
	unsigned n = nLength / (ChOut * 3);
	if (n + 1 == Nominal) m_Stats.nOutHist[0]++;
	else if (n == Nominal) m_Stats.nOutHist[1]++;
	else if (n == Nominal + 1) m_Stats.nOutHist[2]++;
	m_Stats.nOutFrames += n;
	for (unsigned f = 0; f < n; f++, p += ChOut * 3)
	{
		s32 Frame[ChOut];
		for (unsigned c = 0; c < ChOut; c++)
			Frame[c] = (s32) ((p[3 * c] << 8) | (p[3 * c + 1] << 16) | (p[3 * c + 2] << 24)) >> 8;
		if (m_bTest)
		{
#if RASPPI >= 5
			for (unsigned c = 0; c < ChOut; c++)
			{
				unsigned a = Frame[c] < 0 ? -Frame[c] : Frame[c];
				if (a > m_Stats.nOutPeak[c]) m_Stats.nOutPeak[c] = a;
				if (m_nOutLast[c] <= 0 && Frame[c] > 0) m_Stats.nOutCross[c]++;
				m_nOutLast[c] = Frame[c];
			}
#endif
			if (m_nLoopHead - m_nLoopTail >= Ring) { m_Stats.nOutOver++; continue; }
			m_LoopRing[m_nLoopHead % Ring][0] = Frame[0];
			m_LoopRing[m_nLoopHead % Ring][1] = Frame[ChOut - 1];
			m_nLoopHead++;
			continue;
		}
		if (m_nOutHead - m_nOutTail >= Ring) { m_Stats.nOutOver++; continue; }
		memcpy (m_OutRing[m_nOutHead % Ring], Frame, sizeof Frame);
		m_nOutHead++;
	}
}

// IN packet sent (gadget IRQ). The first one of a stream: the computer polls IN some 10-20 ms after SET_INTERFACE,
// and the Rytm's frames meanwhile fill the ring (512 frames, then the oldest go). The ring is trimmed to its target
// here, once, so the stream goes on from InTarget frames behind the Rytm, not from a full ring that overflows on
// every block until 13-frame packets drain it (~120 ms of drops, counted as IN over, and 10 ms of extra latency).
void CSidecarGadget::InSent (void)
{
	m_Stats.nInPkts++;
	if (m_bInRunning) return;
	m_bInRunning = TRUE;
	while (m_nInHead - m_nInTail > InTarget) m_nInTail++;
}

static inline void Put24 (u8 *&p, s32 v)	{ *p++ = v; *p++ = v >> 8; *p++ = v >> 16; }

unsigned CSidecarGadget::FillIn (u8 *p)
{
	const boolean bSource = !m_bTest && Now () - m_nSrcTick < m_nCntFrq / 50;	// a source within 20 ms
	unsigned n = Nominal;
	if (bSource)
	{
		unsigned nLevel = m_nInHead - m_nInTail;
		if (nLevel < m_Stats.nInMin) m_Stats.nInMin = nLevel;
		if (nLevel > m_Stats.nInMax) m_Stats.nInMax = nLevel;
		if (!m_bInPrimed && nLevel >= InTarget) m_bInPrimed = TRUE;
		if (nLevel > InTarget + 7) n = Nominal + 1;
		else if (nLevel < InTarget - 7) n = Nominal - 1;
		m_nInPos = (TimerQ16 () >> 16) - Nominal;		// ready for a fall-back to the timer
	}
	else
	{
		// the generic timer: the frames it made due, 11..13
		u64 nDue = TimerQ16 () >> 16;
		s64 d = (s64) (nDue - m_nInPos);
		if (d > (s64) (8 * Nominal) || d < -(s64) (8 * Nominal)) { m_nInPos = nDue - Nominal; d = Nominal; }
		n = d < (s64) Nominal - 1 ? Nominal - 1 : d > (s64) Nominal + 1 ? Nominal + 1 : (unsigned) d;
		m_nInPos += n;
	}
	m_Stats.nInHist[n - (Nominal - 1)]++;
	m_Stats.nInFrames += n;

	for (unsigned f = 0; f < n; f++)
	{
		if (m_bTest)
		{
			// tones on 1..N-2, the loopbacks of OUT 1 and OUT 12 on N-1, N
			for (unsigned c = 0; c + 2 < ChIn; c++)
			{
				Put24 (p, m_pTone[c] ? m_pTone[c][m_nPhase[c]] : 0);
				if (m_pTone[c] && ++m_nPhase[c] == TonePeriod[c]) m_nPhase[c] = 0;
			}
			s32 a = 0, b = 0;
			unsigned nLevel = m_nLoopHead - m_nLoopTail;
			if (m_bLoopPriming && m_uchOutAlt && nLevel >= OutTarget) m_bLoopPriming = FALSE;
			if (!m_bLoopPriming)
			{
				if (nLevel == 0) { m_Stats.nOutUnder++; m_bLoopPriming = TRUE; }
				else
				{
					a = m_LoopRing[m_nLoopTail % Ring][0];
					b = m_LoopRing[m_nLoopTail % Ring][1];
					m_nLoopTail++;
				}
			}
			Put24 (p, a);
			Put24 (p, b);
			continue;
		}
		const s32 *pFrame = 0;
		if (bSource && m_bInPrimed)
		{
			if (m_nInHead != m_nInTail) pFrame = m_InRing[m_nInTail++ % Ring];
			else { m_Stats.nInUnder++; m_bInPrimed = FALSE; }
		}
		for (unsigned c = 0; c < 12; c++) Put24 (p, pFrame ? pFrame[c] : 0);
	}
	return n * ChIn * 3;
}

// Every feedback packet (1 ms): the source's frames (the Rytm's, the stand-in's, else the generic timer's) against
// the host's microframes, and the OUT ring's level. Without a consumer of OUT the ring drains at the timer's rate.
unsigned CSidecarGadget::Feedback (unsigned nFrame)
{
	u64 nNow = Now ();
	const boolean bSource = !m_bTest && nNow - m_nSrcTick < m_nCntFrq / 50;
	u64 q = bSource ? m_nSrcFrames << 16 : TimerQ16 ();
	const unsigned nWindow = bSource ? 65536 : 8192;
	if (!m_bWinStarted || m_bWinRytm != bSource)
	{
		m_bWinStarted = TRUE;
		m_bWinRytm = bSource;
		m_nWinQ16 = q;
		m_nWinUFrames = 0;
	}
	else
	{
		m_nWinUFrames += (nFrame - m_nLastFrameNo) & 0x3FFF;
		if (m_nWinUFrames >= nWindow)
		{
			m_nFbBase = (unsigned) ((q - m_nWinQ16) / m_nWinUFrames);
			m_nWinQ16 = q;
			m_nWinUFrames = 0;
		}
	}
	m_nLastFrameNo = nFrame;

	// the ring this feedback serves: the loopback ring (test) or the OUT ring
	unsigned *pHead = m_bTest ? &m_nLoopHead : &m_nOutHead, *pTail = m_bTest ? &m_nLoopTail : &m_nOutTail;
	boolean *pPriming = m_bTest ? &m_bLoopPriming : &m_bOutPriming;
	u64 nDue = TimerQ16 () >> 16;
	boolean bConsumer = m_bTest ? m_uchInAlt : nNow - m_nLastConsume < m_nCntFrq / 50;
	if (!bConsumer)
	{
		s64 d = (s64) (nDue - m_nSinkPos);
		if (d < 0 || d > 4096) d = 0;
		for (s64 i = 0; i < d; i++)
		{
			unsigned nLevel = *pHead - *pTail;
			if (*pPriming && nLevel >= OutTarget) *pPriming = FALSE;
			if (*pPriming) break;
			if (nLevel == 0) { *pPriming = TRUE; break; }
			(*pTail)++;
		}
	}
	m_nSinkPos = nDue;

	unsigned nLevel = *pHead - *pTail;
	if (nLevel < m_Stats.nRingMin) m_Stats.nRingMin = nLevel;
	if (nLevel > m_Stats.nRingMax) m_Stats.nRingMax = nLevel;
	m_nLevelQ8 += ((s32) (nLevel << 8) - m_nLevelQ8) >> 3;
	s32 nCorr = 0;
	if (!*pPriming)
	{
		nCorr = (s32) (((s64) ((s32) OutTarget * 256 - m_nLevelQ8) * 65536) / (8000 * 256));
		s32 nLim = m_nFbBase / 200;
		if (nCorr > nLim) nCorr = nLim;
		if (nCorr < -nLim) nCorr = -nLim;
	}
	unsigned v = (m_nFbBase + nCorr) << 1;			// per packet (2 microframes)
	m_Stats.nFb = v;
	return v;
}

// ---- MIDI ------------------------------------------------------------------------------------------------------

void CSidecarGadget::ToHost (const u8 *p)
{
	switch (m_ToHost.Push (p))
	{
	case ToHostQueue<MIDIQueue>::kQueued:	m_Stats.nMidiToHost++; break;
	case ToHostQueue<MIDIQueue>::kFull:	m_Stats.nMidiFull++; break;	// a computer is there and not reading
	default:				break;				// no computer: not queued, not counted
	}
}

// the computer configured (or lost) the MIDI IN endpoint: IRQ level, or OnSuspend's task level with IRQs masked.
// Up empties the queue (no backlog of old messages for a new computer), Down too (and back to idle)
void CSidecarGadget::HostUp (boolean bUp)
{
	if (bUp) m_ToHost.Up ();
	else m_ToHost.Down ();
}

unsigned CSidecarGadget::MIDIForHost (u8 *pBuffer, unsigned nMax)
{
	return m_ToHost.Pop (pBuffer, nMax);
}

// the computer's packets (gadget IRQ) -> the Rytm-bound filter -> the queue core 0's loop
// sends from
void CSidecarGadget::MIDIFromHost (const u8 *pBuffer, size_t nLength)
{
	for (size_t i = 0; i + 4 <= nLength; i += 4)
	{
		if (pBuffer[i] == 0) continue;				// padding
		m_Stats.nMidiFromHost++;
		if (m_bUsbReboot) m_Reboot.Feed (pBuffer + i);		// the request is a host-link SysEx: dropped below
		m_ToRytmFilter.Feed (pBuffer + i, [this] (const u8 *p)
		{
			u32 w;
			memcpy (&w, p, 4);
			if (!m_ToRytm.push (w)) m_Stats.nMidiFull++;
		});
	}
}

// the Rytm's packets (xHCI IRQ, or the fake loop with IRQs masked) -> the computer-bound filter -> the IN queue
void CSidecarGadget::FromRytmMIDI (const u8 *pPacket)
{
	if (!m_ToHost.IsUp () && !m_ToHostFilter.InSysEx ()) return;	// no computer: nothing to filter, queue or kick
	m_ToHostFilter.Feed (pPacket, [this] (const u8 *p) { ToHost (p); });
	if (m_pMIDIIn) m_pMIDIIn->Kick ();
}

void CSidecarGadget::MIDIToComputer (u32 nPacket)
{
	if (!m_ToHost.IsUp ()) return;			// no computer (a plain read: the flag only changes at IRQ level)
	u8 p[4];
	memcpy (p, &nPacket, 4);
	EnterCritical ();				// the queue's other producer is the xHCI IRQ (FromRytmMIDI)
	ToHost (p);
	LeaveCritical ();
}

boolean CSidecarGadget::TakeReboot (void)
{
	if (!m_Reboot.Matched ()) return FALSE;
	m_Reboot.Clear ();
	return TRUE;
}

unsigned CSidecarGadget::ModeFrom (unsigned nMode)
{
	return nMode == usbmidi_os::kOn ? OsUpdateRytm : nMode == usbmidi_os::kEmu ? OsUpdateEmu : OsUpdateOff;
}

const char *CSidecarGadget::OsUpdateName (void) const
{
	return usbmidi_os::name (m_nOsUpdate == OsUpdateRytm ? usbmidi_os::kOn : m_nOsUpdate == OsUpdateEmu ? usbmidi_os::kEmu
										 : usbmidi_os::kOff);
}

void CSidecarGadget::UpdateOsPass (void)
{
	EnterCritical ();					// the filter runs in the gadget IRQ
	m_ToRytmFilter.SetPassOsUpdate (OsPass ());
	LeaveCritical ();
}

void CSidecarGadget::SetLinkTcp (boolean bTcp)
{
	if (bTcp == m_bLinkTcp) return;
	m_bLinkTcp = bTcp;
	UpdateOsPass ();
	if (OsUpdate ())		// another source for the computer: half a SysEx of the old one never lands before the new one's
	{
		EnterCritical ();
		m_ToHostFilter.Reset ();
		if (m_ToHost.IsUp ()) m_ToHost.Up ();	// empties the queue; the computer stays up (tohost_q.h)
		LeaveCritical ();
	}
	CLogger::Get ()->Write (From, LogNotice, "usbmidi=os:%s: OS upgrades %s (host: %s)", OsUpdateName (),
				OsPass () ? "pass" : "are dropped", bTcp ? "the emulator" : "USB");
}

// a whole OUT transfer (BufSize / 4 packets) and what the filter still holds (3 packets)
boolean CSidecarGadget::ToRytmRoom (void) const
{
	return m_ToRytm.size () + 512 / 4 + 3 <= MIDIQueue;
}

boolean CSidecarGadget::ForRytm (u32 *pPacket)
{
	if (!m_ToRytm.pop (pPacket)) return FALSE;
	u8 cin = *pPacket & 0x0F, b1 = (*pPacket >> 8) & 0xFF;
	if (cin == 0x4 && b1 == 0xF0) m_bToRytmSysEx = TRUE;
	else if (cin == 0x5 || cin == 0x6 || cin == 0x7) { if (m_bToRytmSysEx || b1 == 0xF7) m_bToRytmSysEx = FALSE; }
	m_Stats.nMidiToRytm++;
	return TRUE;
}

void CSidecarGadget::Poll (void)
{
#ifdef CIRCLE_GADGET_MIRROR
	if (m_bMirror) MirrorPoll ();
#endif
	UpdatePlugAndPlay ();
	if (OsUpdate () && m_pMIDIOut != 0 && m_pMIDIOut->Held () && ToRytmRoom ())
	{
		EnterCritical ();
		m_pMIDIOut->Resume ();
		LeaveCritical ();
	}
	// :midifake=on without the Rytm's MIDI: what would go to the Rytm comes back as if the Rytm had sent it
	if (m_bFake && !m_bRytmMIDI)
	{
		u32 w;
		for (unsigned k = 0; k < 256 && ForRytm (&w); k++)
		{
			u8 p[4];
			memcpy (p, &w, 4);
			EnterCritical ();
			FromRytmMIDI (p);
			LeaveCritical ();
		}
	}
	else if (!m_bRytmMIDI)
	{
		u32 w;
		while (ForRytm (&w)) m_Stats.nMidiNoRytm++;		// nowhere to go: counted
	}
}

boolean CSidecarGadget::Command (const char *p)
{
	CLogger *L = CLogger::Get ();
	if (strcmp (p, "mac=on") == 0 || strcmp (p, "mac=off") == 0) m_bMac = p[5] == 'n';
	else if (strcmp (p, "uac2test=on") == 0 || strcmp (p, "uac2test=off") == 0)
	{
		EnterCritical ();
		m_bTest = p[10] == 'n';
		OutReset ();
		m_bWinStarted = FALSE;
		LeaveCritical ();
	}
	else if (strcmp (p, "midifake=on") == 0 || strcmp (p, "midifake=off") == 0) m_bFake = p[10] == 'n';
	else if (strncmp (p, "usbmidi=", 8) == 0 || strncmp (p, "osupdate=", 9) == 0)	// osupdate=: the older name
	{
		bool bKnown;
		usbmidi_os::Mode n = usbmidi_os::parse_command (p, &bKnown);
		if (n == usbmidi_os::kInvalid) L->Write (From, LogWarning, "usbmidi=os:on|off|emu, not \"%s\"", p);
		else
		{
			m_nOsUpdate = ModeFrom (n);
			UpdateOsPass ();
			L->Write (From, LogNotice, "usbmidi=os:%s: OS upgrades %s (host: %s)", OsUpdateName (),
				  OsPass () ? "pass" : "are dropped", m_bLinkTcp ? "the emulator" : "USB");
		}
		return TRUE;
	}
	else if (strcmp (p, "mac") != 0 && strcmp (p, "sidecar") != 0) return FALSE;
	L->Write (From, LogNotice, "mac %s (the computer's OUT %s the Rytm), uac2test %s, midifake %s, %u in / %u out",
		  m_bMac ? "on" : "off", m_bMac ? "added to what goes to" : "dropped before", m_bTest ? "on" : "off",
		  m_bFake ? "on" : "off", ChIn, ChOut);
	return TRUE;
}

void CSidecarGadget::LogStats (CLogger *pLogger)
{
#ifdef CIRCLE_GADGET_MIRROR
	if (m_bShowMirror) { MirrorLogStats (pLogger); return; }
#endif
	unsigned nIRQs;
	u64 nTicks, nNow = Now ();
	GetIRQStats (&nIRQs, &nTicks);
	u64 nSpan = nNow - m_nLastStamp;
	if (nSpan == 0) return;
	unsigned nRate = (unsigned) ((u64) (nIRQs - m_nLastIRQs) * m_nCntFrq / nSpan);
	unsigned nPpm = (unsigned) ((nTicks - m_nLastTicks) * 1000000 / nSpan);
	m_nLastIRQs = nIRQs;
	m_nLastTicks = nTicks;
	m_nLastStamp = nNow;
	u64 nMaxGap, nMaxHandler;
	unsigned nLong;
	TakeIRQGaps (&nMaxGap, &nMaxHandler, &nLong, m_nCntFrq * 300 / 1000000);	// gaps over 300 us
	u64 nLate = m_pIso[EPIn] ? m_pIso[EPIn]->LateStamp () : 0;
	unsigned nLateAgo = nLate ? (unsigned) ((nNow - nLate) * 1000 / m_nCntFrq) : 0;	// ms
	EnterCritical ();
	TStats s = m_Stats;
	m_Stats.nRingMin = m_Stats.nInMin = ~0u;
	m_Stats.nRingMax = m_Stats.nInMax = 0;
	m_Stats.nMacPeak = 0;
#if RASPPI >= 5
	memset (m_Stats.nOutCross, 0, sizeof m_Stats.nOutCross);
	memset (m_Stats.nOutPeak, 0, sizeof m_Stats.nOutPeak);
#endif
	LeaveCritical ();
	const boolean bSource = (s64) (nNow - m_nSrcTick) < (s64) (m_nCntFrq / 50);	// the IRQ may set it after nNow
	unsigned nFbmHz = (unsigned) ((u64) s.nFb * 4000 * 1000 / 65536);
	pLogger->Write (From, LogNotice,
		"%s alt %u/%u mac %s pk %u | out %u pkts (11/12/13 %u/%u/%u) bad %u over %u under %u ring %u..%u | "
		"in %u pkts (11/12/13 %u/%u/%u) ring %u..%u under %u over %u | fb %u.%03u | late %u/%u/%u incompl %u/%u/%u | "
		"midi from %u to-mac %u to-rytm %u full %u norytm %u drop %u/%u mac-midi %s (up %u) | irq %u/s %u.%02u %% gap max %u us handler max %u us >300us %u, last late %u ms ago",
		m_bTest ? "test" : bSource ? (m_bSrcRytm ? "rytm" : "standin") : "timer", m_uchOutAlt, m_uchInAlt,
		m_bMac ? "on" : "off", s.nMacPeak,
		s.nOutPkts, s.nOutHist[0], s.nOutHist[1], s.nOutHist[2], s.nOutBad, s.nOutOver, s.nOutUnder,
		s.nRingMin == ~0u ? 0 : s.nRingMin, s.nRingMax,
		s.nInPkts, s.nInHist[0], s.nInHist[1], s.nInHist[2], s.nInMin == ~0u ? 0 : s.nInMin, s.nInMax, s.nInUnder, s.nInOver,
		nFbmHz / 1000, nFbmHz % 1000,
		m_pIso[EPOut] ? m_pIso[EPOut]->Late () : 0, m_pIso[EPFeedback] ? m_pIso[EPFeedback]->Late () : 0,
		m_pIso[EPIn] ? m_pIso[EPIn]->Late () : 0,
		m_pIso[EPOut] ? m_pIso[EPOut]->Incomplete () : 0, m_pIso[EPFeedback] ? m_pIso[EPFeedback]->Incomplete () : 0,
		m_pIso[EPIn] ? m_pIso[EPIn]->Incomplete () : 0,
		s.nMidiFromHost, s.nMidiToHost, s.nMidiToRytm, s.nMidiFull, s.nMidiNoRytm,
		m_ToRytmFilter.DroppedMessages (), m_ToHostFilter.DroppedMessages (),
		m_ToHost.IsUp () ? "up" : "none", m_ToHost.Ups (), nRate, nPpm / 10000, nPpm / 100 % 100,
		(unsigned) (nMaxGap * 1000000 / m_nCntFrq), (unsigned) (nMaxHandler * 1000000 / m_nCntFrq), nLong, nLateAgo);
#if RASPPI >= 5
	if (m_bTest)		// each OUT channel over this second: crossings (~Hz), peak in 1/1000 of full scale (-12 dBFS: 251)
	{
		CString Line;
		for (unsigned c = 0; c < ChOut; c++)
		{
			CString Item;
			Item.Format (" %u:%u/%u", c + 1, s.nOutCross[c], (unsigned) ((u64) s.nOutPeak[c] * 1000 / 8388607));
			Line.Append (Item);
		}
		pLogger->Write (From, LogNotice, "test out Hz/peak:%s", (const char *) Line);
	}
#endif
}

// ---- usb=mirror (docs/usb-mirror.md) ---------------------------------------------------------------------------
#ifdef CIRCLE_GADGET_MIRROR

CMirrorEP::CMirrorEP (const mirror::Ep &rEp, CSidecarGadget *pGadget)
:	CDWUSBGadgetEndpoint ((const TUSBEndpointDescriptor *) rEp.desc, pGadget),
	m_pSidecar (pGadget), m_Role (rEp.role), m_uchAddr (rEp.addr), m_nMps (rEp.mps > 512 ? 512 : rEp.mps),
	m_bActive (FALSE), m_bBusy (FALSE)
{
}

void CMirrorEP::OnActivate (void)
{
	m_bActive = TRUE;
	m_bBusy = FALSE;
	if (m_Role == mirror::kAudioOut || m_Role == mirror::kPipeOut) Rearm ();
	else Kick ();
}

void CMirrorEP::OnDeactivate (void)
{
	boolean bHeld = m_bBusy;
	m_bActive = FALSE;
	m_bBusy = FALSE;
	CancelTransfer ();
	if (bHeld && m_Role == mirror::kPipeIn) m_pSidecar->MirrorPipeInSent (m_uchAddr);	// the box's URB goes on
}

// OUT: the next packet (whole words for the DMA; the endpoint's size, so a full-size packet ends the transfer)
void CMirrorEP::Rearm (void)
{
	if (!m_bActive) return;
	m_bBusy = FALSE;
	BeginTransfer (TransferDataOut, m_Buffer, (m_nMps + 3) & ~3u);
}

boolean CMirrorEP::Send (const u8 *pData, unsigned nLength)
{
	if (!m_bActive || m_bBusy || nLength > sizeof m_Buffer) return FALSE;
	m_bBusy = TRUE;
	if (nLength) memcpy (m_Buffer, pData, nLength);
	BeginTransfer (TransferDataIn, nLength ? m_Buffer : nullptr, nLength);	// a zero-length packet too
	return TRUE;
}

void CMirrorEP::Kick (void)
{
	if (!m_bActive || m_bBusy) return;
	if (m_Role == mirror::kAudioIn)
	{
		unsigned n = m_pSidecar->MirrorFillIn (m_Buffer);
		if (n == 0) return;
		m_bBusy = TRUE;
		BeginTransfer (TransferDataIn, m_Buffer, n);
	}
}

void CMirrorEP::OnTransferComplete (boolean bIn, size_t nLength)
{
	switch (m_Role)
	{
	case mirror::kAudioIn:
		m_bBusy = FALSE;
		m_pSidecar->m_MirStats.nInSent++;
		Kick ();
		break;
	case mirror::kAudioOut:
		m_pSidecar->MirrorAudioOut (m_Buffer, nLength);
		Rearm ();
		break;
	case mirror::kPipeOut:
		m_bBusy = TRUE;					// NAKed until the box took it (MirrorPipeOutDone)
		m_pSidecar->MirrorPipeOut (this, m_Buffer, nLength);
		break;
	case mirror::kPipeIn:
		m_bBusy = FALSE;
		m_pSidecar->MirrorPipeInSent (m_uchAddr);
		break;
	default:
		break;
	}
}

CMirrorEP *CSidecarGadget::MirrorEP (u8 uchAddr)
{
	for (unsigned k = 0; k < mirror::kMaxEps; k++)
		if (m_pMirEP[k] != 0 && m_pMirEP[k]->Address () == uchAddr) return m_pMirEP[k];
	return 0;
}

// ob_usb.cpp, at the box's configuration (task level). The same box again (a replug, a reboot after an OS update) while
// the computer sees it: nothing changes. Another box while it is shown: the gadget leaves the bus first, and MirrorPoll
// brings it back as the new box.
boolean CSidecarGadget::MirrorSetDescriptors (const u8 *pDev, const u8 *pCfg, unsigned nCfg)
{
	mirror::Layout l;
	if (pDev[0] != sizeof m_MirDev || pDev[1] != DESCRIPTOR_DEVICE || pDev[7] != 64
	    || !mirror::parse (pCfg, nCfg, ob::kInEp, ob::kOutEp, &l))
	{
		CLogger::Get ()->Write (From, LogError, "usb=mirror: the box's descriptors are not ones the USB-C side can carry");
		return FALSE;
	}
	boolean bSame = m_bMirHave && nCfg == m_nMirCfg && memcmp (pCfg, m_MirCfg, nCfg) == 0
			&& memcmp (pDev, m_MirDev, sizeof m_MirDev) == 0;
	m_bMirKeep = bSame;				// its strings too
	if (bSame) return TRUE;
	if (m_bShowMirror && !m_bSwitching) MirrorSwitch (TRUE);	// off the bus before the descriptors change under EP0
	memcpy (m_MirDev, pDev, sizeof m_MirDev);
	memcpy (m_MirCfg, pCfg, nCfg);
	m_nMirCfg = nCfg;
	m_MirLayout = l;
	m_nMirStr = 0;
	m_bMirHave = FALSE;				// until MirrorBoxUp: the strings are read in between
	unsigned nRoles[7] = {0};
	for (unsigned k = 0; k < l.n; k++) nRoles[l.ep[k].role]++;
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: %04x:%04x, %u B configuration, %u interfaces: audio %u/%u, pipes %u/%u, "
				"midi %u/%u, other %u", (unsigned) (pDev[8] | pDev[9] << 8), (unsigned) (pDev[10] | pDev[11] << 8),
				nCfg, l.ifaces, nRoles[mirror::kAudioIn], nRoles[mirror::kAudioOut], nRoles[mirror::kPipeIn],
				nRoles[mirror::kPipeOut], nRoles[mirror::kMidiIn], nRoles[mirror::kMidiOut], nRoles[mirror::kNone]);
	return TRUE;
}

void CSidecarGadget::MirrorSetString (u8 uchIndex, const u8 *pDesc, unsigned nLength)
{
	if (m_bMirKeep || m_nMirStr == mirror::kMaxStrings || nLength < 2 || nLength > sizeof m_MirStr[0].Data) return;
	m_MirStr[m_nMirStr].uchIndex = uchIndex;
	m_MirStr[m_nMirStr].nLength = (u8) nLength;
	memcpy (m_MirStr[m_nMirStr].Data, pDesc, nLength);
	m_nMirStr++;
}

void CSidecarGadget::MirrorBoxUp (boolean bUp)
{
	if (bUp && m_nMirCfg != 0) m_bMirHave = TRUE;
	EnterCritical ();
	m_bMirBoxUp = bUp && m_bMirHave;
	m_nMirInHead = m_nMirInTail = 0;
	LeaveCritical ();
	if (!m_bMirBoxUp) m_nBoxGoneAt = Now ();
	CLogger::Get ()->Write (From, LogNotice, "usb=mirror: box %s", m_bMirBoxUp ? "up"
				: m_bShowMirror ? "gone (requests are STALLed; the Sidecar device after mirrorhold)" : "gone");
}

// task level: off the bus; MirrorPoll comes back as the other device (or the box's new descriptors) after 300 ms
void CSidecarGadget::MirrorSwitch (boolean bToMirror)
{
	SoftDisconnect ();
	m_bSwitching = TRUE;
	m_bSwitchTo = bToMirror;
	m_nSwitchAt = Now ();
}

void CSidecarGadget::MirrorAddEndpoints (void)
{
	for (unsigned k = 0; k < m_MirLayout.n; k++)
	{
		const mirror::Ep &e = m_MirLayout.ep[k];
		switch (e.role)
		{
		case mirror::kMidiOut:
			if (m_pMIDIOut == 0) m_pMIDIOut = new CSidecarMIDIEP ((const TUSBEndpointDescriptor *) e.desc, this);
			break;
		case mirror::kMidiIn:
			if (m_pMIDIIn == 0) m_pMIDIIn = new CSidecarMIDIEP ((const TUSBEndpointDescriptor *) e.desc, this);
			break;
		case mirror::kNone:
			break;					// an interface of another class: its endpoint NAKs
		default:
			m_pMirEP[k] = new CMirrorEP (e, this);
			break;
		}
	}
}

// SET_INTERFACE (gadget IRQ): the computer's choice, kept here; the box stays at the alternate the engine opened
// (ob_usb.cpp, alt 3). The audio endpoints' channels follow the alternate's size (ob::block_bytes).
boolean CSidecarGadget::MirrorSetInterface (u8 uchInterface, u8 uchAlt)
{
	if (!mirror::has_alt (m_MirCfg, m_nMirCfg, uchInterface, uchAlt)) return FALSE;
	m_MirAlt[uchInterface] = uchAlt;
	auto Channels = [this] (u8 uchIface, u8 uchAltSet, u8 uchAddr) -> unsigned
	{
		unsigned m = mirror::alt_mps (m_MirCfg, m_nMirCfg, uchIface, uchAltSet, uchAddr);
		if (m < (unsigned) ob::kHeaderBytes) return 0;
		unsigned d = m - ob::kHeaderBytes, f = ob::kFramesPerBlock * 4;
		if (d % f != 0 || d / f > (unsigned) ob::kChannels) { m_MirStats.nAltBad++; return 0; }
		return d / f;
	};
	if (m_MirLayout.audio_in >= 0 && m_MirLayout.ep[m_MirLayout.audio_in].iface == uchInterface)
	{
		m_uchInAlt = uchAlt;
		m_nMirInCh = Channels (uchInterface, uchAlt, ob::kInEp);
		m_nMirInHead = m_nMirInTail = 0;
	}
	if (m_MirLayout.audio_out >= 0 && m_MirLayout.ep[m_MirLayout.audio_out].iface == uchInterface)
	{
		m_nMirOutCh = Channels (uchInterface, uchAlt, ob::kOutEp);
		m_uchOutAlt = m_nMirOutCh ? uchAlt : 0;
		OutReset ();
	}
	return TRUE;
}

// xHCI IRQ: a 12-channel IN block of the box, queued for the computer (cut to its alternate's channels)
void CSidecarGadget::MirrorAudioIn (const u8 *pBlock)
{
	m_MirStats.nInBlocks++;
	if (!m_nMirInCh) return;
	if (m_nMirInHead - m_nMirInTail >= MirInDepth) { m_nMirInTail++; m_MirStats.nInDrop++; }
	u8 *d = m_MirIn[m_nMirInHead % MirInDepth];
	if (m_nMirInCh == (unsigned) ob::kChannels) memcpy (d, pBlock, ob::block_bytes (ob::kChannels));
	else
	{
		memcpy (d, pBlock, ob::kHeaderBytes);
		for (int f = 0; f < ob::kFramesPerBlock; f++)
			memcpy (d + ob::kHeaderBytes + 4 * m_nMirInCh * f, pBlock + ob::kHeaderBytes + 4 * ob::kChannels * f, 4 * m_nMirInCh);
	}
	m_nMirInHead++;
	CMirrorEP *pEP = m_MirLayout.audio_in >= 0 ? m_pMirEP[m_MirLayout.audio_in] : 0;
	if (pEP) pEP->Kick ();
}

unsigned CSidecarGadget::MirrorFillIn (u8 *pBuffer)
{
	if (m_nMirInHead == m_nMirInTail || !m_nMirInCh) return 0;
	unsigned n = ob::block_bytes (m_nMirInCh);
	memcpy (pBuffer, m_MirIn[m_nMirInTail++ % MirInDepth], n);
	return n;
}

// gadget IRQ: the computer's OUT block, its frames into the OUT ring that ToRytm adds to what the engine sends
void CSidecarGadget::MirrorAudioOut (const u8 *p, unsigned nLength)
{
	unsigned ch = m_nMirOutCh;
	if (ch == 0 || nLength != (unsigned) ob::block_bytes (ch)) { m_MirStats.nOutBad++; return; }
	m_MirStats.nOutBlocks++;
	for (int f = 0; f < ob::kFramesPerBlock; f++)
	{
		if (m_nOutHead - m_nOutTail >= Ring) { m_Stats.nOutOver++; continue; }
		s32 *pFrame = m_OutRing[m_nOutHead % Ring];
		for (unsigned c = 0; c < ChOut; c++) pFrame[c] = c < ch ? ob::block_sample (p, ch, f, c) >> 8 : 0;	// 24-bit
		m_nOutHead++;
	}
}

void CSidecarGadget::MirrorPipeOut (CMirrorEP *pEP, const u8 *pData, unsigned nLength)
{
	if (m_bMirBoxUp && CObUsb::MirrorPipeOut (pEP->Address (), pData, nLength)) { m_MirStats.nPipeOut++; return; }
	m_MirStats.nPipeOutDrop++;
	pEP->Rearm ();
}

void CSidecarGadget::MirrorPipeOutDone (u8 uchAddr)
{
	CMirrorEP *pEP = MirrorEP (uchAddr);
	if (pEP) pEP->Rearm ();
}

boolean CSidecarGadget::MirrorPipeIn (u8 uchAddr, const u8 *pData, unsigned nLength)
{
	CMirrorEP *pEP = MirrorEP (uchAddr);
	if (pEP == 0 || !pEP->Active () || nLength > 512) { m_MirStats.nPipeInDrop++; return FALSE; }
	if (!pEP->Send (pData, nLength)) { m_MirStats.nPipeInDrop++; return FALSE; }
	m_MirStats.nPipeIn++;
	return TRUE;
}

void CSidecarGadget::MirrorPipeInSent (u8 uchAddr)
{
	CObUsb::MirrorPipeInNext (uchAddr);
}

// EP0 (gadget IRQ): every class and vendor request goes to the box; the answer comes back in MirrorControlDone
int CSidecarGadget::OnControlRequest (const TSetupData *pSetup, const u8 *pOutData)
{
	if (!m_bShowMirror) return ControlNotHandled;
	if (m_bCtlPending) { m_MirStats.nCtlStall++; return ControlNotHandled; }	// one at a time: this one is STALLed
	memcpy (&m_CtlSetup, pSetup, sizeof m_CtlSetup);
	if (pOutData && pSetup->wLength) memcpy (m_CtlData, pOutData, pSetup->wLength <= sizeof m_CtlData ? pSetup->wLength : sizeof m_CtlData);
	if (pOutData && IsNameRequest (pSetup, 0x40) && pSetup->wLength == mirror::kNameLen)
		mirror::name_store (m_CtlData, mirror::kNameLen, m_MirName);	// the box keeps its plain name
	m_nCtlSeq++;
	m_bCtlPending = TRUE;
	m_MirStats.nCtl++;
	return ControlDeferred;
}

void CSidecarGadget::OnControlAborted (void)
{
	m_nCtlSeq++;					// an answer still on its way is for nobody
	m_bCtlPending = FALSE;
	m_MirStats.nCtlAbort++;
}

void CSidecarGadget::MirrorControlDone (unsigned nSeq, const u8 *pData, int nLength)
{
	if (nSeq != m_nCtlSeq) { m_MirStats.nCtlLate++; return; }
	if (nLength < 0) m_MirStats.nCtlStall++;
	if (nLength > 0 && IsNameRequest (&m_CtlSetup, 0xC0))
	{
		u8 Name[mirror::kNameLen];			// the computer sees the name with the suffix
		unsigned n = (unsigned) nLength < sizeof Name ? (unsigned) nLength : sizeof Name;
		memset (Name, 0, sizeof Name);
		memcpy (Name, pData, n);
		mirror::name_show (Name, n, m_MirName);
		CompleteControl (Name, (int) n);
		return;
	}
	CompleteControl (pData, nLength);
}

// Overbridge's name request (mirror_desc.h), from an Elektron box, with the suffix on
boolean CSidecarGadget::IsNameRequest (const TSetupData *pSetup, u8 uchType) const
{
	return m_MirName[0] != 0 && mirror::le16 (m_MirDev + 8) == mirror::kNameVendor && pSetup->bmRequestType == uchType
		&& pSetup->bRequest == mirror::kNameRequest && pSetup->wValue == 0 && pSetup->wIndex == 0;
}

void CSidecarGadget::MirrorPoll (void)
{
	// which device the computer should see: the box once it has been read; the Sidecar after the box has been gone for
	// mirrorhold seconds (a box that reboots, as after an OS update, comes back as itself)
	u64 nNow = Now ();
	if (!m_bSwitching) CObUsb::MirrorAttachLate ();	// a box that came up before this gadget did (the boot order)
	if (m_bSwitching)
	{
		if (nNow - m_nSwitchAt < m_nCntFrq * 3 / 10) return;
		EnterCritical ();
		m_bShowMirror = m_bSwitchTo && m_bMirHave;
		m_uchOutAlt = m_uchInAlt = 0;
		OutReset ();
		memset (m_MirAlt, 0, sizeof m_MirAlt);
		m_nMirInCh = m_nMirOutCh = 0;
		m_nMirInHead = m_nMirInTail = 0;
		m_bCtlPending = FALSE;
		m_nCtlSeq++;
		LeaveCritical ();
		boolean bOK = SoftConnect ();
		m_bSwitching = FALSE;
		CLogger::Get ()->Write (From, bOK ? LogNotice : LogError, "usb=mirror: USB-C %s as %s", bOK ? "back" : "failed to come back",
					m_bShowMirror ? "the box" : "the Sidecar device");
		return;
	}
	boolean bWant = m_bShowMirror;
	if (m_bMirBoxUp) bWant = TRUE;
	else if (m_bShowMirror && nNow - m_nBoxGoneAt >= (u64) m_nHoldSecs * m_nCntFrq) bWant = FALSE;
	if (bWant != m_bShowMirror)
	{
		CLogger::Get ()->Write (From, LogNotice, "usb=mirror: USB-C leaves the bus, back as %s", bWant ? "the box" : "the Sidecar device");
		MirrorSwitch (bWant);
		return;
	}
	if (!m_bShowMirror) return;
	if (!m_bCtlPending) return;
	TSetupData Setup;
	u8 Data[sizeof m_CtlData];
	EnterCritical ();
	unsigned nSeq = m_nCtlSeq;
	boolean bTake = m_bCtlPending;
	m_bCtlPending = FALSE;
	memcpy (&Setup, &m_CtlSetup, sizeof Setup);
	memcpy (Data, m_CtlData, sizeof Data);
	LeaveCritical ();
	if (!bTake) return;
	if (m_bMirBoxUp && CObUsb::MirrorControl (&Setup, Data, nSeq)) return;
	EnterCritical ();				// no box: STALL (unless the host gave up meanwhile)
	if (nSeq == m_nCtlSeq) { m_MirStats.nCtlStall++; CompleteControl (0, -1); }
	LeaveCritical ();
}

void CSidecarGadget::MirrorLogStats (CLogger *pLogger)
{
	EnterCritical ();
	TMirStats s = m_MirStats;
	unsigned nOutUnder = m_Stats.nOutUnder, nOutOver = m_Stats.nOutOver;
	LeaveCritical ();
	pLogger->Write (From, LogNotice,
		"mirror %s box %s | in ch %u blocks %u sent %u drop %u | out ch %u blocks %u bad %u under %u over %u | "
		"ctl %u stall %u abort %u late %u | pipe in %u drop %u out %u drop %u | alt bad %u | midi from %u to-mac %u to-rytm %u",
		"shown", m_bMirBoxUp ? "up" : "none", m_nMirInCh, s.nInBlocks, s.nInSent, s.nInDrop,
		m_nMirOutCh, s.nOutBlocks, s.nOutBad, nOutUnder, nOutOver, s.nCtl, s.nCtlStall, s.nCtlAbort, s.nCtlLate,
		s.nPipeIn, s.nPipeInDrop, s.nPipeOut, s.nPipeOutDrop, s.nAltBad,
		m_Stats.nMidiFromHost, m_Stats.nMidiToHost, m_Stats.nMidiToRytm);
}

#endif	// CIRCLE_GADGET_MIRROR


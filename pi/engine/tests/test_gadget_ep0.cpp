// pi/engine/tests/test_gadget_ep0.cpp - the DWC2 gadget's EP0 and data endpoints (Circle's dwusbgadgetendpoint*.cpp with
// the Pi 4 patch stack up to pi/circle/circle-gadget-unplug.patch) on a fake register file (shim_dwc/). The core is
// modelled as far as these need: in buffer DMA mode a SETUP is written where DOEPDMA0 points (EP0 OUT armed or not:
// the DMA address then is just past the last OUT transfer) and raises SETUP done; IN / OUT packets move only while
// their EP is enabled; EPDis on an enabled EP raises EP disabled (not on EP0 OUT); interrupt bits are write-1-to-clear.
//
// The crash of 2026-11-06 (Pi 4, serial: dwusbgadgetendpoint.cpp(550)): a SETUP while EP0 waited for a deferred
// answer, nothing armed, FinishTransfer() asserted. Here such a SETUP is read back from where the core wrote it.
// Also the Sidecar's own (CLASS COMPLIANT) traffic: SET_INTERFACE, a class OUT request with data, a bulk IN, a bulk
// OUT and an isochronous IN transfer complete. (rc5's rework of EP0 and of the endpoint disable broke them on the
// Pi 4, in a way this model does not show; that rework is gone.)
//
// The host's side: Setup () sends a SETUP, In () reads one packet (or NAK, or STALL), Out () sends one (or NAK).
#include <circle/usb/gadget/dwusbgadgetendpoint0.h>
#include <circle/usb/gadget/dwusbgadget.h>
#include <circle/usb/dwhciregister.h>
#include <circle/logger.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <vector>

#include "check.h"
#include "mirror_desc.h"
#include "mirror_ep.h"
#include "ep_stop.h"
#include "ncm_ntb.h"

// ---- the fake core: the endpoints live in one arena, bus addresses are offsets into it (DMA arithmetic holds)
alignas (64) static u8 g_Arena[64 * 1024];
static size_t g_nArena;
static void *Alloc (size_t n)
{
	void *p = g_Arena + g_nArena;
	g_nArena += (n + 63) & ~(size_t) 63;
	CHECK (g_nArena <= sizeof g_Arena);
	return p;
}
static const u32 kBusBase = 0x10000;
u32 FakeBusAddress (uintptr p)
{
	CHECK (p >= (uintptr) g_Arena && p <= (uintptr) g_Arena + sizeof g_Arena);
	return kBusBase + (u32) (p - (uintptr) g_Arena);
}
static u8 *Ptr (u32 nBus) { return g_Arena + (nBus - kBusBase); }

static std::map<u32, u32> g_Reg;

static bool IsCtrl (u32 a, bool *pIn, unsigned *pEP)
{
	for (unsigned ep = 0; ep < 8; ep++)
	{
		if (a == DWHCI_DEV_IN_EP_CTRL (ep)) { *pIn = true, *pEP = ep; return true; }
		if (a == DWHCI_DEV_OUT_EP_CTRL (ep)) { *pIn = false, *pEP = ep; return true; }
	}
	return false;
}
static bool IsInt (u32 a)
{
	for (unsigned ep = 0; ep < 8; ep++) if (a == DWHCI_DEV_IN_EP_INT (ep) || a == DWHCI_DEV_OUT_EP_INT (ep)) return true;
	return false;
}

unsigned g_FakeClock;
static unsigned g_nReads;
// the core's answers can be withheld (a vanished host, a gated PHY: no GOUTNAKEff, no EPDisbld), a completion can race
// the handler of "EP disabled" (raised when that interrupt is acknowledged), and the TX FIFO flushes are recorded
static bool g_bMute;
static unsigned g_nRaceEP = ~0u;
static bool g_bRaceIn;
static std::vector<unsigned> g_Flushes;

u32 FakeRead (u32 a)
{
	g_nReads++, g_FakeClock++;
	return a == DWHCI_CORE_RESET ? g_Reg[a] | DWHCI_CORE_RESET_AHB_IDLE : g_Reg[a];
}

void FakeWrite (u32 a, u32 v)
{
	bool bIn; unsigned ep;
	if (IsInt (a))
	{
		g_Reg[a] &= ~v;
		if (g_nRaceEP != ~0u && (v & DWHCI_DEV_IN_EP_INT_EP_DISABLED) && a == (g_bRaceIn ? DWHCI_DEV_IN_EP_INT (g_nRaceEP) : DWHCI_DEV_OUT_EP_INT (g_nRaceEP)))
		{
			// the transfer completes just after the handler read the interrupt register: EPEna cleared, XFER_COMPLETE set
			g_Reg[g_bRaceIn ? DWHCI_DEV_IN_EP_CTRL (g_nRaceEP) : DWHCI_DEV_OUT_EP_CTRL (g_nRaceEP)] &= ~DWHCI_DEV_EP_CTRL_EP_ENABLE;
			g_Reg[a] |= DWHCI_DEV_IN_EP_INT_XFER_COMPLETE;
			g_nRaceEP = ~0u;
		}
		return;
	}
	if (IsCtrl (a, &bIn, &ep))
	{
		const u32 Act = DWHCI_DEV_EP_CTRL_EP_DISABLE | DWHCI_DEV_EP_CTRL_SET_NAK | DWHCI_DEV_EP_CTRL_CLEAR_NAK
				| DWHCI_DEV_EP_CTRL_SETDPID_D0 | DWHCI_DEV_EP_CTRL_SETDPID_D1 | DWHCI_DEV_EP_CTRL_EP_ENABLE;
		u32 old = g_Reg[a];
		u32 now = (v & ~Act) | (old & DWHCI_DEV_EP_CTRL_EP_ENABLE) | (v & DWHCI_DEV_EP_CTRL_EP_ENABLE);
		if ((v & DWHCI_DEV_EP_CTRL_EP_DISABLE) && (old & DWHCI_DEV_EP_CTRL_EP_ENABLE) && !(ep == 0 && !bIn))
		{
			now &= ~DWHCI_DEV_EP_CTRL_EP_ENABLE;
			if (!g_bMute) g_Reg[bIn ? DWHCI_DEV_IN_EP_INT (ep) : DWHCI_DEV_OUT_EP_INT (ep)] |= DWHCI_DEV_IN_EP_INT_EP_DISABLED;
		}
		g_Reg[a] = now;
		return;
	}
	if (a == DWHCI_DEV_CTRL)
	{
		if ((v & DWHCI_DEV_CTRL_SET_GLOBAL_OUT_NAK) && !g_bMute) g_Reg[DWHCI_CORE_INT_STAT] |= DWHCI_CORE_INT_STAT_GLOBAL_OUT_NAK_EFF;
		if (v & DWHCI_DEV_CTRL_CLEAR_GLOBAL_OUT_NAK) g_Reg[DWHCI_CORE_INT_STAT] &= ~DWHCI_CORE_INT_STAT_GLOBAL_OUT_NAK_EFF;
		g_Reg[a] = v & ~(DWHCI_DEV_CTRL_SET_GLOBAL_OUT_NAK | DWHCI_DEV_CTRL_CLEAR_GLOBAL_OUT_NAK);
		return;
	}
	if (a == DWHCI_CORE_RESET)
	{
		if (v & DWHCI_CORE_RESET_TX_FIFO_FLUSH) g_Flushes.push_back ((v & DWHCI_CORE_RESET_TX_FIFO_NUM__MASK) >> DWHCI_CORE_RESET_TX_FIFO_NUM__SHIFT);
		g_Reg[a] = v & ~DWHCI_CORE_RESET_TX_FIFO_FLUSH;
		return;
	}
	if (a == DWHCI_CORE_INT_STAT) { g_Reg[a] &= ~v; return; }	// write-1-to-clear, as the endpoints' interrupts
	g_Reg[a] = v;
}

static int g_nPanics;
CLogger *CLogger::Get (void) { static CLogger l; return &l; }
void CLogger::Write (const char *pSource, TLogSeverity Severity, const char *pMessage, ...)
{
	char b[256];
	va_list a;
	va_start (a, pMessage);
	vsnprintf (b, sizeof b, pMessage, a);
	va_end (a);
	lines.push_back (std::string (pSource) + ": " + b);
	if (Severity == LogPanic) g_nPanics++;
}

// ---- the host
static CDWUSBGadget g_Gadget;
static CDWUSBGadgetEndpoint0 *g_pEP0;
static const u32 Ena = DWHCI_DEV_EP_CTRL_EP_ENABLE;

static void OutIrq (unsigned ep) { g_Gadget.OutIrq (ep); }
static void InIrq (unsigned ep) { g_Gadget.InIrq (ep); }

// a SETUP: ACKed whatever EP0 OUT's state, written where DOEPDMA0 points, the transfer size and the SETUP count down,
// SETUP done raised; its IRQ handled at once, or (bIrq false) still to come
static void Setup (u8 uchType, u8 uchRequest, u16 wValue, u16 wIndex, u16 wLength, bool bIrq = true)
{
	u8 s[8] = {uchType, uchRequest, (u8) wValue, (u8) (wValue >> 8), (u8) wIndex, (u8) (wIndex >> 8), (u8) wLength,
		   (u8) (wLength >> 8)};
	u32 &d = g_Reg[DWHCI_DEV_OUT_EP_DMA_ADDR (0)];
	CHECK (d >= kBusBase);
	memcpy (Ptr (d), s, 8);
	d += 8;
	u32 &t = g_Reg[DWHCI_DEV_OUT_EP_XFER_SIZ (0)];
	t = (t & ~0x7Fu) | ((t & 0x7F) >= 8 ? (t & 0x7F) - 8 : 0);
	unsigned nSup = (t >> 29) & 3;
	t = (t & ~(3u << 29)) | (nSup ? nSup - 1 : 0) << 29;
	g_Reg[DWHCI_DEV_OUT_EP_CTRL (0)] &= ~(Ena | DWHCI_DEV_EP_CTRL_STALL);
	g_Reg[DWHCI_DEV_IN_EP_CTRL (0)] &= ~DWHCI_DEV_EP_CTRL_STALL;
	g_Reg[DWHCI_DEV_OUT_EP_INT (0)] |= DWHCI_DEV_OUT_EP_INT_SETUP_DONE;
	if (bIrq) OutIrq (0);
}

enum { NAK = -1, STALL = -2 };
// one IN packet of EP ep (up to 64 bytes): its bytes, or NAK / STALL
static int In (unsigned ep = 0, std::string *pData = nullptr)
{
	u32 &c = g_Reg[DWHCI_DEV_IN_EP_CTRL (ep)];
	if (c & DWHCI_DEV_EP_CTRL_STALL) return STALL;
	if (!(c & Ena))
	{
		if (g_Reg[DWHCI_DEV_IN_EP_COMMON_INT_MASK] & DWHCI_DEV_IN_EP_INT_NAK)	// an isochronous EP waits for this
		{
			g_Reg[DWHCI_DEV_IN_EP_INT (ep)] |= DWHCI_DEV_IN_EP_INT_NAK;
			InIrq (ep);
		}
		return NAK;
	}
	u32 &t = g_Reg[DWHCI_DEV_IN_EP_XFER_SIZ (ep)];
	unsigned nLeft = t & 0x7FFFF, n = nLeft > 64 ? 64 : nLeft;
	if (pData) pData->assign ((const char *) Ptr (g_Reg[DWHCI_DEV_IN_EP_DMA_ADDR (ep)]), n);
	t = (t & ~0x7FFFFu) | (nLeft - n);
	g_Reg[DWHCI_DEV_IN_EP_DMA_ADDR (ep)] += n;
	if (nLeft == n)
	{
		c &= ~Ena;
		g_Reg[DWHCI_DEV_IN_EP_INT (ep)] |= DWHCI_DEV_IN_EP_INT_XFER_COMPLETE;
		InIrq (ep);
	}
	return (int) n;
}
// one OUT packet to EP ep: ACKed (true) or NAKed
static bool Out (unsigned ep, const void *pData, unsigned n)
{
	u32 &c = g_Reg[DWHCI_DEV_OUT_EP_CTRL (ep)];
	if (!(c & Ena))				// NAKed; an isochronous EP waiting for the host's token takes it
	{
		if (ep != 0)
		{
			g_Reg[DWHCI_DEV_OUT_EP_INT (ep)] |= DWHCI_DEV_OUT_EP_INT_OUT_TOKEN_EP_DISABLED;
			OutIrq (ep);
		}
		return false;
	}
	u32 &t = g_Reg[DWHCI_DEV_OUT_EP_XFER_SIZ (ep)];
	CHECK (n <= (t & 0x7FFFF));
	u32 &d = g_Reg[DWHCI_DEV_OUT_EP_DMA_ADDR (ep)];
	if (n) memcpy (Ptr (d), pData, n);
	d += n;
	t = (t & ~0x7FFFFu) | ((t & 0x7FFFF) - n);
	c &= ~Ena;
	g_Reg[DWHCI_DEV_OUT_EP_INT (ep)] |= DWHCI_DEV_OUT_EP_INT_XFER_COMPLETE;
	OutIrq (ep);
	return true;
}
static bool Out (const void *pData, unsigned n) { return Out (0, pData, n); }

static const u8 VendorIn = REQUEST_IN | REQUEST_VENDOR | REQUEST_TO_DEVICE, VendorOut = REQUEST_OUT | REQUEST_VENDOR;

// the box's answer to a deferred IN request: the host gets it, then the status stage
static void Answer (const char *pAnswer)
{
	CHECK (g_pEP0->CompleteDeferred (pAnswer, (int) strlen (pAnswer)));
	std::string d;
	CHECK (In (0, &d) == (int) strlen (pAnswer) && d == pAnswer);
	CHECK (Out (nullptr, 0));		// the status stage
}

static const TUSBEndpointDescriptor DescBulkIn = {7, DESCRIPTOR_ENDPOINT, 0x81, 2, 64, 0};
static const TUSBEndpointDescriptor DescBulkOut = {7, DESCRIPTOR_ENDPOINT, 0x02, 2, 64, 0};	// (the shim keeps one EP a number)
static const TUSBEndpointDescriptor DescUnplugIn = {7, DESCRIPTOR_ENDPOINT, 0x86, 2, 64, 0};
static const TUSBEndpointDescriptor DescIsoIn = {7, DESCRIPTOR_ENDPOINT, 0x83, 5, 64, 1};
// usb=mirror: one of the box's endpoints on the computer's side with CMirrorEP's shape (OUT: armed at activation and
// after each packet) and its suspend / resume / cancel (mirror_ep.h, the code CMirrorEP runs); bRc6: rc6's instead
// (OnSuspend = OnDeactivate with CancelTransfer, no OnResume)
static const TUSBEndpointDescriptor DescIsoOut = {7, DESCRIPTOR_ENDPOINT, 0x04, 5, 64, 1};
struct CMirrorTestEP : CDWUSBGadgetEndpoint
{
	CMirrorTestEP (bool b) : CDWUSBGadgetEndpoint (&DescIsoOut, &g_Gadget), bRc6 (b) {}
	void OnActivate (void) override { w.activated (); active = true; Rearm (); }
	void OnDeactivate (void) override { active = false; if (bRc6) CancelTransfer (); else mirror::cancel (*this); }
	void OnSuspend (void) override { if (bRc6) OnDeactivate (); else w.suspend (*this); }
	void OnResume (void) override { if (!bRc6) w.wake (*this); }
	void OnTransferComplete (boolean, size_t) override { ++done; Rearm (); }
	void Rearm (void) { if (active) BeginTransfer (TransferDataOut, m_Buf, 64); }
	boolean Active (void) const { return active; }
	boolean Iso (void) const { return GetType () == TypeIsochronous; }
	void StopIso (void) { StopIsoTransfer (); }
	void Cancel (void) { CancelTransfer (); }
	mirror::EpWake w;
	bool bRc6, active = false;
	int done = 0;
	DMA_BUFFER (u8, m_Buf, 64);
};

// CMirrorEP's pipe OUT (mirror_ep.h OutGate: armed when active, nothing armed, the box owns no packet) and
// CSidecarMIDIEP's OUT (re-armed after each packet), each with mirror_ep.h EpWake across a suspend
static const TUSBEndpointDescriptor DescPipeOut = {7, DESCRIPTOR_ENDPOINT, 0x05, 2, 64, 0};
static const TUSBEndpointDescriptor DescMidiOut = {7, DESCRIPTOR_ENDPOINT, 0x06, 2, 64, 0};
struct CPipeOutEP : CDWUSBGadgetEndpoint
{
	CPipeOutEP (const TUSBEndpointDescriptor *p, bool bBox) : CDWUSBGadgetEndpoint (p, &g_Gadget), bBoxPipe (bBox) {}
	void OnActivate (void) override { w.activated (); active = true; if (bBoxPipe) out.arm (*this); else ArmOut (); }
	// (CSidecarMIDIEP stops an armed transfer as ep_stop.h does since circle-gadget-unplug; the pipe as before)
	void OnDeactivate (void) override
	{
		active = false;
		if (!bBoxPipe && IsTransferActive ()) ep_stop::stop (*this, GetEPNumber (), false);
		else CancelTransfer ();
	}
	void Cancel (void) { CancelTransfer (); }
	void OnSuspend (void) override { w.suspend (*this); }
	void OnResume (void) override { w.wake (*this); }
	void OnTransferComplete (boolean, size_t) override { ++done; if (bBoxPipe) out.taken (); else if (active) ArmOut (); }
	void BoxDone (void) { out.done (*this); }		// MirrorPipeOutDone
	boolean Active (void) const { return active; }
	boolean Armed (void) const { return IsTransferActive (); }
	void ArmOut (void) { BeginTransfer (TransferDataOut, m_Buf, 64); }
	mirror::EpWake w;
	mirror::OutGate out;
	bool bBoxPipe, active = false;
	int done = 0;
	DMA_BUFFER (u8, m_Buf, 64);
};
// the bus suspend as the gadget handles it (dwusbgadget.cpp HandleUSBSuspend, circle-gadget-wake.patch): each EP's
// OnSuspend, and its interrupts acknowledged unprocessed while suspended
static void Suspend (CDWUSBGadgetEndpoint *p, unsigned ep)
{
	p->OnSuspend ();
	g_Reg[DWHCI_DEV_OUT_EP_INT (ep)] = 0, g_Reg[DWHCI_DEV_IN_EP_INT (ep)] = 0;
}


// the USB-C network's data endpoints with CNcmEP's shape (sidecar_gadget.cpp): nothing armed at SET_CONFIGURATION, OUT
// armed at alt 1 and again after each block (its datagrams parsed), the link's stop at alt 0 and at a bus suspend.
// bOld: the stop of bf90503 / 53e1469 (CancelTransfer alone); else ep_stop.h's
static const TUSBEndpointDescriptor DescNcmOut = {7, DESCRIPTOR_ENDPOINT, 0x07, 2, 512, 0};
static const TUSBEndpointDescriptor DescNcmIn = {7, DESCRIPTOR_ENDPOINT, 0x81, 2, 512, 0};
struct CNcmTestEP : CDWUSBGadgetEndpoint
{
	CNcmTestEP (const TUSBEndpointDescriptor *p, bool b) : CDWUSBGadgetEndpoint (p, &g_Gadget), bOld (b), nEP (p->bEndpointAddress & 0xF),
		bOutRole (!(p->bEndpointAddress & 0x80)), m_Role ((p->bEndpointAddress & 0x80) ? RoleIn : RoleOut) {}
	void OnActivate (void) override { w.activated (); active = true; busy = false; if (bResuming && bOutRole) ArmOut (); }
	void OnDeactivate (void) override { active = false; busy = false; Stop (); }
	void OnSuspend (void) override { w.suspend (*this); }
	void OnResume (void) override { bResuming = true; w.wake (*this); bResuming = false; }
	void OnTransferComplete (boolean, size_t n) override
	{
		busy = false;
		if (!bOutRole) { ++sent; return; }
		ncm::parse16 (m_Buf, n, ncm::kOutMaxDatagrams, [this] (const u8 *, unsigned) { ++frames; });
		++blocks;
		ArmOut ();
	}
	void ArmOut (void) { if (!active || busy || !up) return; busy = true; BeginTransfer (TransferDataOut, m_Buf, sizeof m_Buf); }
	void Send (unsigned n) { if (!active || busy || !up) return; busy = true; BeginTransfer (TransferDataIn, m_Buf, n); }
	void Stop (void) { if (!IsTransferActive ()) return; if (bOld) CancelTransfer (); else ep_stop::stop (*this, nEP, !bOutRole); }
	void Halt (void) { Stop (); busy = false; }
	void Cancel (void) { CancelTransfer (); }
	boolean Armed (void) const { return IsTransferActive (); }
	// CNcmEP::OnTransferDropped itself (circle-gadget-unplug.patch): the Makefile extracts the body from sidecar_gadget.cpp
	// (build/drop_ncm.inc); the members it uses are named as the class's
	enum TRole { RoleNotify, RoleIn, RoleOut };
	void OnTransferDropped (boolean bIn) override
	{
		++dropped;
#include "build/drop_ncm.inc"
	}
	void Kick (void) {}					// (IN: nothing queued here)
	boolean Active (void) const { return active; }
	mirror::EpWake w;
	int dropped = 0;
	bool bOld, up = false, bResuming = false;
	boolean active = FALSE, busy = FALSE, held = FALSE;
	boolean &m_bActive = active, &m_bBusy = busy, &m_bHeld = held;
	unsigned nEP;
	bool bOutRole;
	TRole m_Role;
	int frames = 0, blocks = 0, sent = 0;
	DMA_BUFFER (u8, m_Buf, 2048);
};
// SET_INTERFACE on the data interface as NetSetAlt does it: both stopped, the link's state, OUT armed at alt 1
static void NcmSetAlt (CNcmTestEP *pOut, CNcmTestEP *pIn, u8 uchAlt)
{
	pIn->Halt (), pOut->Halt ();
	pIn->up = pOut->up = uchAlt != 0;
	if (uchAlt) pOut->ArmOut ();
}
// an NTB16 with one datagram of nFrame bytes
static unsigned NcmBlock (u8 *p, unsigned nFrame)
{
	memset (p, 0, 12 + 16 + nFrame + 4);
	ncm::wr32 (p, ncm::kNth16Sig), ncm::wr16 (p + 4, 12);
	const unsigned nNdp = (12 + nFrame + 3) & ~3u, nBlock = nNdp + 16;
	ncm::wr16 (p + 8, (u16) nBlock), ncm::wr16 (p + 10, (u16) nNdp);
	memset (p + 12, 0xAB, nFrame);
	ncm::wr32 (p + nNdp, ncm::kNdp16Sig), ncm::wr16 (p + nNdp + 4, 16);
	ncm::wr16 (p + nNdp + 8, 12), ncm::wr16 (p + nNdp + 10, (u16) nFrame);
	return nBlock;
}

struct CTestEP : CDWUSBGadgetEndpoint
{
	CTestEP (const TUSBEndpointDescriptor *p) : CDWUSBGadgetEndpoint (p, &g_Gadget) {}
	void OnActivate (void) override {}
	void OnDeactivate (void) override {}
	void OnTransferComplete (boolean, size_t n) override { ++done; last = n; }
	void Send (size_t n) { BeginTransfer (TransferDataIn, m_Buf, n); }
	void Receive (void) { BeginTransfer (TransferDataOut, m_Buf, 64); }
	void Cancel (void) { CancelTransfer (); }
	bool Armed (void) const { return IsTransferActive (); }
	int done = 0;
	size_t last = 0;
	DMA_BUFFER (u8, m_Buf, 64);
};

// CSidecarMIDIEP's shape (sidecar_gadget.cpp), both directions: OUT armed at activation and after each packet, IN while
// packets wait; Stop is the same statement as CSidecarMIDIEP::Stop (the Makefile checks the source still has it), so the
// endpoint number and direction come from the descriptor as there. OnTransferDropped as circle-gadget-unplug.patch wants it
static const TUSBEndpointDescriptor DescMidiOut4 = {7, DESCRIPTOR_ENDPOINT, 0x04, 2, 512, 0};	// the Pi 5 panic's EP4
static const TUSBEndpointDescriptor DescMidiIn3 = {7, DESCRIPTOR_ENDPOINT, 0x83, 2, 512, 0};
struct CMidiTestEP : CDWUSBGadgetEndpoint
{
	CMidiTestEP (const TUSBEndpointDescriptor *p) : CDWUSBGadgetEndpoint (p, &g_Gadget) {}
	void OnActivate (void) override
	{
		w.activated (); active = true; busy = false; held = false;
		if (GetDirection () == DirectionOut) BeginTransfer (TransferDataOut, m_Buf, 512);
		else Kick ();
	}
	void OnDeactivate (void) override { active = false; busy = false; held = false; Stop (); }
	void OnSuspend (void) override { w.suspend (*this); }
	void OnResume (void) override { w.wake (*this); }
	void Stop (void)
	{
		if (!IsTransferActive ()) return;
		ep_stop::stop (*this, GetEPNumber (), GetDirection () == DirectionIn);
	}
	void Kick (void)
	{
		if (!active || busy || GetDirection () != DirectionIn || !queued) return;
		queued--; busy = true;
		memcpy (m_Buf, "midi", 4);
		BeginTransfer (TransferDataIn, m_Buf, 4);
	}
	void OnTransferComplete (boolean bIn, size_t) override
	{
		if (!bIn) { ++done; if (active) BeginTransfer (TransferDataOut, m_Buf, 512); return; }
		busy = false; ++sent; Kick ();
	}
	// CSidecarMIDIEP::OnTransferDropped itself (circle-gadget-unplug.patch): the Makefile extracts the body from
	// sidecar_gadget.cpp (build/drop_midi.inc); the members it uses are named as the class's
	void OnTransferDropped (boolean bIn) override
	{
		++dropped;
#include "build/drop_midi.inc"
	}
	static constexpr size_t BufSize = 512;
	u8 *m_Buffer = m_Buf;
	boolean Active (void) const { return active; }
	boolean Armed (void) const { return IsTransferActive (); }
	void Cancel (void) { CancelTransfer (); }	// ep_stop.h
	mirror::EpWake w;
	boolean active = FALSE, busy = FALSE, held = FALSE;
	boolean &m_bActive = active, &m_bBusy = busy, &m_bHeld = held;
	int done = 0, sent = 0, dropped = 0, queued = 0;
	DMA_BUFFER (u8, m_Buf, 512);
};

// CMirrorEP's shape (sidecar_gadget.cpp), all four roles of the box's endpoints on the computer's side. Its
// OnTransferDropped is the real body (the Makefile extracts it into build/drop_mirror.inc); the members it uses are named
// as the class's. (The real audio endpoints are isochronous and never see a drop; bulk descriptors here so every role's
// branch runs.)
static const TUSBEndpointDescriptor DescMirAudioIn = {7, DESCRIPTOR_ENDPOINT, 0x81, 2, 64, 0};
static const TUSBEndpointDescriptor DescMirPipeIn = {7, DESCRIPTOR_ENDPOINT, 0x83, 2, 64, 0};
static const TUSBEndpointDescriptor DescMirAudioOut = {7, DESCRIPTOR_ENDPOINT, 0x04, 2, 64, 0};
static const TUSBEndpointDescriptor DescMirPipeOut = {7, DESCRIPTOR_ENDPOINT, 0x05, 2, 64, 0};
struct CMirrorSidecarStub
{
	int nPipeInSent = 0;
	void MirrorPipeInSent (u8) { nPipeInSent++; }	// the box's URB goes on
};
struct CMirrorBoxEP : CDWUSBGadgetEndpoint
{
	CMirrorBoxEP (const TUSBEndpointDescriptor *p, mirror::Role r)
	:	CDWUSBGadgetEndpoint (p, &g_Gadget), m_pSidecar (&Stub), m_Role (r), m_uchAddr (p->bEndpointAddress) {}
	void OnActivate (void) override
	{
		m_Wake.activated (); m_bActive = TRUE; m_bBusy = FALSE;
		if (m_Role == mirror::kAudioOut || m_Role == mirror::kPipeOut) m_Out.arm (*this);
		else Kick ();
	}
	void OnDeactivate (void) override { m_bActive = FALSE; m_bBusy = FALSE; Cancel (); }
	void OnTransferComplete (boolean, size_t) override
	{
		++done;
		switch (m_Role)
		{
		case mirror::kAudioIn: m_bBusy = FALSE; Kick (); break;
		case mirror::kAudioOut: Rearm (); break;
		case mirror::kPipeOut: m_Out.taken (); break;		// NAKed until the box took it
		case mirror::kPipeIn: m_bBusy = FALSE; m_pSidecar->MirrorPipeInSent (m_uchAddr); break;
		default: break;
		}
	}
	void OnTransferDropped (boolean bIn) override
	{
		++dropped;
#include "build/drop_mirror.inc"
	}
	void Rearm (void) { m_Out.done (*this); }
	void ArmOut (void) { BeginTransfer (TransferDataOut, m_Buffer, 64); }
	void Kick (void)
	{
		if (!m_bActive || m_bBusy || m_Role != mirror::kAudioIn || queued == 0) return;
		queued--; m_bBusy = TRUE;
		BeginTransfer (TransferDataIn, m_Buffer, 4);
	}
	void Send (unsigned n) { m_bBusy = TRUE; BeginTransfer (TransferDataIn, m_Buffer, n); }	// pipe IN: the box's URB
	void Cancel (void) { CancelTransfer (); }
	void BoxDone (void) { m_Out.done (*this); }	// MirrorPipeOutDone
	boolean Active (void) const { return m_bActive; }
	boolean Armed (void) const { return IsTransferActive (); }
	CMirrorSidecarStub Stub;
	CMirrorSidecarStub *m_pSidecar;
	mirror::EpWake m_Wake;
	mirror::OutGate m_Out;
	mirror::Role m_Role;
	u8 m_uchAddr;
	boolean m_bActive = FALSE, m_bBusy = FALSE;
	int done = 0, dropped = 0, queued = 0;
	DMA_BUFFER (u8, m_Buffer, 64);
};

int main (void)
{
	g_Reg[DWHCI_DEV_IN_EP_COMMON_INT_MASK] = ~0u & ~DWHCI_DEV_IN_EP_INT_NAK;
	g_Reg[DWHCI_DEV_OUT_EP_COMMON_INT_MASK] = ~0u;
	g_pEP0 = new (Alloc (sizeof (CDWUSBGadgetEndpoint0))) CDWUSBGadgetEndpoint0 (64, &g_Gadget);
	g_pEP0->OnUSBReset ();
	g_pEP0->OnActivate ();
	CHECK (g_Reg[DWHCI_DEV_OUT_EP_CTRL (0)] & Ena);		// SETUP armed

	{	// a standard request (GET_DESCRIPTOR, IN): data, status, the next SETUP
		Setup (REQUEST_IN, GET_DESCRIPTOR, DESCRIPTOR_DEVICE << 8, 0, 18);
		std::string d;
		CHECK (In (0, &d) == 18 && (u8) d[0] == 18);
		CHECK (Out (nullptr, 0));
		CHECK (In () == NAK && (g_Reg[DWHCI_DEV_OUT_EP_CTRL (0)] & Ena));
	}
	{	// CLASS COMPLIANT: SET_INTERFACE reaches the gadget, its status stage goes out
		int n = g_Gadget.set_interface;
		Setup (REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE, 1, 2, 0);
		CHECK (g_Gadget.set_interface == n + 1 && In () == 0);
		// a class OUT request with data (the sample rate, SET CUR): data stage, the gadget takes it at the status stage
		g_Gadget.defer = false;
		const u8 Rate[4] = {0x80, 0xBB, 0, 0};
		int c = g_Gadget.class_requests;
		Setup (REQUEST_OUT | REQUEST_CLASS | REQUEST_TO_INTERFACE, 1, 0x0100, 0x1000, 4);
		CHECK (Out (Rate, 4));
		CHECK (In () == 0 && g_Gadget.class_requests == c + 1);
		g_Gadget.defer = true;
	}
	{	// a deferred vendor IN request: NAKed until the box's answer, then data and status
		size_t r = g_Gadget.requests.size ();
		Setup (VendorIn, 1, 0, 0, 16);
		CHECK (g_Gadget.requests.size () == r + 1 && In () == NAK);
		Answer ("hello");
		CHECK (g_Gadget.aborted == 0);
	}
	{	// the host gives up on a deferred request and sends the next SETUP (the 2026-11-06 crash): read back from where
		// the core wrote it, it ends the deferred request and is taken as any; the late answer is the gadget's to drop
		unsigned s = CDWUSBGadgetEndpoint::GetStrayCount ();
		Setup (VendorIn, 2, 0, 0, 16);
		Setup (VendorIn, 3, 0, 0, 16);
		CHECK (g_Gadget.aborted == 1 && CDWUSBGadgetEndpoint0::GetSetupAbortCount () == 1);
		CHECK (CDWUSBGadgetEndpoint::GetStrayCount () == s + 1);
		CHECK (g_Gadget.requests.back ().bRequest == 3);
		Answer ("three");
	}
	{	// ... and the next one is host-to-device with data: its data stage, then deferred, then the status stage
		Setup (VendorIn, 4, 0, 0, 16);
		Setup (VendorOut, 0x45, 0, 0, 16);
		CHECK (g_Gadget.aborted == 2);
		const char Data[17] = "0123456789abcdef";
		CHECK (Out (Data, 16));
		CHECK (g_Gadget.requests.back ().bRequest == 0x45);
		CHECK (g_Gadget.out_data.size () == 16 && memcmp (g_Gadget.out_data.data (), Data, 16) == 0);
		CHECK (In () == NAK);			// status NAKed until the box took it
		// the host gives up on that one too: the SETUP lands just past the data stage
		Setup (VendorIn, 6, 0, 0, 16);
		CHECK (g_Gadget.aborted == 3 && g_Gadget.requests.back ().bRequest == 6);
		Answer ("six");
	}
	{	// ... and the next one is a standard one without data (SET_INTERFACE): answered at once
		Setup (VendorIn, 7, 0, 0, 16);
		int n = g_Gadget.set_interface;
		Setup (REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE, 1, 3, 0);
		CHECK (g_Gadget.aborted == 4 && g_Gadget.set_interface == n + 1);
		CHECK (In () == 0);
	}
	{	// the box's answer comes as the host's next SETUP is in but its IRQ not handled yet: not armed (the new
		// request's first IN token would get the old answer); the SETUP's IRQ then ends the old one
		Setup (VendorIn, 8, 0, 0, 16);
		Setup (VendorIn, 9, 0, 0, 16, false);
		CHECK (g_pEP0->CompleteDeferred ("stale", 5));
		CHECK (In () == NAK);
		OutIrq (0);
		CHECK (g_Gadget.aborted == 5 && g_Gadget.requests.back ().bRequest == 9 && In () == NAK);
		Answer ("fresh");
	}
	{	// the box STALLs: so does EP0, until the next SETUP
		Setup (VendorIn, 10, 0, 0, 16);
		CHECK (g_pEP0->CompleteDeferred (nullptr, -1));
		CHECK (In () == STALL);
		Setup (VendorIn, 11, 0, 0, 16);
		Answer ("ok");
	}
	{	// an IN completion nothing waits for: ignored and counted, no assert
		unsigned n = CDWUSBGadgetEndpoint::GetStrayCount ();
		Setup (VendorIn, 12, 0, 0, 16);
		g_Reg[DWHCI_DEV_IN_EP_INT (0)] |= DWHCI_DEV_IN_EP_INT_XFER_COMPLETE;
		InIrq (0);
		CHECK (CDWUSBGadgetEndpoint::GetStrayCount () == n + 1);
		Answer ("fine");
	}
	{	// CLASS COMPLIANT data endpoints: a bulk IN, a bulk OUT and an isochronous IN transfer complete
		CTestEP *pIn = new (Alloc (sizeof (CTestEP))) CTestEP (&DescBulkIn);
		CTestEP *pOut = new (Alloc (sizeof (CTestEP))) CTestEP (&DescBulkOut);
		pIn->OnUSBReset (), pOut->OnUSBReset ();
		memcpy (pIn->m_Buf, "midi", 4);
		pIn->Send (4);
		pOut->Receive ();
		std::string d;
		CHECK (In (1, &d) == 4 && d == "midi" && pIn->done == 1);
		CHECK (Out (2, "note", 4) && pOut->done == 1 && pOut->last == 4 && memcmp (pOut->m_Buf, "note", 4) == 0);
		pIn->Send (4), pOut->Receive ();	// and again
		CHECK (In (1) == 4 && pIn->done == 2 && Out (2, "x", 1) && pOut->done == 2);
		CHECK (!(g_Reg[DWHCI_CORE_INT_STAT] & DWHCI_CORE_INT_STAT_GLOBAL_OUT_NAK_EFF));

		g_Reg[DWHCI_DEV_IN_EP_COMMON_INT_MASK] |= DWHCI_DEV_IN_EP_INT_NAK;	// the iso EP's token interrupt
		CTestEP *pIso = new (Alloc (sizeof (CTestEP))) CTestEP (&DescIsoIn);
		pIso->OnUSBReset ();
		pIso->Send (48);
		CHECK (In (3) == NAK);			// armed at the host's token
		CHECK (In (3) == 48 && pIso->done == 1);
		pIso->Send (48);			// the next one follows on
		CHECK (In (3) == 48 && pIso->done == 2);
	}
	{	// usb=mirror: the box's isochronous OUT across a bus suspend and a resume without a reset (macOS idles the
		// device some 10 s after the switch; rc6 on the Pi 4: the async channel and audio dead after it, "EP3: OUT
		// completion with no transfer armed")
		for (bool bRc6 : {true, false})
		{
			g_Reg[DWHCI_DEV_OUT_EP_CTRL (4)] = 0, g_Reg[DWHCI_DEV_OUT_EP_INT (4)] = 0;
			CMirrorTestEP *p = new (Alloc (sizeof (CMirrorTestEP))) CMirrorTestEP (bRc6);
			p->OnUSBReset ();
			p->OnActivate ();			// SET_CONFIGURATION
			CHECK (!Out (4, "a", 1));		// armed at the host's token
			CHECK (Out (4, "a", 1) && p->done == 1);
			CHECK (g_Reg[DWHCI_DEV_OUT_EP_CTRL (4)] & Ena);	// the next one armed
			unsigned s = CDWUSBGadgetEndpoint::GetStrayCount ();
			p->OnSuspend ();			// bus suspend
			if (bRc6)
			{
				// the cancel did not wait: the core finishes the transfer later, with nothing armed
				g_Reg[DWHCI_DEV_OUT_EP_INT (4)] |= DWHCI_DEV_OUT_EP_INT_XFER_COMPLETE;
				OutIrq (4);
				CHECK (CDWUSBGadgetEndpoint::GetStrayCount () == s + 1);
			}
			else
			{
				CHECK (!(g_Reg[DWHCI_DEV_OUT_EP_CTRL (4)] & Ena) && g_Reg[DWHCI_DEV_OUT_EP_INT (4)] == 0);
			}
			p->OnResume ();				// resume, no reset: no SET_CONFIGURATION follows
			bool bFlows = false;
			for (int i = 0; i < 3 && !bFlows; i++) bFlows = Out (4, "b", 1);
			if (bRc6) CHECK (!bFlows && p->done == 1);	// dead for good: what the Pi 4 showed
			else CHECK (bFlows && p->done == 2 && CDWUSBGadgetEndpoint::GetStrayCount () == s);
			g_Gadget.m_pEP[4] = nullptr;
		}
	}
	{	// the box's pipe OUT across a suspend and a resume while the box still has the computer's last packet: not
		// armed again until the box took it (then once: a second arm would assert in BeginTransfer)
		CPipeOutEP *p = new (Alloc (sizeof (CPipeOutEP))) CPipeOutEP (&DescPipeOut, true);
		p->OnUSBReset ();
		p->OnActivate ();
		CHECK (Out (5, "a", 1) && p->done == 1 && p->out.box);
		CHECK (!(g_Reg[DWHCI_DEV_OUT_EP_CTRL (5)] & Ena));	// NAKed: the box has it
		Suspend (p, 5);
		p->OnResume ();
		CHECK (p->active && !(g_Reg[DWHCI_DEV_OUT_EP_CTRL (5)] & Ena));	// back, still not armed
		p->BoxDone ();
		CHECK (g_Reg[DWHCI_DEV_OUT_EP_CTRL (5)] & Ena);
		CHECK (Out (5, "b", 1) && p->done == 2);
		p->BoxDone ();
		// the box takes it while the endpoint is suspended: armed at the resume
		CHECK (Out (5, "c", 1) && p->done == 3);
		Suspend (p, 5);
		p->BoxDone ();
		CHECK (!(g_Reg[DWHCI_DEV_OUT_EP_CTRL (5)] & Ena));
		p->OnResume ();
		CHECK (Out (5, "d", 1) && p->done == 4);
		// a reset after a suspend: nothing until SET_CONFIGURATION, then armed once (and a late OnResume adds nothing)
		p->BoxDone ();
		Suspend (p, 5);
		g_Reg[DWHCI_DEV_OUT_EP_CTRL (5)] = 0;	// the core's USB reset clears the EP's state
		p->OnUSBReset ();
		CHECK (!(g_Reg[DWHCI_DEV_OUT_EP_CTRL (5)] & Ena) && !p->active);
		p->OnActivate ();
		CHECK (g_Reg[DWHCI_DEV_OUT_EP_CTRL (5)] & Ena);
		p->OnResume ();				// (no wake follows a reset; were one late, it changes nothing)
		CHECK (Out (5, "e", 1) && p->done == 5);
		g_Gadget.m_pEP[5] = nullptr;
	}
	{	// the Sidecar's MIDI OUT (CSidecarMIDIEP) across a suspend and a resume without a reset: armed again
		CPipeOutEP *p = new (Alloc (sizeof (CPipeOutEP))) CPipeOutEP (&DescMidiOut, false);
		p->OnUSBReset ();
		p->OnActivate ();
		CHECK (Out (6, "m", 1) && p->done == 1);
		const int np = g_nPanics;
		p->OnSuspend ();				// the cancel waits for the core and acknowledges its "EP disabled"
		CHECK (g_Reg[DWHCI_DEV_OUT_EP_INT (6)] == 0 && !(g_Reg[DWHCI_DEV_OUT_EP_CTRL (6)] & Ena));
		OutIrq (6);					// (and had it been left, it is not fatal either)
		CHECK (g_nPanics == np);
		CHECK (!Out (6, "n", 1));			// suspended: not armed
		g_Reg[DWHCI_DEV_OUT_EP_INT (6)] = 0;
		p->OnResume ();
		CHECK (Out (6, "n", 1) && p->done == 2);
		g_Gadget.m_pEP[6] = nullptr;
	}
	{	// the USB-C network (CNcmEP, docs/usb-network.md): alt 1, the computer's blocks taken and the OUT armed again
		// each time; then the link's stop with the endpoints armed (alt 0, a bus suspend). The Pi 5 on 2026-11-07:
		// CancelTransfer alone left "EP disabled" to Circle's IRQ handler, which panics on it for a bulk endpoint
		for (bool bOld : {true, false})
		{
			g_Gadget.m_pEP[1] = g_Gadget.m_pEP[7] = nullptr;
			for (unsigned ep : {1u, 7u})
				g_Reg[DWHCI_DEV_OUT_EP_CTRL (ep)] = g_Reg[DWHCI_DEV_IN_EP_CTRL (ep)] = 0,
				g_Reg[DWHCI_DEV_OUT_EP_INT (ep)] = g_Reg[DWHCI_DEV_IN_EP_INT (ep)] = 0;
			CNcmTestEP *pOut = new (Alloc (sizeof (CNcmTestEP))) CNcmTestEP (&DescNcmOut, bOld);
			CNcmTestEP *pIn = new (Alloc (sizeof (CNcmTestEP))) CNcmTestEP (&DescNcmIn, bOld);
			pOut->OnUSBReset (), pIn->OnUSBReset ();
			pOut->OnActivate (), pIn->OnActivate ();	// SET_CONFIGURATION: the data interface at alt 0
			static u8 Ntb[256];
			const unsigned n = NcmBlock (Ntb, 60);
			CHECK (!Out (7, Ntb, n));			// alt 0: nothing armed, NAKed
			NcmSetAlt (pOut, pIn, 1);			// the computer's driver takes the link
			CHECK (Out (7, Ntb, n) && pOut->blocks == 1 && pOut->frames == 1);
			CHECK (g_Reg[DWHCI_DEV_OUT_EP_CTRL (7)] & Ena);	// armed again
			CHECK (Out (7, Ntb, n) && pOut->blocks == 2 && pOut->frames == 2);
			// alt 0 with OUT armed and an IN block the computer has not read: both stopped, the core's "EP
			// disabled" delivered as the IRQ would
			pIn->Send (100);
			int p = g_nPanics;
			NcmSetAlt (pOut, pIn, 0);
			OutIrq (7), InIrq (1);
			// (bOld: the Pi's panic "EP7 disabled", "EP1 disabled" before circle-gadget-unplug.patch; now none)
			CHECK (g_nPanics == p && g_Reg[DWHCI_DEV_OUT_EP_INT (7)] == 0 && g_Reg[DWHCI_DEV_IN_EP_INT (1)] == 0);
			CHECK (!(g_Reg[DWHCI_DEV_OUT_EP_CTRL (7)] & Ena) && !(g_Reg[DWHCI_DEV_IN_EP_CTRL (1)] & Ena));
			CHECK (!(g_Reg[DWHCI_CORE_INT_STAT] & DWHCI_CORE_INT_STAT_GLOBAL_OUT_NAK_EFF) || bOld);
			CHECK (!(g_Reg[DWHCI_DEV_CTRL] & DWHCI_DEV_CTRL_SET_GLOBAL_OUT_NAK));	// the other OUT endpoints run on
			g_nPanics = p;
			// up again: blocks flow
			NcmSetAlt (pOut, pIn, 1);
			CHECK (Out (7, Ntb, n) && pOut->blocks == 3);
			pIn->Send (100);
			CHECK (In (1) == 64 && In (1) == 36 && pIn->sent == 1);
			// a bus suspend with OUT armed, then a resume without a reset: armed again, no panic
			p = g_nPanics;
			pOut->OnSuspend (), pIn->OnSuspend ();
			OutIrq (7);
			CHECK (g_nPanics == p);
			g_nPanics = p;
			g_Reg[DWHCI_DEV_OUT_EP_INT (7)] = 0;
			pOut->OnResume (), pIn->OnResume ();
			CHECK (Out (7, Ntb, n) && pOut->blocks == 4 && pOut->frames == 4);
			g_Gadget.m_pEP[1] = g_Gadget.m_pEP[7] = nullptr;
		}
	}
	{	// circle-gadget-unplug.patch: the host vanishes (the USB-C cable pulled) with bulk transfers armed. The Pi 5 on
		// 2026-11-17: "EP4 disabled" (the MIDI OUT endpoint), LOGPANIC in HandleOutInterrupt, the Pi halted and rebooted
		for (unsigned ep : {5u, 6u})
			g_Reg[DWHCI_DEV_OUT_EP_CTRL (ep)] = g_Reg[DWHCI_DEV_IN_EP_CTRL (ep)] = 0,
			g_Reg[DWHCI_DEV_OUT_EP_INT (ep)] = g_Reg[DWHCI_DEV_IN_EP_INT (ep)] = 0;
		g_Gadget.m_pEP[5] = g_Gadget.m_pEP[6] = nullptr;
		CTestEP *pOut = new (Alloc (sizeof (CTestEP))) CTestEP (&DescPipeOut);
		CTestEP *pIn = new (Alloc (sizeof (CTestEP))) CTestEP (&DescUnplugIn);
		pOut->OnUSBReset (), pIn->OnUSBReset ();
		const int p = g_nPanics;
		const size_t nLines = CLogger::Get ()->lines.size ();
		// the core disables an armed OUT endpoint: acknowledged, the transfer forgotten, the endpoint armed again
		pOut->Receive ();
		g_Reg[DWHCI_DEV_OUT_EP_CTRL (5)] &= ~Ena;
		g_Reg[DWHCI_DEV_OUT_EP_INT (5)] |= DWHCI_DEV_OUT_EP_INT_EP_DISABLED;
		OutIrq (5);
		CHECK (g_nPanics == p && g_Reg[DWHCI_DEV_OUT_EP_INT (5)] == 0 && !pOut->Armed ());
		pOut->Receive ();
		CHECK (Out (5, "a", 1) && pOut->done == 1);
		// the same for an armed IN endpoint
		memcpy (pIn->m_Buf, "midi", 4);
		pIn->Send (4);
		g_Reg[DWHCI_DEV_IN_EP_CTRL (6)] &= ~Ena;
		g_Reg[DWHCI_DEV_IN_EP_INT (6)] |= DWHCI_DEV_IN_EP_INT_EP_DISABLED;
		InIrq (6);
		CHECK (g_nPanics == p && g_Reg[DWHCI_DEV_IN_EP_INT (6)] == 0 && !pIn->Armed ());
		pIn->Send (4);
		CHECK (In (6) == 4 && pIn->done == 1);
		// the late "disabled" of an earlier cancel arrives when the endpoint is armed again: the new transfer stays
		pOut->Receive ();
		pOut->Cancel ();
		pOut->Receive ();
		OutIrq (5);
		CHECK (g_nPanics == p && pOut->Armed () && Out (5, "b", 1) && pOut->done == 2);
		// an IN timeout (the host did not answer): acknowledged, the transfer stays armed
		pIn->Send (4);
		const unsigned nUnplug = CDWUSBGadgetEndpoint::GetUnplugCount (), nTimeout = CDWUSBGadgetEndpoint::GetTimeoutCount ();
		g_Reg[DWHCI_DEV_IN_EP_INT (6)] |= DWHCI_DEV_IN_EP_INT_TIMEOUT;
		InIrq (6);
		CHECK (g_nPanics == p && g_Reg[DWHCI_DEV_IN_EP_INT (6)] == 0 && pIn->Armed ());
		// counted on its own, not as an EP disabled; its WARN has its own backoff
		CHECK (CDWUSBGadgetEndpoint::GetTimeoutCount () == nTimeout + 1 && CDWUSBGadgetEndpoint::GetUnplugCount () == nUnplug);
		CHECK (CLogger::Get ()->lines.back ().find ("EP6: IN timeout") != std::string::npos);
		CHECK (In (6) == 4 && pIn->done == 2);
		// logged as warnings, not fatal
		CHECK (CLogger::Get ()->lines.size () > nLines);
		g_Gadget.m_pEP[5] = g_Gadget.m_pEP[6] = nullptr;
	}
	{	// circle-gadget-unplug.patch, review round. The stand-ins have the shape of CSidecarMIDIEP and CNcmEP.
		auto Clear = [] (std::initializer_list<unsigned> eps)
		{
			for (unsigned ep : eps)
			{
				g_Reg[DWHCI_DEV_OUT_EP_CTRL (ep)] = g_Reg[DWHCI_DEV_IN_EP_CTRL (ep)] = 0;
				g_Reg[DWHCI_DEV_OUT_EP_INT (ep)] = g_Reg[DWHCI_DEV_IN_EP_INT (ep)] = 0;
				g_Gadget.m_pEP[ep] = nullptr;
			}
		};
		Clear ({1, 3, 4, 7});
		const int p0 = g_nPanics;
		CMidiTestEP *pMOut = new (Alloc (sizeof (CMidiTestEP))) CMidiTestEP (&DescMidiOut4);
		CMidiTestEP *pMIn = new (Alloc (sizeof (CMidiTestEP))) CMidiTestEP (&DescMidiIn3);
		pMOut->OnUSBReset (), pMIn->OnUSBReset ();
		pMOut->OnActivate (), pMIn->OnActivate ();
		CHECK (g_Reg[DWHCI_DEV_OUT_EP_CTRL (4)] & Ena);

		{	// the lost completion (1): the transfer completes while the late "EP disabled" is handled (EPEna cleared by
			// the core, XFER_COMPLETE raised after the handler read its register). It is not dropped: the completion runs
			const unsigned nDrops = CDWUSBGadgetEndpoint::GetDropCount ();
			g_Reg[DWHCI_DEV_OUT_EP_INT (4)] |= DWHCI_DEV_OUT_EP_INT_EP_DISABLED;
			g_nRaceEP = 4, g_bRaceIn = false;
			OutIrq (4);
			CHECK (pMOut->Armed () && pMOut->dropped == 0 && pMOut->done == 0);
			CHECK (CDWUSBGadgetEndpoint::GetDropCount () == nDrops);
			CHECK (g_Reg[DWHCI_DEV_OUT_EP_INT (4)] & DWHCI_DEV_OUT_EP_INT_XFER_COMPLETE);	// still to come
			OutIrq (4);					// the level interrupt comes again
			CHECK (pMOut->done == 1 && pMOut->Armed () && (g_Reg[DWHCI_DEV_OUT_EP_CTRL (4)] & Ena));	// armed again
			CHECK (Out (4, "a", 1) && pMOut->done == 2);
			// the same on an IN endpoint
			pMIn->queued = 1;
			pMIn->Kick ();
			CHECK (pMIn->Armed ());
			g_Reg[DWHCI_DEV_IN_EP_INT (3)] |= DWHCI_DEV_IN_EP_INT_EP_DISABLED;
			g_nRaceEP = 3, g_bRaceIn = true;
			InIrq (3);
			CHECK (pMIn->Armed () && pMIn->dropped == 0 && pMIn->sent == 0 && pMIn->busy);
			InIrq (3);
			CHECK (pMIn->sent == 1 && !pMIn->busy && !pMIn->Armed ());
			CHECK (CDWUSBGadgetEndpoint::GetDropCount () == nDrops);
		}
		{	// a dropped armed transfer (2): the function driver arms the endpoint again, MIDI OUT and IN
			const unsigned nDrops = CDWUSBGadgetEndpoint::GetDropCount ();
			g_Reg[DWHCI_DEV_OUT_EP_CTRL (4)] &= ~Ena;		// the core disabled it, nothing completed
			g_Reg[DWHCI_DEV_OUT_EP_INT (4)] |= DWHCI_DEV_OUT_EP_INT_EP_DISABLED;
			OutIrq (4);
			CHECK (pMOut->dropped == 1 && CDWUSBGadgetEndpoint::GetDropCount () == nDrops + 1);
			CHECK (pMOut->Armed () && (g_Reg[DWHCI_DEV_OUT_EP_CTRL (4)] & Ena));
			CHECK (Out (4, "b", 1) && pMOut->done == 3);		// MIDI OUT flows again
			pMIn->queued = 2;					// IN: the first packet is lost, the second goes out
			pMIn->Kick ();
			g_Flushes.clear ();
			g_Reg[DWHCI_DEV_IN_EP_CTRL (3)] &= ~Ena;
			g_Reg[DWHCI_DEV_IN_EP_INT (3)] |= DWHCI_DEV_IN_EP_INT_EP_DISABLED;
			InIrq (3);
			CHECK (pMIn->dropped == 1 && pMIn->Armed () && pMIn->queued == 0);
			CHECK (g_Flushes.size () == 1 && g_Flushes[0] == 3);	// (the IN drop) the TX FIFO of EP 3 flushed, and only that
			std::string d;
			CHECK (In (3, &d) == 4 && d == "midi" && pMIn->sent == 2);
			CHECK (CDWUSBGadgetEndpoint::GetUnplugCount () >= 4);
		}
		{	// the WARN backoff is per endpoint: three EPs each log their first, an EP its second but not its third
			const size_t nLines = CLogger::Get ()->lines.size ();
			unsigned nEP4 = 0, nEP3 = 0;
			for (size_t i = 0; i < nLines; i++)
			{
				if (CLogger::Get ()->lines[i].find ("EP4: OUT disabled") != std::string::npos) nEP4++;
				if (CLogger::Get ()->lines[i].find ("EP3: IN disabled") != std::string::npos) nEP3++;
			}
			// EP4 has had 2 so far (the race's and the drop's), EP3 the same
			CHECK (nEP4 >= 1 && nEP3 >= 1);
			unsigned nLogged = 0;
			for (int i = 0; i < 6; i++)
			{
				const size_t n0 = CLogger::Get ()->lines.size ();
				g_Reg[DWHCI_DEV_OUT_EP_CTRL (4)] &= ~Ena;
				g_Reg[DWHCI_DEV_OUT_EP_INT (4)] |= DWHCI_DEV_OUT_EP_INT_EP_DISABLED;
				OutIrq (4);
				if (CLogger::Get ()->lines.size () > n0) nLogged++;
			}
			CHECK (nLogged == 2);				// EP4 had 2: the 3rd to 8th log at the 4th and the 8th only
			CHECK (pMOut->Armed ());
		}
		{	// the real Stop (direction and EP number from the descriptor) at a bus suspend: OUT EP4 and IN EP3 both stopped,
			// only EP3's TX FIFO flushed, the global OUT NAK released, no interrupt left, no panic; a resume arms them again
			pMIn->queued = 1;
			pMIn->Kick ();
			g_Flushes.clear ();
			g_Reg[DWHCI_DEV_STS] |= ep_stop::kSuspSts;
			pMOut->OnSuspend (), pMIn->OnSuspend ();
			CHECK (!(g_Reg[DWHCI_DEV_OUT_EP_CTRL (4)] & Ena) && !(g_Reg[DWHCI_DEV_IN_EP_CTRL (3)] & Ena));
			CHECK (!pMOut->Armed () && !pMIn->Armed ());
			CHECK (g_Reg[DWHCI_DEV_OUT_EP_INT (4)] == 0 && g_Reg[DWHCI_DEV_IN_EP_INT (3)] == 0);
			CHECK (g_Flushes.size () == 1 && g_Flushes[0] == 3);
			CHECK (!(g_Reg[DWHCI_DEV_CTRL] & DWHCI_DEV_CTRL_SET_GLOBAL_OUT_NAK));
			g_Reg[DWHCI_DEV_STS] &= ~ep_stop::kSuspSts;
			pMOut->OnResume (), pMIn->OnResume ();
			CHECK (Out (4, "c", 1) && pMOut->done == 4);
			CHECK (g_nPanics == p0);
		}
		{	// the stop at IRQ level while suspended (3): the core never answers (no GOUTNAKEff, no EPDisbld; the PHY gated).
			// The waits are cut short, the time spent is kept; with the bus up they are the full polls
			ep_stop::Stats St;
			ep_stop::take (&St);
			g_bMute = true;
			for (bool bSusp : {true, false})
			{
				if (bSusp) g_Reg[DWHCI_DEV_STS] |= ep_stop::kSuspSts;
				else g_Reg[DWHCI_DEV_STS] &= ~ep_stop::kSuspSts;
				pMOut->Cancel ();				// (nothing armed, the core's disable of it forgotten)
				g_Reg[DWHCI_DEV_OUT_EP_CTRL (4)] = g_Reg[DWHCI_DEV_OUT_EP_INT (4)] = 0;
				pMOut->active = true;
				pMOut->OnTransferComplete (FALSE, 0);		// armed
				CHECK (pMOut->Armed () && (g_Reg[DWHCI_DEV_OUT_EP_CTRL (4)] & Ena));
				g_nReads = 0;
				pMOut->OnDeactivate ();
				if (bSusp) CHECK (g_nReads < 300);
				else CHECK (g_nReads >= ep_stop::kPolls);
				g_Reg[DWHCI_CORE_INT_STAT] = 0, g_Reg[DWHCI_DEV_CTRL] = 0;
			}
			g_bMute = false;
			CHECK (ep_stop::take (&St) && St.nStops == 2 && St.nSuspStops == 1);
			CHECK (St.maxSuspUs > 0 && St.maxSuspUs < 300 && St.maxUs >= ep_stop::kPolls);	// logged by OnSuspend
			CHECK (!ep_stop::take (&St));
			pMOut->active = false;
		}
		Clear ({1, 3, 4, 7});
		{	// the USB-C network after a drop: OUT armed again (blocks flow), IN not busy for good
			CNcmTestEP *pOut = new (Alloc (sizeof (CNcmTestEP))) CNcmTestEP (&DescNcmOut, false);
			CNcmTestEP *pIn = new (Alloc (sizeof (CNcmTestEP))) CNcmTestEP (&DescNcmIn, false);
			pOut->OnUSBReset (), pIn->OnUSBReset ();
			pOut->OnActivate (), pIn->OnActivate ();
			static u8 Ntb[256];
			const unsigned n = NcmBlock (Ntb, 60);
			NcmSetAlt (pOut, pIn, 1);
			CHECK (Out (7, Ntb, n) && pOut->blocks == 1);
			g_Reg[DWHCI_DEV_OUT_EP_CTRL (7)] &= ~Ena;		// the core disables the armed OUT endpoint
			g_Reg[DWHCI_DEV_OUT_EP_INT (7)] |= DWHCI_DEV_OUT_EP_INT_EP_DISABLED;
			OutIrq (7);
			CHECK (pOut->dropped == 1 && pOut->busy && (g_Reg[DWHCI_DEV_OUT_EP_CTRL (7)] & Ena));
			CHECK (Out (7, Ntb, n) && pOut->blocks == 2);		// the network keeps receiving
			pIn->Send (100);
			CHECK (pIn->busy);
			g_Flushes.clear ();
			g_Reg[DWHCI_DEV_IN_EP_CTRL (1)] &= ~Ena;
			g_Reg[DWHCI_DEV_IN_EP_INT (1)] |= DWHCI_DEV_IN_EP_INT_EP_DISABLED;
			InIrq (1);
			CHECK (pIn->dropped == 1 && !pIn->busy && !pIn->Armed ());
			CHECK (g_Flushes.size () == 1 && g_Flushes[0] == 1);
			pIn->Send (100);					// not stuck busy: the next block goes
			CHECK (In (1) == 64 && In (1) == 36 && pIn->sent == 1);
			CHECK (g_nPanics == p0);
			Clear ({1, 7});
		}
		{	// usb=mirror (CMirrorEP::OnTransferDropped, the real body): each role after a drop. OUT roles armed again,
			// except while the box owns the packet (OutGate); the audio IN goes on with the next block, the pipe IN's URB
			// is handed back to the box
			Clear ({1, 3, 4, 5, 7});
			CMirrorBoxEP *pAIn = new (Alloc (sizeof (CMirrorBoxEP))) CMirrorBoxEP (&DescMirAudioIn, mirror::kAudioIn);
			CMirrorBoxEP *pPIn = new (Alloc (sizeof (CMirrorBoxEP))) CMirrorBoxEP (&DescMirPipeIn, mirror::kPipeIn);
			CMirrorBoxEP *pAOut = new (Alloc (sizeof (CMirrorBoxEP))) CMirrorBoxEP (&DescMirAudioOut, mirror::kAudioOut);
			CMirrorBoxEP *pPOut = new (Alloc (sizeof (CMirrorBoxEP))) CMirrorBoxEP (&DescMirPipeOut, mirror::kPipeOut);
			for (CMirrorBoxEP *e : {pAIn, pPIn, pAOut, pPOut}) e->OnUSBReset ();
			pAIn->queued = 2;
			for (CMirrorBoxEP *e : {pAIn, pPIn, pAOut, pPOut}) e->OnActivate ();
			pPIn->Send (4);
			CHECK (pAIn->Armed () && pPIn->Armed () && pAOut->Armed () && pPOut->Armed ());
			auto DropOut = [] (unsigned ep)
			{
				g_Reg[DWHCI_DEV_OUT_EP_CTRL (ep)] &= ~Ena;
				g_Reg[DWHCI_DEV_OUT_EP_INT (ep)] |= DWHCI_DEV_OUT_EP_INT_EP_DISABLED;
				OutIrq (ep);
			};
			auto DropIn = [] (unsigned ep)
			{
				g_Reg[DWHCI_DEV_IN_EP_CTRL (ep)] &= ~Ena;
				g_Reg[DWHCI_DEV_IN_EP_INT (ep)] |= DWHCI_DEV_IN_EP_INT_EP_DISABLED;
				InIrq (ep);
			};
			DropOut (4);						// audio OUT: armed again (Rearm)
			CHECK (pAOut->dropped == 1 && pAOut->Armed () && (g_Reg[DWHCI_DEV_OUT_EP_CTRL (4)] & Ena));
			CHECK (Out (4, "a", 1) && pAOut->done == 1 && pAOut->Armed ());
			DropOut (5);						// pipe OUT, the box owns nothing: armed again
			CHECK (pPOut->dropped == 1 && pPOut->Armed () && (g_Reg[DWHCI_DEV_OUT_EP_CTRL (5)] & Ena));
			CHECK (Out (5, "b", 1) && pPOut->done == 1 && !pPOut->Armed ());	// the box owns the packet: NAKed
			pPOut->BoxDone ();					// the box took it: the next one
			CHECK (pPOut->Armed ());
			pPOut->m_Out.taken ();					// the held OUT: the box owns a packet, a drop must not arm
			DropOut (5);
			CHECK (pPOut->dropped == 2 && !pPOut->Armed () && !(g_Reg[DWHCI_DEV_OUT_EP_CTRL (5)] & Ena));
			pPOut->BoxDone ();
			CHECK (pPOut->Armed () && Out (5, "c", 1) && pPOut->done == 2);
			g_Flushes.clear ();					// audio IN: the block is lost, the next one is sent
			DropIn (1);
			CHECK (pAIn->dropped == 1 && pAIn->Armed () && pAIn->queued == 0);
			CHECK (g_Flushes.size () == 1 && g_Flushes[0] == 1);
			CHECK (In (1) == 4 && pAIn->done == 1);
			DropIn (3);						// pipe IN: not busy, the box's URB goes on
			CHECK (pPIn->dropped == 1 && !pPIn->m_bBusy && !pPIn->Armed () && pPIn->Stub.nPipeInSent == 1);
			CHECK (g_nPanics == p0);
			Clear ({1, 3, 4, 5, 7});
		}
	}
	CHECK (g_nPanics == 0);
	puts ("test_gadget_ep0: ok");
	return 0;
}

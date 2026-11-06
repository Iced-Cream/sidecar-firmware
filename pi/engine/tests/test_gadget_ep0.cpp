// pi/engine/tests/test_gadget_ep0.cpp - the DWC2 gadget's EP0 and data endpoints (Circle's dwusbgadgetendpoint*.cpp with
// the Pi 4 patch stack up to pi/circle/circle-gadget-stray.patch) on a fake register file (shim_dwc/). The core is
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

u32 FakeRead (u32 a) { return a == DWHCI_CORE_RESET ? g_Reg[a] | DWHCI_CORE_RESET_AHB_IDLE : g_Reg[a]; }

void FakeWrite (u32 a, u32 v)
{
	bool bIn; unsigned ep;
	if (IsInt (a)) { g_Reg[a] &= ~v; return; }
	if (IsCtrl (a, &bIn, &ep))
	{
		const u32 Act = DWHCI_DEV_EP_CTRL_EP_DISABLE | DWHCI_DEV_EP_CTRL_SET_NAK | DWHCI_DEV_EP_CTRL_CLEAR_NAK
				| DWHCI_DEV_EP_CTRL_SETDPID_D0 | DWHCI_DEV_EP_CTRL_SETDPID_D1 | DWHCI_DEV_EP_CTRL_EP_ENABLE;
		u32 old = g_Reg[a];
		u32 now = (v & ~Act) | (old & DWHCI_DEV_EP_CTRL_EP_ENABLE) | (v & DWHCI_DEV_EP_CTRL_EP_ENABLE);
		if ((v & DWHCI_DEV_EP_CTRL_EP_DISABLE) && (old & DWHCI_DEV_EP_CTRL_EP_ENABLE) && !(ep == 0 && !bIn))
		{
			now &= ~DWHCI_DEV_EP_CTRL_EP_ENABLE;
			g_Reg[bIn ? DWHCI_DEV_IN_EP_INT (ep) : DWHCI_DEV_OUT_EP_INT (ep)] |= DWHCI_DEV_IN_EP_INT_EP_DISABLED;
		}
		g_Reg[a] = now;
		return;
	}
	if (a == DWHCI_DEV_CTRL)
	{
		if (v & DWHCI_DEV_CTRL_SET_GLOBAL_OUT_NAK) g_Reg[DWHCI_CORE_INT_STAT] |= DWHCI_CORE_INT_STAT_GLOBAL_OUT_NAK_EFF;
		if (v & DWHCI_DEV_CTRL_CLEAR_GLOBAL_OUT_NAK) g_Reg[DWHCI_CORE_INT_STAT] &= ~DWHCI_CORE_INT_STAT_GLOBAL_OUT_NAK_EFF;
		g_Reg[a] = v & ~(DWHCI_DEV_CTRL_SET_GLOBAL_OUT_NAK | DWHCI_DEV_CTRL_CLEAR_GLOBAL_OUT_NAK);
		return;
	}
	if (a == DWHCI_CORE_RESET) { g_Reg[a] = v & ~DWHCI_CORE_RESET_TX_FIFO_FLUSH; return; }
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
	if (!(c & Ena)) return false;
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
static const TUSBEndpointDescriptor DescIsoIn = {7, DESCRIPTOR_ENDPOINT, 0x83, 5, 64, 1};
struct CTestEP : CDWUSBGadgetEndpoint
{
	CTestEP (const TUSBEndpointDescriptor *p) : CDWUSBGadgetEndpoint (p, &g_Gadget) {}
	void OnActivate (void) override {}
	void OnDeactivate (void) override {}
	void OnTransferComplete (boolean, size_t n) override { ++done; last = n; }
	void Send (size_t n) { BeginTransfer (TransferDataIn, m_Buf, n); }
	void Receive (void) { BeginTransfer (TransferDataOut, m_Buf, 64); }
	int done = 0;
	size_t last = 0;
	DMA_BUFFER (u8, m_Buf, 64);
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
	CHECK (g_nPanics == 0);
	puts ("test_gadget_ep0: ok");
	return 0;
}

// pi/engine/tests/test_gadget_ep0.cpp - the DWC2 gadget's EP0 and endpoint cancel (Circle's dwusbgadgetendpoint*.cpp
// with the Pi 4 patch stack up to pi/circle/circle-gadget-stray.patch) on a fake register file (shim_dwc/). The core
// is modelled as far as EP0 needs: SETUPs land at DOEPDMA0 and raise SETUP done whether EP0 OUT is armed or not (the
// Pi 4 crash of 2026-11-06: a SETUP while a request waited for its deferred answer, nothing armed, FinishTransfer()
// asserted), IN / OUT packets move only while their EP is enabled, EPDis on an enabled EP raises EP disabled (not on
// EP0 OUT: the core ignores it there), interrupt bits are write-1-to-clear.
//
// The host's side: setup () sends a SETUP, in () reads one packet (or NAK, or STALL), out () sends one (or NAK).
#include <circle/usb/gadget/dwusbgadgetendpoint0.h>
#include <circle/usb/gadget/dwusbgadget.h>
#include <circle/usb/dwhciregister.h>
#include <circle/logger.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "check.h"

// ---- the fake core
static std::map<u32, u32> g_Reg;
static std::vector<uintptr> g_Bus;		// bus address handle - 1 -> pointer

u32 FakeBusAddress (uintptr p)
{
	for (size_t i = 0; i < g_Bus.size (); i++) if (g_Bus[i] == p) return (u32) (i + 1) << 8;
	g_Bus.push_back (p);
	return (u32) g_Bus.size () << 8;
}
static u8 *Ptr (u32 nBus) { return nBus ? (u8 *) g_Bus[(nBus >> 8) - 1] + (nBus & 0xFF) : nullptr; }

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
		if (bIn && (v & DWHCI_DEV_EP_CTRL_SET_NAK))	// the NAK takes effect at once here
		{
			g_Reg[DWHCI_DEV_IN_EP_INT (ep)] |= DWHCI_DEV_IN_EP_INT_IN_EP_NAK_EFF;
		}
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

// a SETUP: ACKed whatever EP0 OUT's state, written where DOEPDMA0 points (stale or not), the SETUP count down by one,
// SETUP done raised; its IRQ handled at once, or (bIrq false) still to come
static void Setup (u8 uchType, u8 uchRequest, u16 wValue, u16 wIndex, u16 wLength, bool bIrq = true)
{
	u8 s[8] = {uchType, uchRequest, (u8) wValue, (u8) (wValue >> 8), (u8) wIndex, (u8) (wIndex >> 8), (u8) wLength,
		   (u8) (wLength >> 8)};
	u8 *p = Ptr (g_Reg[DWHCI_DEV_OUT_EP_DMA_ADDR (0)]);
	CHECK (p != nullptr);
	memcpy (p, s, 8);
	g_Reg[DWHCI_DEV_OUT_EP_DMA_ADDR (0)] += 8;
	u32 &t = g_Reg[DWHCI_DEV_OUT_EP_XFER_SIZ (0)];
	t = (t & ~0x7Fu) | ((t & 0x7F) >= 8 ? (t & 0x7F) - 8 : 0);	// the transfer size counts it
	unsigned nSup = (t >> 29) & 3;
	t = (t & ~(3u << 29)) | (nSup ? nSup - 1 : 0) << 29;		// and the SETUP count
	g_Reg[DWHCI_DEV_OUT_EP_CTRL (0)] &= ~(Ena | DWHCI_DEV_EP_CTRL_STALL);
	g_Reg[DWHCI_DEV_IN_EP_CTRL (0)] &= ~DWHCI_DEV_EP_CTRL_STALL;
	g_Reg[DWHCI_DEV_OUT_EP_INT (0)] |= DWHCI_DEV_OUT_EP_INT_SETUP_DONE;
	if (bIrq) OutIrq (0);
}

enum { NAK = -1, STALL = -2 };
// one IN packet of EP0: its bytes, or NAK / STALL
static int In (std::string *pData = nullptr)
{
	u32 &c = g_Reg[DWHCI_DEV_IN_EP_CTRL (0)];
	if (c & DWHCI_DEV_EP_CTRL_STALL) return STALL;
	if (!(c & Ena)) return NAK;
	u32 &t = g_Reg[DWHCI_DEV_IN_EP_XFER_SIZ (0)];
	unsigned n = t & 0x7F;
	if (pData) pData->assign ((const char *) Ptr (g_Reg[DWHCI_DEV_IN_EP_DMA_ADDR (0)]), n);
	t &= ~0x7Fu;
	c &= ~Ena;
	g_Reg[DWHCI_DEV_IN_EP_INT (0)] |= DWHCI_DEV_IN_EP_INT_XFER_COMPLETE;
	InIrq (0);
	return (int) n;
}
// one OUT packet to EP0: ACKed (true) or NAKed
static bool Out (const void *pData, unsigned n)
{
	u32 &c = g_Reg[DWHCI_DEV_OUT_EP_CTRL (0)];
	if (!(c & Ena)) return false;
	u32 &t = g_Reg[DWHCI_DEV_OUT_EP_XFER_SIZ (0)];
	CHECK (n <= (t & 0x7F));
	if (n) memcpy (Ptr (g_Reg[DWHCI_DEV_OUT_EP_DMA_ADDR (0)]), pData, n);
	t = (t & ~0x7Fu) | ((t & 0x7F) - n);
	c &= ~Ena;
	g_Reg[DWHCI_DEV_OUT_EP_INT (0)] |= DWHCI_DEV_OUT_EP_INT_XFER_COMPLETE;
	OutIrq (0);
	return true;
}

static const u8 VendorIn = REQUEST_IN | REQUEST_VENDOR | REQUEST_TO_DEVICE, VendorOut = REQUEST_OUT | REQUEST_VENDOR;

// a full IN request answered by the box after the deferral: the host gets sAnswer, then the status stage
static void Answer (const char *pAnswer)
{
	CHECK (g_pEP0->CompleteDeferred (pAnswer, (int) strlen (pAnswer)));
	std::string d;
	CHECK (In (&d) == (int) strlen (pAnswer) && d == pAnswer);
	CHECK (Out (nullptr, 0));		// the status stage
}

int main (void)
{
	g_Reg[DWHCI_DEV_IN_EP_COMMON_INT_MASK] = ~0u;
	g_Reg[DWHCI_DEV_OUT_EP_COMMON_INT_MASK] = ~0u;
	g_pEP0 = new CDWUSBGadgetEndpoint0 (64, &g_Gadget);
	g_pEP0->OnUSBReset ();
	g_pEP0->OnActivate ();
	CHECK (g_Reg[DWHCI_DEV_OUT_EP_CTRL (0)] & Ena);		// SETUP armed

	{	// a standard request (GET_DESCRIPTOR, IN): data, status, the next SETUP
		Setup (REQUEST_IN, GET_DESCRIPTOR, DESCRIPTOR_DEVICE << 8, 0, 18);
		std::string d;
		CHECK (In (&d) == 18 && (u8) d[0] == 18);
		CHECK (Out (nullptr, 0));
		CHECK (In () == NAK && (g_Reg[DWHCI_DEV_OUT_EP_CTRL (0)] & Ena));
	}
	{	// a deferred vendor IN request: NAKed until the box's answer, then data and status
		Setup (VendorIn, 1, 0, 0, 16);
		CHECK (g_Gadget.requests.size () == 1 && In () == NAK);
		CHECK (g_Reg[DWHCI_DEV_OUT_EP_CTRL (0)] & Ena);	// the next SETUP can come meanwhile
		Answer ("hello");
		CHECK (g_Gadget.aborted == 0);
	}
	{	// the host gives up on a deferred request and sends the next SETUP (the 2026-11-06 crash): the deferred one
		// is aborted, the new one taken as any; the late answer of the old one is the gadget's to drop (sequence)
		Setup (VendorIn, 2, 0, 0, 16);
		Setup (VendorIn, 3, 0, 0, 16);
		CHECK (g_Gadget.aborted == 1 && CDWUSBGadgetEndpoint0::GetSetupAbortCount () == 1);
		CHECK (g_Gadget.requests.size () == 3 && g_Gadget.requests.back ().bRequest == 3);
		Answer ("three");
	}
	{	// ... and the next one is host-to-device with data: its data stage, then deferred, then the status stage
		Setup (VendorIn, 4, 0, 0, 16);
		Setup (VendorOut, 5, 0, 0, 16);
		CHECK (g_Gadget.aborted == 2 && g_Gadget.requests.size () == 4);
		const char Data[17] = "0123456789abcdef";
		CHECK (Out (Data, 16));
		CHECK (g_Gadget.requests.size () == 5 && g_Gadget.out_data.size () == 16 && memcmp (g_Gadget.out_data.data (), Data, 16) == 0);
		CHECK (In () == NAK);			// status NAKed until the box took it
		CHECK (g_pEP0->CompleteDeferred (nullptr, 0));
		CHECK (In () == 0);			// the status stage
	}
	{	// a vendor OUT request with data whose number is a standard one's (9: SET_CONFIGURATION, 5: SET_ADDRESS) is the
		// gadget's, never the standard request (Circle's switch took it by number alone)
		const char Data[9] = "abcdefgh";
		for (u8 r : {(u8) SET_CONFIGURATION, (u8) SET_ADDRESS})
		{
			Setup (VendorOut, r, 0, 0, 8);
			CHECK (Out (Data, 8));
			CHECK (g_Gadget.requests.back ().bRequest == r && In () == NAK);
			CHECK (g_pEP0->CompleteDeferred (nullptr, 0) && In () == 0);
		}
		CHECK (g_Gadget.set_config == 0 && g_Gadget.set_address == 0);
	}
	{	// ... and the next one is a standard one without data (SET_INTERFACE): answered at once
		Setup (VendorIn, 6, 0, 0, 16);
		Setup (REQUEST_OUT | REQUEST_TO_INTERFACE, SET_INTERFACE, 1, 3, 0);
		CHECK (g_Gadget.aborted == 3 && g_Gadget.set_interface == 1);
		CHECK (In () == 0);
	}
	{	// the box's answer is armed as the host's next SETUP comes: the old answer is not sent as the new one's data
		Setup (VendorIn, 7, 0, 0, 16);
		CHECK (g_pEP0->CompleteDeferred ("old", 3));
		Setup (VendorIn, 8, 0, 0, 16);
		CHECK (In () == NAK);			// the old answer was stopped and flushed
		CHECK (g_Gadget.aborted == 3);		// it had been answered: nothing to abort
		Answer ("new");
	}
	{	// the box STALLs: so does EP0, until the next SETUP
		Setup (VendorIn, 9, 0, 0, 16);
		CHECK (g_pEP0->CompleteDeferred (nullptr, -1));
		CHECK (In () == STALL);
		Setup (VendorIn, 10, 0, 0, 16);
		Answer ("ok");
	}
	{	// completions nothing waits for: ignored and counted, no assert
		unsigned n = CDWUSBGadgetEndpoint::GetStrayCount ();
		g_Reg[DWHCI_DEV_IN_EP_INT (0)] |= DWHCI_DEV_IN_EP_INT_XFER_COMPLETE;
		InIrq (0);
		CHECK (CDWUSBGadgetEndpoint::GetStrayCount () == n + 1);
		CHECK (Out ("x", 1));			// an OUT packet with no OUT data stage: into the SETUP arm
		CHECK (CDWUSBGadgetEndpoint::GetStrayCount () == n + 2 && (g_Reg[DWHCI_DEV_OUT_EP_CTRL (0)] & Ena));
		Setup (VendorIn, 11, 0, 0, 16);		// and EP0 goes on
		Answer ("fine");
	}
	{	// another EP: a cancel waits for the disable and acknowledges it (no "EP disabled" panic later); a completion
		// latched for the cancelled transfer is ignored (it asserted)
		static const TUSBEndpointDescriptor Desc = {7, DESCRIPTOR_ENDPOINT, 0x81, 2, 64, 0};
		struct CTestEP : CDWUSBGadgetEndpoint
		{
			CTestEP (CDWUSBGadget *p) : CDWUSBGadgetEndpoint (&Desc, p) {}
			void OnActivate (void) override {}
			void OnDeactivate (void) override {}
			void OnTransferComplete (boolean, size_t) override { ++done; }
			void Send (void) { BeginTransfer (TransferDataIn, m_Buf, 10); }
			void Cancel (void) { CancelTransfer (); }
			int done = 0;
			DMA_BUFFER (u8, m_Buf, 64);
		};
		CTestEP *p = new CTestEP (&g_Gadget);
		p->OnUSBReset ();
		p->Send ();
		CHECK (g_Reg[DWHCI_DEV_IN_EP_CTRL (1)] & Ena);
		p->Cancel ();
		CHECK (!(g_Reg[DWHCI_DEV_IN_EP_CTRL (1)] & Ena) && g_Reg[DWHCI_DEV_IN_EP_INT (1)] == 0);
		unsigned n = CDWUSBGadgetEndpoint::GetStrayCount ();
		g_Reg[DWHCI_DEV_IN_EP_INT (1)] |= DWHCI_DEV_IN_EP_INT_XFER_COMPLETE;
		InIrq (1);
		CHECK (p->done == 0 && CDWUSBGadgetEndpoint::GetStrayCount () == n + 1);
		p->Send ();				// and it sends again
		g_Reg[DWHCI_DEV_IN_EP_CTRL (1)] &= ~Ena;
		g_Reg[DWHCI_DEV_IN_EP_XFER_SIZ (1)] &= ~0x7FFFFu;
		g_Reg[DWHCI_DEV_IN_EP_INT (1)] |= DWHCI_DEV_IN_EP_INT_XFER_COMPLETE;
		InIrq (1);
		CHECK (p->done == 1);
		delete p;
	}
	{	// the box's answer comes as the host's next SETUP is in but its IRQ not handled yet (SETUP done pending, the
		// SETUP count down): not armed, or the new request's first IN token would get the old answer
		Setup (VendorIn, 12, 0, 0, 16);
		int nAborted = g_Gadget.aborted;
		Setup (VendorIn, 13, 0, 0, 16, false);
		CHECK (g_pEP0->CompleteDeferred ("stale", 5));
		CHECK (In () == NAK);
		OutIrq (0);				// now the SETUP's IRQ: the old request aborted, the new one deferred
		CHECK (g_Gadget.aborted == nAborted + 1 && g_Gadget.requests.back ().bRequest == 13 && In () == NAK);
		Answer ("fresh");
		// the count alone (SETUP done already acknowledged elsewhere is not a case; the count is what the core keeps)
		Setup (VendorIn, 14, 0, 0, 16);
		u32 &t = g_Reg[DWHCI_DEV_OUT_EP_XFER_SIZ (0)];
		t &= ~(3u << 29);
		CHECK (g_pEP0->CompleteDeferred ("stale", 5) && In () == NAK);
		Setup (VendorIn, 15, 0, 0, 16);
		Answer ("fresh");
	}
	{	// an answer shorter than asked that ends on a packet boundary: a zero-length packet closes the data stage
		static char Big[129];
		memset (Big, 'x', 128);
		for (unsigned n : {64u, 128u})
		{
			Setup (VendorIn, 16, 0, 0, 512);
			CHECK (g_pEP0->CompleteDeferred (Big, (int) n));
			for (unsigned k = 0; k < n / 64; k++) CHECK (In () == 64);
			CHECK (In () == 0);		// the zero-length packet
			CHECK (In () == NAK);
			CHECK (Out (nullptr, 0));	// the status stage
		}
		Setup (VendorIn, 17, 0, 0, 64);		// all that was asked: no zero-length packet
		CHECK (g_pEP0->CompleteDeferred (Big, 64));
		CHECK (In () == 64 && In () == NAK);
		CHECK (Out (nullptr, 0));
		Setup (VendorIn, 18, 0, 0, 512);	// shorter, not on a boundary: none either
		CHECK (g_pEP0->CompleteDeferred (Big, 70));
		CHECK (In () == 64 && In () == 6 && In () == NAK);
		CHECK (Out (nullptr, 0));
	}
	CHECK (CDWUSBGadgetEndpoint::GetWaitTimeouts () == 0);
	CHECK (g_nPanics == 0);
	puts ("test_gadget_ep0: ok");
	return 0;
}

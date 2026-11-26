// pi/engine/ep_stop.h - stopping a bulk or interrupt endpoint that has a transfer armed (the USB-C network's CNcmEP,
// docs/usb-network.md), without Circle's panic. Portable over CDWHCIRegister: tests/test_gadget_ep0.cpp runs it on
// Circle's real endpoint code.
//
// CDWUSBGadgetEndpoint::CancelTransfer sets EPDis on the endpoint and returns. When the core has disabled it, it raises
// "EP disabled", and Circle's interrupt handler takes that as fatal for anything but an isochronous endpoint
// (LOGPANIC "EP%u disabled"). An OUT endpoint is also disabled reliably only under the global OUT NAK (DWC2 databook;
// Circle's own DisableEndpoint does it, but it is private). Circle's MIDI endpoints cancel only at a reset, when the core
// has already disabled them, so they never met this; the network cancels at a link change, a bus suspend and NetDetach,
// with the endpoint armed.
//
// stop (): called at IRQ level or with IRQs masked (the gadget's IRQ is core 0's), so the "EP disabled" it waits for is
// acknowledged here before the handler can see it. A completion that raced the cancel is acknowledged too (the
// transfer is forgotten either way). An IN endpoint's TX FIFO is flushed.
//
// Bus suspended (DSTS.SuspSts, the USB-C cable pulled or the computer idling the device): the host is gone and the PHY
// may be gated, the core may never raise GOUTNAKEff / EPDisbld. The waits are cut short then (kSuspPolls reads instead of
// kPolls), or stop () at IRQ level stalls core 0's other interrupts for as long as the full polls take. The time each stop
// took is kept (Stats, maximum per suspend) for the Pi 5 trial: CSidecarGadget::LogSuspendStops (the once-a-second LogStats; a bus suspend never reaches CSidecarGadget::OnSuspend)
// logs it once per suspend.
// EP: Cancel () (CancelTransfer)
#ifndef _ep_stop_h
#define _ep_stop_h

#include <circle/usb/dwhciregister.h>
#include <circle/timer.h>

namespace ep_stop {

constexpr unsigned kPolls = 5000;	// as Circle's DisableEndpoint: a few microseconds on hardware
constexpr unsigned kSuspPolls = 50;	// bus suspended: the same waits, cut short
constexpr u32 kSuspSts = 1u << 0;	// DSTS bit 0, suspend status (DWHCI_DEV_STS)

struct Stats
{
	unsigned nStops = 0, nSuspStops = 0;	// stop () calls since the last take, of them with the bus suspended
	unsigned maxUs = 0, maxSuspUs = 0;	// the longest of each
};
inline Stats g_Stats;				// IRQ level and task level: read with IRQs masked

// the stats since the last call, cleared; false when no stop ran
inline bool take (Stats *pOut)
{
	*pOut = g_Stats;
	g_Stats = Stats ();
	return pOut->nStops != 0;
}

// the time of one stop () into g_Stats
struct Timing
{
	const bool bSusp;
	const unsigned nStart;
	Timing (bool b) : bSusp (b), nStart (CTimer::GetClockTicks ()) {}
	~Timing ()
	{
		const unsigned nUs = CTimer::GetClockTicks () - nStart;
		g_Stats.nStops++;
		if (nUs > g_Stats.maxUs) g_Stats.maxUs = nUs;
		if (bSusp)
		{
			g_Stats.nSuspStops++;
			if (nUs > g_Stats.maxSuspUs) g_Stats.maxSuspUs = nUs;
		}
	}
};

template <class EP>
void stop (EP &e, unsigned nEP, bool bIn)
{
	const bool bSusp = (CDWHCIRegister (DWHCI_DEV_STS).Read () & kSuspSts) != 0;
	const unsigned nPolls = bSusp ? kSuspPolls : kPolls;
	Timing Time (bSusp);
	if (bIn)
	{
		CDWHCIRegister Ctrl (DWHCI_DEV_IN_EP_CTRL (nEP));
		const bool bEnabled = (Ctrl.Read () & DWHCI_DEV_EP_CTRL_EP_ENABLE) != 0;
		if (bEnabled)			// NAK the host's next IN tokens first
		{
			Ctrl.And (~(DWHCI_DEV_EP_CTRL_CLEAR_NAK | DWHCI_DEV_EP_CTRL_SETDPID_D0 | DWHCI_DEV_EP_CTRL_SETDPID_D1));
			Ctrl.Or (DWHCI_DEV_EP_CTRL_SET_NAK);
			Ctrl.Write ();
		}
		e.Cancel ();
		if (bEnabled)
		{
			CDWHCIRegister Int (DWHCI_DEV_IN_EP_INT (nEP));
			for (unsigned i = 0; i < nPolls && !(Int.Read () & DWHCI_DEV_IN_EP_INT_EP_DISABLED); i++) {}
		}
		CDWHCIRegister Ack (DWHCI_DEV_IN_EP_INT (nEP), DWHCI_DEV_IN_EP_INT_EP_DISABLED | DWHCI_DEV_IN_EP_INT_XFER_COMPLETE);
		Ack.Write ();
		if (bEnabled)			// what was in its FIFO is not sent later
		{
			CDWHCIRegister Reset (DWHCI_CORE_RESET, 0);
			Reset.Or (DWHCI_CORE_RESET_TX_FIFO_FLUSH);
			Reset.Or (nEP << DWHCI_CORE_RESET_TX_FIFO_NUM__SHIFT);
			Reset.Write ();
			for (unsigned i = 0; i < nPolls && (Reset.Read () & DWHCI_CORE_RESET_TX_FIFO_FLUSH); i++) {}
		}
		return;
	}
	CDWHCIRegister Ctrl (DWHCI_DEV_OUT_EP_CTRL (nEP));
	const bool bEnabled = (Ctrl.Read () & DWHCI_DEV_EP_CTRL_EP_ENABLE) != 0;
	if (bEnabled)
	{
		CDWHCIRegister DevCtrl (DWHCI_DEV_CTRL);
		DevCtrl.Read ();
		DevCtrl.Or (DWHCI_DEV_CTRL_SET_GLOBAL_OUT_NAK);
		DevCtrl.Write ();
		CDWHCIRegister IntStat (DWHCI_CORE_INT_STAT);
		for (unsigned i = 0; i < nPolls && !(IntStat.Read () & DWHCI_CORE_INT_STAT_GLOBAL_OUT_NAK_EFF); i++) {}
	}
	e.Cancel ();
	if (bEnabled)
	{
		CDWHCIRegister Int (DWHCI_DEV_OUT_EP_INT (nEP));
		for (unsigned i = 0; i < nPolls && !(Int.Read () & DWHCI_DEV_OUT_EP_INT_EP_DISABLED); i++) {}
	}
	CDWHCIRegister Ack (DWHCI_DEV_OUT_EP_INT (nEP), DWHCI_DEV_OUT_EP_INT_EP_DISABLED | DWHCI_DEV_OUT_EP_INT_XFER_COMPLETE);
	Ack.Write ();
	if (bEnabled)
	{
		CDWHCIRegister DevCtrl (DWHCI_DEV_CTRL);
		DevCtrl.Read ();
		DevCtrl.And (~DWHCI_DEV_CTRL_SET_GLOBAL_OUT_NAK);
		DevCtrl.Or (DWHCI_DEV_CTRL_CLEAR_GLOBAL_OUT_NAK);
		DevCtrl.Write ();
		CDWHCIRegister NakAck (DWHCI_CORE_INT_STAT, DWHCI_CORE_INT_STAT_GLOBAL_OUT_NAK_EFF);
		NakAck.Write ();
	}
}

}	// namespace ep_stop

#endif

// pi/engine/mirror_ep.h - usb=mirror (docs/usb-mirror.md): one of the box's endpoints on the computer's side (CMirrorEP,
// sidecar_gadget.cpp) at a bus suspend and at the resume after it. Portable: tests/test_gadget_ep0.cpp runs it on
// Circle's real endpoint code.
//
// A bus suspend (the computer idles the device; macOS does it some 10 s after the box is shown, before the
// Overbridge driver opens it) cancels the endpoint's transfer: a USB reset after the suspend passes no OnDeactivate,
// and the endpoint must not be armed then. A resume WITHOUT a reset brings OnResume and nothing else: no
// SET_CONFIGURATION, no OnActivate. CMirrorEP had no OnResume, so after the first suspend and resume its endpoints
// stayed inactive for good: pipe OUT NAKed (the computer's async writes failed, kIOReturnError), pipe IN and the audio
// never sent, Overbridge stuck at "Measuring" (0.4.1 rc6 on the Pi 4, 2026-11-06). The cancel did not wait either, so
// the isochronous transfer completed afterwards with nothing armed: rc4 asserted on it (the reset of the first
// Overbridge start after a switch), rc6 counted it (stray, "EP3: IN / OUT completion with no transfer armed").
//
// EpWake: an endpoint active at the suspend is activated again at the resume (as the configuration still holds).
// cancel (): an isochronous transfer is stopped and waited for (StopIsoTransfer: the core done with it, its completion
// acknowledged), any other cancelled as before.
#ifndef _mirror_ep_h
#define _mirror_ep_h

namespace mirror {

// EP: Active (), OnActivate (), OnDeactivate ()
struct EpWake
{
	bool resume = false;
	template <class EP> void suspend (EP &e)	{ resume = e.Active (); e.OnDeactivate (); }
	template <class EP> void wake (EP &e)		{ if (!resume) return; resume = false; e.OnActivate (); }
	void activated (void)				{ resume = false; }	// a reset and SET_CONFIGURATION came instead
};

// EP: Iso (), StopIso (), Cancel ()
template <class EP>
void cancel (EP &e)
{
	if (e.Iso ()) e.StopIso ();
	else e.Cancel ();
}

}	// namespace mirror

#endif

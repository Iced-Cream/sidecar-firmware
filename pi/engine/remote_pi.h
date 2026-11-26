// pi/engine/remote_pi.h - the remote UI service on the Pi (docs/remote-ui.md): Circle's side of remote_srv.h. TCP port 7340,
// served ONLY while the network stack's device is the USB-C one (usbnet_pi::Iface () == kIfUsb: the cable's trust rule, the
// interface a connection arrived on, never its address): over Wi-Fi or Ethernet the port is not even opened. One client at a
// time; a second one is told "busy" and closed. With usbtrust=1 (default) the cable needs no proof; with usbtrust=0 the client
// proves it knows the pairing code (HMAC over a nonce: the code is never sent, logged or put in a URL).
//
// The views are fed by the kernel's loop (kernel.cpp), which builds each one once for whoever wants it. A view's gate is a
// reference count: the local display (its page shown) and the remote client (its subscription) each hold one, so the app's
// page is fed while the Pi's screen shows another, and the Pi's page costs nothing extra when nobody is looking.
//   if (remote_pi::Gate (RMT_V_VOICE, sidecar_voice_shown ())) { fill; if (shown) sidecar_voice_set (&v); remote_pi::Offer (RMT_V_VOICE, &v); }
// Everything runs on core 0 (the kernel's loop and Circle tasks never preempt each other: no locks).
#ifndef _pi_engine_remote_pi_h
#define _pi_engine_remote_pi_h

#include <circle/net/netsubsystem.h>
#include <circle/types.h>

#include "../ui/remote/rmt_views.h"

class CConsole;

namespace remote_pi {

// Once, after the network is up and usbnet_pi::Use () has run (kernel.cpp InitNet): starts the service when the stack's
// device is the USB-C one. pConsole: where the accepted command lines go. pFw: the build's version text.
void Start (CNetSubSystem *pNet, CConsole *pConsole, const char *pFw);

boolean Running (void);

// The local display's hold on a view (its page is shown) is passed in, the combined gate comes out: TRUE while the display
// or a client wants the view fed. Call it every loop for each view.
boolean Gate (int view, boolean bLocalShown);

// The freshly built view (the whole struct, as the setter takes it): sent to the client as changed sections, at its rate
// (taken: TRUE; held back by the subscription's rate: FALSE, so a builder that runs only when its view changed offers it again)
boolean Offer (int view, const void *pView);

// The client is subscribed to the view and has no snapshot of it yet (a new subscription, a resync, a frame that did not fit): a change-driven
// builder (SEQ, PAT) offers its current view when this is TRUE even though nothing changed
boolean Due (int view);

// The Pi's local page (sidecar_ui_page ()), so the app can follow it
void Page (int nPage);

}	// namespace remote_pi

#endif

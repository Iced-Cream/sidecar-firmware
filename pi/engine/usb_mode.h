// pi/engine/usb_mode.h - what the Pi 4's USB-C side shows the computer, read in one place. Today the cmdline's usb=
// (sidecar | mirror, docs/usb-mirror.md); a stored Sidecar setting can take over here without touching its users.
#ifndef _usb_mode_h
#define _usb_mode_h

#include <circle/koptions.h>
#include <circle/util.h>

namespace usb_mode {

enum Mode { kSidecar, kMirror };

inline Mode get (void)
{
	return strcmp (CKernelOptions::Get ()->GetAppOptionString ("usb", "sidecar"), "mirror") == 0 ? kMirror : kSidecar;
}

}	// namespace usb_mode

#endif

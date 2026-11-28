// pi/engine/pi_display.h - the Pi-attached display's Circle side (S8k; docs/pi-display.md): a minimal panel and touch
// driver taken from the removed pi/ui tree (sidecar_display.cpp, 3a8bf7d) without LVGL, the splash, themes, EDID, pinch,
// the screensaver or the Pi 5's rp1dsi and portrait rotation. It gives core/pidisplay (through bridge.h) a panel to draw
// on and the touches as one finger; everything with logic (layout, stack, keys, the calibration's maths) is portable and
// host-tested there.
//
// - Frame buffer: the firmware's, 16 bits, on the DSI display when the firmware reports one (cmdline dsi=0: display 0),
//   else HDMI. Drawn by the control core (core/pidisplay's tick) straight into the frame buffer's memory, each row
//   cleaned from the data cache: no Circle call and no DMA channel off core 0.
// - Touch: a USB touch screen (Circle's HID digitizer driver, circle-touch*.patch; priority) or the DSI display's firmware
//   touch buffer (CRPiTouchScreen, polled), bound when they appear (plug-and-play). Raw values mapped to panel pixels by
//   cmdline touchcal=x0,x1,y0,y1,swap (core/pidisplay/touch_cal.h), else the USB descriptor's logical range, else 12/15
//   bit auto. Handed to the control core through aligned words (the IRQ on core 0 writes them; TakeTouch reads).
// - Off unless cmdline pidisplay=1 (S8k is host-tested only; no unit run has used it).
#pragma once
#include <circle/types.h>

namespace pi_display {
// core 0, from the kernel's Initialize after USB: the frame buffer and the DSI touch; false: off (pidisplay=0, the default,
// or no frame buffer). On success the panel is handed to bridge (bridge::pidisplay_attach).
boolean Init (void);
// core 0's loop: USB touch bound / unbound at plug-and-play, the touch devices polled
void Poll (boolean bPlugAndPlayUpdated);
boolean On (void);
const char *Input (void);   // "touch (USB)", "touch (DSI)" or "none"
}

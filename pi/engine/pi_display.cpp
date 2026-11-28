// pi/engine/pi_display.cpp - see pi_display.h. Taken from pi/ui/sidecar_display.cpp (3a8bf7d), which was based on Circle's
// addon/lvgl/lvgl.cpp (GPL-3.0, R. Stange): the frame buffer's choice of display, the touch binding (USB priority, the
// DSI firmware buffer second) and the raw-to-pixel mapping, with LVGL, the splash, themes, EDID, pinch, the screensaver
// and the Pi 5's rp1dsi / rotation left out.
#include "pi_display.h"

#include <circle/bcmframebuffer.h>
#include <circle/bcmpropertytags.h>
#include <circle/devicenameservice.h>
#include <circle/input/rpitouchscreen.h>
#include <circle/input/touchscreen.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/string.h>
#include <circle/synchronize.h>
#include <circle/usb/usbhiddevice.h>
#include <circle/usb/usbtouchscreen.h>
#include <circle/util.h>

#include "bridge.h"
#include "pidisplay/touch_cal.h"

LOGMODULE ("display");

namespace pi_display {
namespace {

// the firmware's displays (Pi 4: max_framebuffers=2 in config.txt gives each its frame buffer): "get display id" per
// display number -> 0 the DSI LCD (the official 7" display or a compatible), 2 HDMI0, 7 HDMI1, 3 composite
#define PROPTAG_GET_DISPLAY_ID	0x00040016
const unsigned DisplayIdDsi = 0;

const char *DisplayName (unsigned nId)
{
	switch (nId)
	{
	case 0:	return "DSI";
	case 1:	return "DSI-aux";
	case 2:	return "HDMI0";
	case 3:	return "composite";
	case 7:	return "HDMI1";
	case ~0u: return "?";
	default: return "other";
	}
}

struct TState
{
	boolean		bOn;
	CBcmFrameBuffer	*pFB;
	uintptr		nBuffer;		// the frame buffer's memory, its pitch in bytes, its size
	unsigned	nPitch, nWidth, nHeight;
	boolean		bDsiSeen, bDsiUnknown;

	CDevice *volatile pTouch;		// the USB touch screen (priority)
	CRPiTouchScreen	*pRPi;			// the DSI display's firmware touch buffer, and its touchN
	CDevice		*pDsiTouch;
	unsigned	nDsiInvert;		// cmdline dsitouch_inv: 1 x, 2 y
	const char	*pSource;		// the touch source last logged
	unsigned	nLogMaxX, nLogMaxY;	// the USB touch's logical maxima (its descriptor), 0: not known
	unsigned	nRawMax;		// auto: 0 (raw = pixels), 4095 or 32767
	boolean		bCal;
	touch_cal_t	Cal;

	// core 0 (the USB IRQ, the DSI poll) -> the control core (TakeTouch)
	volatile u32	nPointer;		// pressed << 31 | y << 16 | x (panel pixels)
	volatile u32	nSeq;			// bumped per event
	volatile u32	nDownSeq;		// fingers down so far: a tap shorter than one read still shows
	u32		nReadSeq, nReadDown;	// the control core's
	boolean		bTapUp;			// TakeTouch owes the release of a short tap
};
TState s;

void Map (unsigned nRawX, unsigned nRawY, int *pX, int *pY)
{
	if (s.bCal)
	{
		touch_cal_map (&s.Cal, nRawX, nRawY, (int) s.nWidth, (int) s.nHeight, pX, pY);
		return;
	}
	unsigned x1 = s.nWidth - 1, y1 = s.nHeight - 1;
	if (s.nLogMaxX && s.nLogMaxY)
	{
		x1 = s.nLogMaxX; y1 = s.nLogMaxY;
	}
	else
	{
		if (s.nRawMax < 32767 && (nRawX >= s.nWidth || nRawY >= s.nHeight))
			s.nRawMax = (nRawX > 4095 || nRawY > 4095) ? 32767 : 4095;
		if (s.nRawMax) x1 = y1 = s.nRawMax;
	}
	int x = (int) ((long) nRawX * (s.nWidth - 1) / x1), y = (int) ((long) nRawY * (s.nHeight - 1) / y1);
	*pX = x < 0 ? 0 : x >= (int) s.nWidth ? s.nWidth - 1 : x;
	*pY = y < 0 ? 0 : y >= (int) s.nHeight ? s.nHeight - 1 : y;
}

void PushEvent (TTouchScreenEvent Event, int x, int y)
{
	u32 nOld = s.nPointer;
	if (Event == TouchScreenEventFingerUp) s.nPointer = nOld & 0x7FFF7FFF;		// released where it was
	else
	{
		s.nPointer = 1u << 31 | (u32) (y & 0x7FFF) << 16 | (u32) (x & 0x7FFF);
		if (Event == TouchScreenEventFingerDown) s.nDownSeq = s.nDownSeq + 1;
	}
	DataMemBarrier ();
	s.nSeq = s.nSeq + 1;
}

void TouchHandler (TTouchScreenEvent Event, unsigned nID, unsigned nPosX, unsigned nPosY)
{
	if (nID != 0) return;						// the first finger only
	int x = 0, y = 0;
	if (Event != TouchScreenEventFingerUp) Map (nPosX, nPosY, &x, &y);
	PushEvent (Event, x, y);
}

// the DSI panel's coordinates (the firmware's buffer: 800x480) to the frame buffer's, unless a USB touch screen is bound
void DsiTouchHandler (TTouchScreenEvent Event, unsigned nID, unsigned nPosX, unsigned nPosY)
{
	if (s.pTouch || nID != 0) return;
	const unsigned PW = 800, PH = 480;
	unsigned x = nPosX < PW ? nPosX : PW - 1, y = nPosY < PH ? nPosY : PH - 1;
	if (s.nDsiInvert & 1) x = PW - 1 - x;
	if (s.nDsiInvert & 2) y = PH - 1 - y;
	PushEvent (Event, (int) (x * s.nWidth / PW), (int) (y * s.nHeight / PH));
}

void TouchRemoved (CDevice *pDevice, void *pContext)
{
	s.pTouch = 0;
}

// the control core: the next touch for core/pidisplay (a tap shorter than one read: its press, then its release)
bool TakeTouch (int *pX, int *pY, bool *pDown)
{
	u32 p = s.nPointer;
	*pX = (int) (p & 0x7FFF), *pY = (int) ((p >> 16) & 0x7FFF);
	if (s.bTapUp)
	{
		s.bTapUp = FALSE;
		*pDown = false;
		return true;
	}
	u32 nSeq = s.nSeq;
	if (nSeq == s.nReadSeq) return false;
	DataMemBarrier ();
	p = s.nPointer;
	*pX = (int) (p & 0x7FFF), *pY = (int) ((p >> 16) & 0x7FFF);
	s.nReadSeq = nSeq;
	u32 nDown = s.nDownSeq;
	boolean bDown = (p >> 31) != 0;
	if (!bDown && nDown != s.nReadDown)	// pressed and released between two reads
	{
		s.nReadDown = nDown;
		s.bTapUp = TRUE;
		*pDown = true;
		return true;
	}
	s.nReadDown = nDown;
	*pDown = bDown;
	return true;
}

// the control core (core/pidisplay's tick): RGB565 rows straight into the frame buffer, each row cleaned from the cache
void Put (void *pCtx, int x, int y, int w, int h, const u16 *pPixels, int nStride)
{
	for (int r = 0; r < h; r++)
	{
		uintptr nRow = s.nBuffer + (uintptr) (y + r) * s.nPitch + (uintptr) x * 2;
		memcpy ((void *) nRow, pPixels + r * nStride, (size_t) w * 2);
		CleanAndInvalidateDataCacheRange (nRow, (u32) w * 2);
	}
}

boolean OpenFrameBuffer (void)
{
	unsigned nDisplays = CBcmFrameBuffer::GetNumDisplays ();
	boolean bPreferDsi = CKernelOptions::Get ()->GetAppOptionDecimal ("dsi", 1) != 0;
	CString List;
	unsigned nDisplay = 0;
	boolean bIdAnswered = FALSE;
	for (unsigned i = 0; i < nDisplays && i < 8; i++)
	{
		CBcmPropertyTags Tags;
		TPropertyTagSimple Tag;
		Tag.nValue = i;
		unsigned nId = ~0u;
		if (Tags.GetTag (PROPTAG_GET_DISPLAY_ID, &Tag, sizeof Tag, 4))
		{
			nId = Tag.nValue;
			bIdAnswered = TRUE;
		}
		if (nId == DisplayIdDsi)
		{
			if (!s.bDsiSeen && bPreferDsi) nDisplay = i;
			s.bDsiSeen = TRUE;
		}
		CString Item;
		Item.Format (" %u=%s", i, DisplayName (nId));
		List.Append (Item);
	}
	s.bDsiUnknown = !bIdAnswered;
	s.pFB = new CBcmFrameBuffer (0, 0, 16, 0, 0, nDisplay);	// the firmware's size (config.txt / EDID)
	boolean bOK = s.pFB->Initialize () && s.pFB->GetDepth () == 16;
	LOGNOTE ("firmware %u display(s):%s%s -> frame buffer on %u %ux%u%s", nDisplays, (const char *) List,
		 bIdAnswered ? "" : " (display id not answered)", nDisplay,
		 bOK ? s.pFB->GetWidth () : 0, bOK ? s.pFB->GetHeight () : 0, bOK ? "" : " FAILED");
	if (!bOK) return FALSE;						// (not deleted: Circle's frame buffer has no safe teardown)
	s.nBuffer = s.pFB->GetBuffer ();
	s.nPitch = s.pFB->GetPitch ();
	s.nWidth = s.pFB->GetWidth ();
	s.nHeight = s.pFB->GetHeight ();
	return s.nBuffer != 0;
}

// the official 7" display's touch (and compatibles'): the firmware reads the FT5406 and keeps the points in a buffer
// that CRPiTouchScreen polls (its touchN's Update, from Poll). Only when the firmware reports a DSI display (or does not
// answer the display id), never with dsi=0. cmdline dsitouch_inv=1 (x), 2 (y), 3 (both) flips it.
void OpenDsiTouch (void)
{
	if (CKernelOptions::Get ()->GetAppOptionDecimal ("dsi", 1) == 0) return;
	if (!s.bDsiSeen && !s.bDsiUnknown)
	{
		LOGNOTE ("touch: no DSI display reported, no firmware touch");
		return;
	}
	unsigned nInv = CKernelOptions::Get ()->GetAppOptionDecimal ("dsitouch_inv", 0);
	s.nDsiInvert = nInv <= 3 ? nInv : 0;
	CDeviceNameService *pNames = CDeviceNameService::Get ();
	CDevice *pBefore[8];						// the touch devices so far: the new one is the DSI's
	for (unsigned i = 0; i < 8; i++)
	{
		CString Name;
		Name.Format ("touch%u", i + 1);
		pBefore[i] = pNames->GetDevice (Name, FALSE);
	}
	CRPiTouchScreen *pRPi = new CRPiTouchScreen;
	if (!pRPi->Initialize ())
	{
		LOGNOTE ("touch: DSI (firmware) not available");
		delete pRPi;
		return;
	}
	s.pRPi = pRPi;
	for (unsigned i = 0; i < 8 && !s.pDsiTouch; i++)
	{
		CString Name;
		Name.Format ("touch%u", i + 1);
		CDevice *pDev = pNames->GetDevice (Name, FALSE);
		if (pDev && pDev != pBefore[i]) s.pDsiTouch = pDev;
	}
	if (!s.pDsiTouch) return;
	CTouchScreenDevice *pTouch = (CTouchScreenDevice *) s.pDsiTouch;
	pTouch->Setup (s.pFB);
	pTouch->RegisterEventHandler (DsiTouchHandler);
	LOGNOTE ("touch: DSI (firmware) touch buffer set up%s", s.nDsiInvert ? ", inverted" : "");
}

// a USB touch screen (any touchN but the DSI's): the one with the most contacts, as Circle's driver reports it
void BindInput (void)
{
	CDeviceNameService *pNames = CDeviceNameService::Get ();
	CUSBTouchScreenDevice *pBest = CUSBTouchScreenDevice::Get ();
	CDevice *pPreferred = pBest ? pBest->GetTouchDevice () : 0;
	for (unsigned i = 1; i <= 8 && !s.pTouch; i++)
	{
		CString Name;
		Name.Format ("touch%u", i);
		CTouchScreenDevice *pTouch = (CTouchScreenDevice *) pNames->GetDevice (Name, FALSE);
		if (pTouch && (CDevice *) pTouch != s.pDsiTouch && (!pPreferred || (CDevice *) pTouch == pPreferred))
		{
			pTouch->Setup (s.pFB);				// no SetCalibration: raw values (Map scales them)
			pTouch->RegisterEventHandler (TouchHandler);
			pTouch->RegisterRemovedHandler (TouchRemoved, 0);
			s.pTouch = pTouch;
			if (!pBest || !pBest->GetLogicalMax (&s.nLogMaxX, &s.nLogMaxY)) s.nLogMaxX = s.nLogMaxY = 0;
			LOGNOTE ("touch screen bound (%s), raw range %s%u x %u", (const char *) Name,
				 s.nLogMaxX ? "0.." : "auto ", s.nLogMaxX, s.nLogMaxY);
		}
	}
	const char *pSource = s.pTouch ? "USB" : s.pDsiTouch ? "DSI (firmware)" : 0;
	if (pSource != s.pSource)
	{
		s.pSource = pSource;
		LOGNOTE ("touch: %s", pSource ? pSource : "none");
	}
}
}  // namespace

boolean Init (void)
{
	if (CKernelOptions::Get ()->GetAppOptionDecimal ("pidisplay", 0) == 0)
	{
		LOGNOTE ("off (pidisplay=1 turns it on)");
		return FALSE;
	}
	if (!OpenFrameBuffer ())
	{
		LOGWARN ("no frame buffer: the display is off");
		return FALSE;
	}
	const char *pCal = CKernelOptions::Get ()->GetAppOptionString ("touchcal", 0);
	s.bCal = pCal && touch_cal_parse (pCal, &s.Cal);
	if (pCal && !s.bCal) LOGWARN ("touchcal=%s not understood (x0,x1,y0,y1,swap): not used", pCal);
	s.bOn = TRUE;
	OpenDsiTouch ();
	BindInput ();
	bridge::pidisplay_attach ((int) s.nWidth, (int) s.nHeight, (bridge::PanelPut) Put, 0, TakeTouch);
	LOGNOTE ("%ux%u for the screen stack (core/pidisplay)%s", s.nWidth, s.nHeight, s.bCal ? ", touch calibrated" : "");
	return TRUE;
}

void Poll (boolean bPlugAndPlayUpdated)
{
	if (!s.bOn) return;
	if (bPlugAndPlayUpdated) BindInput ();
	// the USB touch screen's recovery (circle-hid-reenum*.patch): Poll may re-enumerate it, which deletes its touchN
	// (TouchRemoved) and makes a new one, so re-bind when the count moved
	CUSBTouchScreenDevice *pUSBTouch = CUSBTouchScreenDevice::Get ();
	if (pUSBTouch)
	{
		unsigned nReEnums = CUSBHIDDevice::GetReEnumCount ();
		pUSBTouch->Poll ();
		if (CUSBHIDDevice::GetReEnumCount () != nReEnums) BindInput ();
	}
	if (s.pTouch) ((CTouchScreenDevice *) (CDevice *) s.pTouch)->Update ();
	if (s.pDsiTouch) ((CTouchScreenDevice *) s.pDsiTouch)->Update ();
}

boolean On (void)
{
	return s.bOn;
}

const char *Input (void)
{
	return s.pTouch ? "touch (USB)" : s.pDsiTouch ? "touch (DSI)" : "none";
}
}  // namespace pi_display

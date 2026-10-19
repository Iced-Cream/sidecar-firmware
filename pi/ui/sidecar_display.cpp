// pi/ui/sidecar_display.cpp - see sidecar_display.h. Based on Circle's addon/lvgl/lvgl.cpp (GPL-3.0, R. Stange),
// changed: the touch screen and mouse are bound late (they enumerate after USB's plug-and-play), raw touch values are
// scaled here (Circle's USB touch driver reports the HID logical range unscaled), synchronous flushes, a tick callback,
// the firmware's DSI display preferred and its touch (CRPiTouchScreen) as a second touch source (docs/pi-display.md).
#include "sidecar_display.h"
#include "sidecar_splash.h"
#include "lvgl.h"
#include <circle/bcmpropertytags.h>
#include <circle/devicenameservice.h>
#include <circle/input/mouse.h>
#include <circle/input/rpitouchscreen.h>
#include <circle/input/touchscreen.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/new.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/usb/usbtouchscreen.h>
#include <circle/util.h>

LOGMODULE ("display");

CSidecarDisplay *CSidecarDisplay::s_pThis = 0;

struct TSidecarDisplayHooks
{
	static void Flush (lv_display_t *pDisplay, const lv_area_t *pArea, u8 *pBuffer);
	static void PointerRead (lv_indev_t *pIndev, lv_indev_data_t *pData);
	static u32 Tick (void);
	static void Log (lv_log_level_t Level, const char *pMsg);
	static void TouchHandler (TTouchScreenEvent Event, unsigned nID, unsigned nPosX, unsigned nPosY);
	static void DsiTouchHandler (TTouchScreenEvent Event, unsigned nID, unsigned nPosX, unsigned nPosY);
	static void MouseHandler (TMouseEvent Event, unsigned nButtons, unsigned nPosX, unsigned nPosY, int nWheel);
	static void TouchRemoved (CDevice *pDevice, void *pContext);
	static void MouseRemoved (CDevice *pDevice, void *pContext);
};
typedef TSidecarDisplayHooks H;

CSidecarDisplay::CSidecarDisplay (unsigned nWidth, unsigned nHeight)
:	m_nReqWidth (nWidth),
	m_nReqHeight (nHeight),
	m_pFB (0),
	m_nDisplay (0),
	m_bDsiSeen (FALSE),
	m_bDsiUnknown (FALSE),
	m_bFrameBuffer (FALSE),
	m_bOn (FALSE),
	m_nWidth (0),
	m_nHeight (0),
	m_pBuffer (0),
	m_nLastUpdate (0),
	m_nMaxUs (0),
	m_pTouch (0),
	m_pMouse (0),
	m_pRPiTouch (0),
	m_pDsiTouch (0),
	m_nDsiInvert (0),
	m_pSource (0),
	m_pCalibration (0),
	m_nRawMax (0),
	m_nPointer (0),
	m_nRaw (0),
	m_nTouchSeq (0),
	m_nEvents (0),
	m_nDiagEvents (0),
	m_nDiagReports (0),
	m_nDiagResets (0),
	m_nShownSeq (0),
	m_nSaverMin (60),
	m_nLastActive (0),
	m_bBlank (FALSE),
	m_bSwallow (FALSE),
	m_bStatusNew (FALSE)
{
	memset (&m_Status, 0, sizeof m_Status);
	for (unsigned c = 0; c < SIDECAR_UI_CORES; c++) m_Status.load[c] = -1;
	s_pThis = this;
}

// The firmware's displays (Pi 4: max_framebuffers=2 in config.txt gives each its frame buffer): "get display id" per
// display number -> 0 the DSI LCD (the official 7" display or a compatible), 2 HDMI0, 7 HDMI1, 3 composite
#define PROPTAG_GET_DISPLAY_ID	0x00040016
static const unsigned DisplayIdDsi = 0;

static const char *DisplayName (unsigned nId)
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

// the frame buffer, once: on the DSI display if the firmware reports one (cmdline dsi=0: display 0, as before), and the
// firmware's report logged: "display: firmware 2 display(s): 0=HDMI0 1=DSI -> frame buffer on 1 (DSI) 800x480"
boolean CSidecarDisplay::OpenFrameBuffer (void)
{
	if (m_bFrameBuffer) return TRUE;
	if (m_pFB) return FALSE;				// tried, failed
	unsigned nDisplays = CBcmFrameBuffer::GetNumDisplays ();
	boolean bPreferDsi = CKernelOptions::Get ()->GetAppOptionDecimal ("dsi", 1) != 0;
	CString List;
	m_nDisplay = 0;
	m_bDsiSeen = FALSE;
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
			if (!m_bDsiSeen && bPreferDsi) m_nDisplay = i;
			m_bDsiSeen = TRUE;
		}
		CString Item;
		Item.Format (" %u=%s", i, DisplayName (nId));
		List.Append (Item);
	}
	m_bDsiUnknown = !bIdAnswered;
	m_pFB = new CBcmFrameBuffer (m_nReqWidth, m_nReqHeight, 16, 0, 0, m_nDisplay);
	m_bFrameBuffer = m_pFB->Initialize ();
	LOGNOTE ("firmware %u display(s):%s%s -> frame buffer on %u %ux%u%s", nDisplays, (const char *) List,
		 bIdAnswered ? "" : " (display id not answered)", m_nDisplay,
		 m_bFrameBuffer ? m_pFB->GetWidth () : 0, m_bFrameBuffer ? m_pFB->GetHeight () : 0,
		 m_bFrameBuffer ? "" : " FAILED");
	return m_bFrameBuffer;
}

boolean CSidecarDisplay::Splash (void)
{
	if (!OpenFrameBuffer ()) return FALSE;
	sidecar_splash_draw ((u16 *) (uintptr) m_pFB->GetBuffer (), m_pFB->GetPitch () / 2,
			     m_pFB->GetWidth (), m_pFB->GetHeight ());
	return TRUE;
}

void CSidecarDisplay::SplashStep (unsigned nStep)
{
	if (!m_bFrameBuffer || m_bOn) return;
	sidecar_splash_progress ((u16 *) (uintptr) m_pFB->GetBuffer (), m_pFB->GetPitch () / 2,
				 m_pFB->GetWidth (), m_pFB->GetHeight (), nStep, SplashSteps);
}

boolean CSidecarDisplay::Initialize (void)
{
	if (!OpenFrameBuffer ())
	{
		LOGWARN ("no frame buffer: the display is off");
		return FALSE;
	}
	m_nWidth = m_pFB->GetWidth ();
	m_nHeight = m_pFB->GetHeight ();

	lv_init ();
	lv_tick_set_cb (H::Tick);
	lv_log_register_print_cb (H::Log);

	const unsigned nPixels = m_nWidth * 48;			// a 48-line band: 77 KB at 800 wide
	m_pBuffer = new (HEAP_DMA30) u16[nPixels];
	if (!m_pBuffer) return FALSE;

	lv_display_t *pDisplay = lv_display_create (m_nWidth, m_nHeight);
	lv_display_set_flush_cb (pDisplay, H::Flush);
	lv_display_set_buffers (pDisplay, m_pBuffer, 0, nPixels * sizeof (u16), LV_DISPLAY_RENDER_MODE_PARTIAL);

	lv_indev_t *pIndev = lv_indev_create ();
	lv_indev_set_type (pIndev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb (pIndev, H::PointerRead);

	const unsigned *pCal = CKernelOptions::Get ()->GetTouchScreen ();
	if (pCal && pCal[0] < pCal[1] && pCal[2] < pCal[3]) m_pCalibration = pCal;

	sidecar_ui_create ();
	m_bOn = TRUE;
	Activity ();
	LOGNOTE ("%ux%u, LVGL %d.%d", m_nWidth, m_nHeight, LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR);
	OpenDsiTouch ();
	BindInput ();
	return TRUE;
}

// The official 7" display's touch (and compatibles'): the firmware reads the FT5406 over the display's I2C and keeps
// the points in a buffer that CRPiTouchScreen polls (its CTouchScreenDevice's Update, from Update below). Created when
// the firmware reports a DSI display (or does not answer the display id: then it is tried), never with dsi=0.
// Coordinates are the panel's 0..799 / 0..479; cmdline dsitouch_inv=1 (x), 2 (y), 3 (both) flips them.
void CSidecarDisplay::OpenDsiTouch (void)
{
	if (CKernelOptions::Get ()->GetAppOptionDecimal ("dsi", 1) == 0) return;
	if (!m_bDsiSeen && !m_bDsiUnknown)
	{
		LOGNOTE ("touch: no DSI display reported, no firmware touch");
		return;
	}
	unsigned nInv = CKernelOptions::Get ()->GetAppOptionDecimal ("dsitouch_inv", 0);
	m_nDsiInvert = nInv <= 3 ? nInv : 0;
	CDeviceNameService *pNames = CDeviceNameService::Get ();
	CDevice *pBefore[8];					// the touch devices so far: the new one is the DSI's
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
	m_pRPiTouch = pRPi;
	for (unsigned i = 0; i < 8 && !m_pDsiTouch; i++)
	{
		CString Name;
		Name.Format ("touch%u", i + 1);
		CDevice *pDev = pNames->GetDevice (Name, FALSE);
		if (pDev && pDev != pBefore[i]) m_pDsiTouch = pDev;
	}
	if (!m_pDsiTouch) return;
	CTouchScreenDevice *pTouch = (CTouchScreenDevice *) (CDevice *) m_pDsiTouch;
	pTouch->Setup (m_pFB);
	pTouch->RegisterEventHandler (H::DsiTouchHandler);
	LOGNOTE ("touch: DSI (firmware) touch buffer set up%s", m_nDsiInvert ? ", inverted" : "");
}

const char *CSidecarDisplay::GetInput (void) const
{
	return m_pTouch ? "touch (USB)" : m_pDsiTouch ? "touch (DSI)" : m_pMouse ? "mouse1" : "none";
}

// The USB touch screen (any touchN but the DSI's) has priority: while it is bound the DSI's events are dropped
void CSidecarDisplay::BindInput (void)
{
	CDeviceNameService *pNames = CDeviceNameService::Get ();
	for (unsigned i = 1; i <= 8 && !m_pTouch; i++)
	{
		CString Name;
		Name.Format ("touch%u", i);
		CTouchScreenDevice *pTouch = (CTouchScreenDevice *) pNames->GetDevice (Name, FALSE);
		if (pTouch && (CDevice *) pTouch != m_pDsiTouch)
		{
			pTouch->Setup (m_pFB);		// no SetCalibration: raw values (Map scales them)
			pTouch->RegisterEventHandler (H::TouchHandler);
			pTouch->RegisterRemovedHandler (H::TouchRemoved, this);
			m_pTouch = pTouch;
			LOGNOTE ("touch screen bound (%s)", (const char *) Name);
		}
	}
	const char *pSource = m_pTouch ? "USB" : m_pDsiTouch ? "DSI (firmware)" : 0;
	if (pSource != m_pSource)
	{
		m_pSource = pSource;
		LOGNOTE ("touch: %s", pSource ? pSource : "none");
	}
	if (!m_pTouch && !m_pDsiTouch && !m_pMouse)
	{
		CMouseDevice *pMouse = (CMouseDevice *) pNames->GetDevice ("mouse1", FALSE);
		if (pMouse && pMouse->Setup (m_pFB, FALSE))
		{
			pMouse->RegisterEventHandler (H::MouseHandler);
			pMouse->RegisterRemovedHandler (H::MouseRemoved, this);
			m_pMouse = pMouse;
			LOGNOTE ("mouse bound (mouse1)");
		}
	}
}

void CSidecarDisplay::SetScreensaver (unsigned nMinutes)
{
	m_nSaverMin = nMinutes;
	Activity ();
}

void CSidecarDisplay::Activity (void)
{
	m_nLastActive = CTimer::GetClockTicks64 ();
}

// The firmware's "blank screen" property (0x00040002): black, and with hdmi_blanking=1 in config.txt the HDMI output off
// (a monitor's power saving; whether a panel's backlight follows is the panel's)
#define PROPTAG_BLANK_SCREEN	0x00040002

void CSidecarDisplay::Blank (boolean bOn)
{
	m_bBlank = bOn;
	if (bOn)
	{
		CDisplay::TArea Area;				// black, row by row out of the (zeroed) draw buffer
		memset (m_pBuffer, 0, m_nWidth * 40 * sizeof (u16));
		for (unsigned y = 0; y < m_nHeight; y += 40)
		{
			Area.x1 = 0; Area.x2 = m_nWidth - 1;
			Area.y1 = y; Area.y2 = (y + 40 < m_nHeight ? y + 40 : m_nHeight) - 1;
			m_pFB->SetArea (Area, m_pBuffer);
		}
	}
	CBcmPropertyTags Tags;
	TPropertyTagSimple Tag;
	Tag.nValue = bOn ? 1 : 0;
	boolean bOK = Tags.GetTag (PROPTAG_BLANK_SCREEN, &Tag, sizeof Tag, 4);
	if (bOn) LOGNOTE ("screensaver: blank after %u min without a touch (firmware blank %s)", m_nSaverMin, bOK ? "ok" : "refused");
	else
	{
		lv_obj_invalidate (lv_screen_active ());	// everything drawn again
		LOGNOTE ("screensaver: woken by a touch");
	}
}

void CSidecarDisplay::Update (boolean bPlugAndPlayUpdated)
{
	if (!m_bOn) return;
	if (bPlugAndPlayUpdated) BindInput ();

	unsigned nNow = CTimer::GetClockTicks ();
	if (nNow - m_nLastUpdate < 10 * CLOCKHZ / 1000) return;
	m_nLastUpdate = nNow;

	if (m_pMouse) ((CMouseDevice *) m_pMouse)->UpdateCursor ();	// polls the mouse (cursor off)
	if (m_pTouch) ((CTouchScreenDevice *) m_pTouch)->Update ();
	if (m_pDsiTouch) ((CTouchScreenDevice *) m_pDsiTouch)->Update ();	// reads the firmware's touch buffer
	CUSBTouchScreenDevice *pUSBTouch = CUSBTouchScreenDevice::Get ();
	if (pUSBTouch) pUSBTouch->Poll ();		// resets and resubmits a halted report endpoint (main loop: xHCI commands)

	u32 nSeq = m_nTouchSeq;
	boolean bDown = (m_nPointer >> 31) != 0;
	if (m_bSwallow && !bDown) m_bSwallow = FALSE;	// the waking finger is up: touches reach the controls again
	if (nSeq != m_nShownSeq) Activity ();
	if (m_bBlank)
	{
		if (nSeq != m_nShownSeq && bDown)
		{
			m_bSwallow = TRUE;			// this press only wakes the screen
			Blank (FALSE);
		}
		else
		{
			m_nShownSeq = nSeq;
			return;					// black: LVGL not run
		}
	}
	else if (m_nSaverMin && CTimer::GetClockTicks64 () - m_nLastActive >= (u64) m_nSaverMin * 60 * CLOCKHZ)
	{
		m_nShownSeq = nSeq;
		Blank (TRUE);
		return;
	}
	if (nSeq != m_nShownSeq)
	{
		m_nShownSeq = nSeq;
		u32 p = m_nPointer, r = m_nRaw;
		sidecar_ui_touch (p >> 31, r & 0xFFFF, r >> 16, p & 0x7FFF, (p >> 16) & 0x7FFF);
	}
	if (m_bStatusNew)
	{
		m_bStatusNew = FALSE;
		sidecar_ui_status (&m_Status);
	}
	lv_timer_handler ();
	unsigned nUs = CTimer::GetClockTicks () - nNow;
	if (nUs > m_nMaxUs) m_nMaxUs = nUs;
}

const char *CSidecarDisplay::TakeTouchDiag (void)
{
	m_DiagText[0] = '\0';
	CUSBTouchScreenDevice *pUSB = CUSBTouchScreenDevice::Get ();
	if (!m_pTouch || !pUSB) return m_DiagText;

	CUSBHIDDevice::THIDStats S;
	pUSB->GetHIDStats (&S);
	u32 nEvents = m_nEvents, p = m_nPointer, r = m_nRaw;
	CString Text;
	Text.Format (" | touch: ev +%u rep +%u raw %u,%u %s | hid err %u (xhci %u, last code %u) resub %u reset %u (+%u) fail %u "
		     "ep state %u pend %u ignored %u%s",
		     nEvents - m_nDiagEvents, S.nReports - m_nDiagReports, r & 0xFFFF, r >> 16, (p >> 31) ? "DOWN" : "up",
		     S.nErrors, S.nXhciErrors, S.nLastCode, S.nResubmits, S.nResets, S.nResets - m_nDiagResets, S.nFailed,
		     S.nState, S.nPending, pUSB->GetIgnored (), S.bGaveUp ? " GAVE-UP" : "");
	m_nDiagEvents = nEvents;
	m_nDiagReports = S.nReports;
	m_nDiagResets = S.nResets;
	strncpy (m_DiagText, (const char *) Text, sizeof m_DiagText - 1);
	m_DiagText[sizeof m_DiagText - 1] = '\0';
	return m_DiagText;
}

unsigned CSidecarDisplay::TakeMaxUpdateUs (void)
{
	unsigned n = m_nMaxUs;
	m_nMaxUs = 0;
	return n;
}

void CSidecarDisplay::SetStatus (const sidecar_ui_status_t &rStatus)
{
	m_Status = rStatus;
	m_bStatusNew = TRUE;
}

void H::Flush (lv_display_t *pDisplay, const lv_area_t *pArea, u8 *pBuffer)
{
	CDisplay::TArea Area;
	Area.x1 = pArea->x1;
	Area.x2 = pArea->x2;
	Area.y1 = pArea->y1;
	Area.y2 = pArea->y2;
	CSidecarDisplay::s_pThis->m_pFB->SetArea (Area, pBuffer);		// no completion routine: DMA and wait
	lv_display_flush_ready (pDisplay);
}

void H::PointerRead (lv_indev_t *pIndev, lv_indev_data_t *pData)
{
	u32 p = CSidecarDisplay::s_pThis->m_nPointer;
	pData->state = (p >> 31) && !CSidecarDisplay::s_pThis->m_bSwallow ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
	pData->point.x = p & 0x7FFF;
	pData->point.y = (p >> 16) & 0x7FFF;
}

void H::Log (lv_log_level_t Level, const char *pMsg)
{
	CLogger::Get ()->Write ("lvgl", Level >= LV_LOG_LEVEL_ERROR ? LogError : LogNotice, "%s", pMsg);
}

u32 H::Tick (void)
{
	return (u32) (CTimer::GetClockTicks64 () / (CLOCKHZ / 1000));
}

// raw -> screen: touchscreen= from cmdline.txt, else raw = pixels until a value is out of the screen, then a 12-bit
// (0-4095) or 15-bit (0-32767) range, the usual HID digitizer ranges
void CSidecarDisplay::Map (unsigned nRawX, unsigned nRawY, int *pX, int *pY)
{
	unsigned x0 = 0, x1 = m_nWidth - 1, y0 = 0, y1 = m_nHeight - 1;
	if (m_pCalibration)
	{
		x0 = m_pCalibration[0]; x1 = m_pCalibration[1]; y0 = m_pCalibration[2]; y1 = m_pCalibration[3];
	}
	else
	{
		if (m_nRawMax < 32767 && (nRawX >= m_nWidth || nRawY >= m_nHeight))
			m_nRawMax = (nRawX > 4095 || nRawY > 4095) ? 32767 : 4095;
		if (m_nRawMax) x1 = y1 = m_nRawMax;
	}
	int x = (int) ((long) ((int) nRawX - (int) x0) * (m_nWidth - 1) / (x1 - x0));
	int y = (int) ((long) ((int) nRawY - (int) y0) * (m_nHeight - 1) / (y1 - y0));
	*pX = x < 0 ? 0 : x >= (int) m_nWidth ? m_nWidth - 1 : x;
	*pY = y < 0 ? 0 : y >= (int) m_nHeight ? m_nHeight - 1 : y;
}

void H::TouchHandler (TTouchScreenEvent Event, unsigned nID, unsigned nPosX, unsigned nPosY)
{
	CSidecarDisplay *p = CSidecarDisplay::s_pThis;
	p->m_nEvents = p->m_nEvents + 1;
	if (nID != 0) return;						// the first finger only
	u32 nOld = p->m_nPointer;
	if (Event == TouchScreenEventFingerUp)
	{
		p->m_nPointer = nOld & 0x7FFF7FFF;			// released where it was
	}
	else
	{
		int x, y;
		p->Map (nPosX, nPosY, &x, &y);
		p->m_nRaw = (nPosY & 0xFFFF) << 16 | (nPosX & 0xFFFF);
		p->m_nPointer = 1u << 31 | (u32) y << 16 | (u32) x;
	}
	p->m_nTouchSeq = p->m_nTouchSeq + 1;
}

// the DSI panel's coordinates (800x480) to the screen's, unless a USB touch screen is bound (it has priority)
void H::DsiTouchHandler (TTouchScreenEvent Event, unsigned nID, unsigned nPosX, unsigned nPosY)
{
	CSidecarDisplay *p = CSidecarDisplay::s_pThis;
	if (p->m_pTouch) return;
	p->m_nEvents = p->m_nEvents + 1;
	if (nID != 0) return;						// the first finger only
	u32 nOld = p->m_nPointer;
	if (Event == TouchScreenEventFingerUp)
	{
		p->m_nPointer = nOld & 0x7FFF7FFF;
	}
	else
	{
		unsigned x = nPosX < 800 ? nPosX : 799, y = nPosY < 480 ? nPosY : 479;
		if (p->m_nDsiInvert & 1) x = 799 - x;
		if (p->m_nDsiInvert & 2) y = 479 - y;
		x = x * p->m_nWidth / 800;
		y = y * p->m_nHeight / 480;
		p->m_nRaw = (nPosY & 0xFFFF) << 16 | (nPosX & 0xFFFF);
		p->m_nPointer = 1u << 31 | (u32) y << 16 | (u32) x;
	}
	p->m_nTouchSeq = p->m_nTouchSeq + 1;
}

void H::MouseHandler (TMouseEvent Event, unsigned nButtons, unsigned nPosX, unsigned nPosY, int nWheel)
{
	CSidecarDisplay *p = CSidecarDisplay::s_pThis;
	p->m_nEvents = p->m_nEvents + 1;
	u32 nDown = p->m_nPointer & (1u << 31);
	if (Event == MouseEventMouseDown && (nButtons & MOUSE_BUTTON_LEFT)) nDown = 1u << 31;
	else if (Event == MouseEventMouseUp && (nButtons & MOUSE_BUTTON_LEFT)) nDown = 0;
	p->m_nRaw = (nPosY & 0xFFFF) << 16 | (nPosX & 0xFFFF);
	p->m_nPointer = nDown | (nPosY & 0x7FFF) << 16 | (nPosX & 0x7FFF);
	p->m_nTouchSeq = p->m_nTouchSeq + 1;
}

void H::TouchRemoved (CDevice *pDevice, void *pContext)
{
	((CSidecarDisplay *) pContext)->m_pTouch = 0;
}

void H::MouseRemoved (CDevice *pDevice, void *pContext)
{
	((CSidecarDisplay *) pContext)->m_pMouse = 0;
}

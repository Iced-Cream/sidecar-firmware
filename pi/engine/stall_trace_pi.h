// stall_trace_pi.h - the engine's one stall_trace ring (stall_trace.h) with the Pi's clock and the xHCI MFINDEX
// (pi/circle/circle-xhci-trace.patch). StallNote is safe at IRQ and task level. On a host test or a Pi without the
// xHCI host the MFINDEX reads 0.
#pragma once

#include "stall_trace.h"
#include <circle/timer.h>
#if RASPPI >= 4 && !defined(OB_USB_TEST)
#include <circle/usb/xhcicommandmanager.h>
#include <circle/usb/xhciendpoint.h>
#define STALL_TRACE_XHCI 1
#endif

inline stall_trace::Ring<1024> g_StallTrace;	// 16 KB: every Teensy vendor request (credits too) for 3 s and more

inline uint16_t StallTraceMf(void) {
#ifdef STALL_TRACE_XHCI
  return (uint16_t)CXHCICommandManager::ReadMFIndex();
#else
  return 0;
#endif
}

inline void StallNote(uint8_t kind, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0, uint16_t d = 0, uint16_t e = 0) {
  g_StallTrace.Add(CTimer::GetClockTicks(), StallTraceMf(), kind, a, b, c, d, e);
}

// every xHCI command as it is rung, and every endpoint starting after an idle time (installed by CObUsb's first box)
inline void StallTraceInstall(void) {
#ifdef STALL_TRACE_XHCI
  CXHCICommandManager::s_pTraceHook = [](unsigned nType, unsigned nSlot) {
    StallNote(stall_trace::kXhciCmd, 0, (uint8_t)nType, (uint8_t)nSlot);
  };
  CXHCIEndpoint::s_pStartHook = [](unsigned nSlot, unsigned nEp, unsigned nType, unsigned nIdleMs) {
    StallNote(stall_trace::kEpStart, (uint8_t)nType, (uint8_t)nEp, (uint8_t)nSlot, (uint16_t)nIdleMs);
  };
#endif
}

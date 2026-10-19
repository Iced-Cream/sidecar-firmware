// pi/lvgl-test/kernel.h - the display test kernel (docs/pi-display.md): the Sidecar screen (pi/ui) on the HDMI display,
// touch over USB, the log on the serial port at 921600 baud, RYTM-REBOOT for flash.sh. Single core, no engine.
#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/serial.h>
#include <circle/timer.h>
#include <circle/types.h>
#include <circle/usb/usbhcidevice.h>
#include "sidecar_display.h"

class CKernel
{
public:
	CKernel (void);
	boolean Initialize (void);
	void Run (void);

private:
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CSerialDevice		m_Serial;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CUSBHCIDevice		m_USBHCI;
	CSidecarDisplay		m_Display;
};

#endif

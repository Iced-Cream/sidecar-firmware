#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/serial.h>
#include <circle/screen.h>
#include <circle/device.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/types.h>

// Writes the log to the serial port and, if it came up, the HDMI screen.
class CTeeDevice : public CDevice
{
public:
	CTeeDevice (CDevice *pA) : m_pA (pA), m_pB (0) {}
	void SetSecond (CDevice *pB) { m_pB = pB; }
	int Write (const void *pBuffer, size_t nCount)
	{
		if (m_pB)	m_pB->Write (pBuffer, nCount);
		return m_pA->Write (pBuffer, nCount);
	}

private:
	CDevice *m_pA;
	CDevice *m_pB;
};

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);
	void Run (void);

private:
	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CScreenDevice		m_Screen;
	CSerialDevice		m_Serial;
	CTeeDevice		m_Tee;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
};

#endif

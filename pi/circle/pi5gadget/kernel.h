// pi/circle/pi5gadget/kernel.h - the Pi 5 USB-C bring-up kernel (docs/pi5.md "Bring-up kernel"): Circle's USB MIDI gadget
// alone, as sample/29-miniorgan with USB_GADGET_MODE, on BCM2712's DWC2, every USB event on the serial console (921600,
// GPIO14/15). The computer's MIDI is echoed back, so pi/engine/tests/gadget_midi_test.py runs against it. While no USB
// reset has come it walks through the ways of setting up the core (usbinit=, circle-pi5-gadget.patch). It reboots by
// itself, which on a tryboot leaves the Pi on the card's normal kernel.
#ifndef _pi5gadget_kernel_h
#define _pi5gadget_kernel_h

#include <circle/actled.h>
#include <circle/bcmwatchdog.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/serial.h>
#include <circle/timer.h>
#include <circle/types.h>
#include <circle/usb/gadget/usbmidigadget.h>

class CTestGadget : public CUSBMIDIGadget
{
public:
	CTestGadget (CInterruptSystem *pInterrupt) : CUSBMIDIGadget (pInterrupt) {}

	boolean Bind (void);			// the MIDI device's handler, once configured; TRUE: configured
	unsigned GetInPackets (void) const	{ return m_nInPackets; }
	unsigned GetEchoFails (void) const	{ return m_nEchoFail; }

protected:
	const void *GetDescriptor (u16 wValue, u16 wIndex, size_t *pLength) override;

private:
	static void PacketHandler (unsigned nCable, u8 *pPacket, unsigned nLength, unsigned nDevice, void *pParam);

	CUSBMIDIDevice *m_pBound = 0;
	volatile unsigned m_nInPackets = 0, m_nEchoFail = 0;
};

class CKernel
{
public:
	CKernel (void);
	boolean Initialize (void);
	void Run (void);

private:
	void LogBoard (void);
	void LogStatus (void);
	void TryInit (unsigned nInit);
	void Command (char c);

	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CSerialDevice		m_Serial;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CBcmWatchdog		m_Watchdog;
	CTestGadget		m_Gadget;

	unsigned		m_nInit;		// the way of setting up the core now
	unsigned		m_nInitSecs;		// when it was set up
	boolean			m_bWalk;
};

#endif

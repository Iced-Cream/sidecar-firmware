// pi/engine/transport.h - a host transport (spec "Test path": the USB Overbridge host here, TCP to rytm-emu in plan
// 2026-10-14-pi-engine-emulator-link.md; the engine core never knows which). A transport drains g_Output at the
// host's clock, pushes the host's MIDI bytes into g_MidiIn, and takes the engine's bytes from g_MidiOut.
#ifndef _transport_h
#define _transport_h
#include <circle/logger.h>
#include <circle/types.h>

class CTransport
{
public:
	virtual ~CTransport (void) {}
	virtual const char *Name (void) const = 0;
	virtual boolean Running (void) const = 0;		// the host's clock runs (blocks flow)
	virtual void Poll (void) = 0;				// core 0's loop: g_MidiOut, and what else it needs
	virtual void Stop (void) = 0;				// before a reboot: the host's stream stopped cleanly
	virtual void LogStats (CLogger *pLogger) = 0;		// once a second
	virtual unsigned MIDIOutDropped (void) const = 0;	// engine bytes not sent (counted)
};

#endif

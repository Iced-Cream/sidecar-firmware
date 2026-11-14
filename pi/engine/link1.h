// pi/engine/link1.h - the second box's USB MIDI: link 1 of core/device/device.h (docs/device-profiles.md "Several devices").
// One Elektron box on USB that is not the host link (the Rytm's, link 0) gets its own queues: its bytes to the control core,
// where each whole SysEx is handled as link 1's (core::engine_sysex_link), and the engine's replies to link 1 back to it as
// records, sent from core 0's loop. Nothing here touches link 0's queues (g_MidiIn / g_MidiOut), so a Rytm alone runs as it did.
//
//   core 0, USB IRQ   Packet         the box's MIDI bytes -> g_Link1In
//   core 0, loop      Send           g_Link1Out's records -> the box's MIDI OUT, packed as USB MIDI event packets
//   core 0, loop      SetDevice      the box came (its CUSBMIDIDevice) or went (0)
//   control core      Poll           g_Link1In -> whole SysEx -> engine_sysex_link(1, ...); the sender handed to
//                                    core::device::set_out(1, ...) while a device is there
#ifndef _link1_h
#define _link1_h
#include <stdint.h>

#include "spsc.h"
#include "records.h"
#include "sysex_asm.h"

class CUSBMIDIDevice;

namespace link1 {

const unsigned kInBytes = 4096, kOutBytes = 8192;
extern Spsc<uint8_t, kInBytes> g_In;
extern Spsc<uint8_t, kOutBytes> g_Out;

struct TStats {
	unsigned nInLost;	// its bytes lost: g_In full
	unsigned nOutFull;	// engine bytes for it lost: g_Out full, or a record > 4096
	unsigned nSent, nSendFail;	// SysEx sent to it; failed sends (the message dropped)
	unsigned nSysEx, nBadSysEx;	// whole SysEx handed to the engine; pieces dropped (too long, broken)
};
const TStats &Stats (void);

void Packet (unsigned nCable, uint8_t *pPacket, unsigned nLength);
void SetDevice (CUSBMIDIDevice *pDevice);
CUSBMIDIDevice *Device (void);
void Send (void);
void Poll (void);
void ToBox (const uint8_t *pData, int nLength);	// core::device::set_out(1, ...): records into g_Out

}  // namespace link1

#endif

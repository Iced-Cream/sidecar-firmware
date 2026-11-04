// pi/engine/shared.h - what the cores share (one definition, shared.cpp): the output stage (control core -> the
// transport), the host's MIDI bytes (the transport -> control core), the engine's bytes for the host (control
// core -> core 0's transport), the console's commands (core 0 -> control core). Every queue has one producer and
// one consumer;
// g_MidiOut is read only through g_ToHost (records.h), whichever transport consumes it.
#ifndef _shared_h
#define _shared_h
#include <stdint.h>

#include "records.h"
#include "spsc.h"

// g_MidiOut: a burst of replies (0x10: pool table, JUNO and FM names; a 0D listing) fits whole while core 0 sends
const unsigned kMidiOutBytes = 32768;

struct TCmd { char text[192]; };   // pool=S:<a path under /Samples> is the longest

// The generic engine has no output stage (audio_return.h): the streams and the frames sent since the last one started,
// as g_Output counted them (mbtrace.h's records; core 0's transports write, any core reads)
extern volatile unsigned g_nStreams, g_nFramesSinceReset;
extern Spsc<uint8_t, 8192> g_MidiIn;
extern Spsc<uint8_t, kMidiOutBytes> g_MidiOut;
extern RecordReader<Spsc<uint8_t, kMidiOutBytes>> g_ToHost;	// g_MidiOut's only reader: every consumer takes whole records
extern Spsc<TCmd, 16> g_Cmds;
// The USB-A MIDI devices (core/devmidi.h): their channel and realtime messages for the control core, each (src << 24 |
// status << 16 | d1 << 8 | d2) - src 0..3 a USB-A device; status 0xF4 a source plugged in (d1 1) / out (d1 0), its
// name in g_KeyNames[src] (written before the push). Producers: core 0's USB IRQs and core 0's loop with IRQs masked.
// And what core/devmidi sends on to the Rytm (control core -> core 0's transport), USB MIDI event packets (cable 0).
extern Spsc<uint32_t, 1024> g_KeysIn;
extern Spsc<uint32_t, 512> g_KeysToRytm;
extern char g_KeyNames[5][24];
extern volatile bool g_bRytmGone;   // the Rytm's USB MIDI went (core 0 -> control core, once a removal)
extern unsigned g_nToHostFull;      // engine bytes for the host lost: a full g_MidiOut (none expected) or a record > 4096 B
extern volatile bool g_bRytmResync; // records for the Rytm were lost and sending is on again (core 0 -> control core, DropResync)
extern volatile bool g_bRytmOff;    // records for the Rytm are being dropped: no umidi1, or a failed send has the sending off (core 0 -> control core)

#endif

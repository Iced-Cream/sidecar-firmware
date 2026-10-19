// pi/engine/shared.h - what the cores share (one definition, shared.cpp): the output stage (control core -> the
// transport), the host's MIDI bytes (the transport -> control core), the engine's bytes for the host (control
// core -> core 0's transport), the console's commands (core 0 -> control core). Every queue has one producer and
// one consumer;
// g_MidiOut is read only through g_ToHost (records.h), whichever transport consumes it.
#ifndef _shared_h
#define _shared_h
#include <stdint.h>

#include "output.h"
#include "records.h"
#include "spsc.h"

// g_MidiOut: a burst of replies (0x10: pool table, JUNO and FM names; a 0D listing) fits whole while core 0 sends
const unsigned kMidiOutBytes = 32768;

struct TCmd { char text[192]; };   // pool=S:<a path under /Samples> is the longest

extern Output g_Output;
extern Spsc<uint8_t, 8192> g_MidiIn;
extern Spsc<uint8_t, kMidiOutBytes> g_MidiOut;
extern RecordReader<Spsc<uint8_t, kMidiOutBytes>> g_ToHost;	// g_MidiOut's only reader: every consumer takes whole records
extern Spsc<TCmd, 16> g_Cmds;

#endif

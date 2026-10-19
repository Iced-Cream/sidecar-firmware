// pi/engine/shared.cpp - see shared.h.
#include "shared.h"

Output g_Output;
Spsc<uint8_t, 8192> g_MidiIn;
Spsc<uint8_t, kMidiOutBytes> g_MidiOut;
RecordReader<Spsc<uint8_t, kMidiOutBytes>> g_ToHost (&g_MidiOut);
Spsc<TCmd, 16> g_Cmds;

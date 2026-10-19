//
// synth.h - the audio source of the OUT blocks' MAIN L/R: one Plaits voice played by MIDI notes,
// or spike2's 1 kHz triangle, behind the safety stage every source goes through.
//
// Everything that touches the voice runs in the xHCI interrupt on core 0: Fill() from the OUT
// block fill (paced by IN), NoteOn/NoteOff from the USB MIDI completion. The voice renders just
// in time: when the FIFO holds fewer frames than a block needs, one 12-sample Plaits block is
// rendered into it, so the FIFO never holds more than 11 + 7 frames.
//
// Safety (the Pi's audio goes into the Rytm's main mix and the speakers): after Reset() (each new
// stream) the output fades in linearly over 200 ms, and then a hard limiter clips every sample at
// -12 dBFS. The voice is scaled to about -18 dBFS peak at full velocity.
//
// Run-time options (cmdline.txt):
//   obsrc=plaits|tone|off   MAIN L/R source (default plaits; tone: spike2's 1 kHz triangle)
//   plaitseng=N             Plaits engine 0..23 (default 20 MODAL, the Teensy's heaviest)
//   midich=N                MIDI channel 1..16, 0 = any (default 1)
//   voicedb=N               voice peak at full velocity, -dBFS (default 18, clamped 12..60)
//
#ifndef _synth_h
#define _synth_h

#include <circle/logger.h>
#include <circle/types.h>

class CSynth
{
public:
	enum TSource { SourcePlaits, SourceTone, SourceOff };

	static void Init (void);			// task level, once: options, voice init
	static void Bench (CLogger *pLogger);		// task level, before streaming: CPU per engine
	static void Reset (void);			// a new stream: fade in again, FIFO empty

	// IRQ: nFrames frames of MAIN L/R as int32 full scale; returns the frame where a trigger
	// takes effect (the first sample rendered after it), or -1
	static int Fill (s32 *pL, s32 *pR, unsigned nFrames);

	// IRQ (USB MIDI completion): one USB MIDI event's MIDI bytes
	static void MIDIPacket (unsigned nCable, const u8 *pPacket, unsigned nLength);

	static unsigned TotalPackets (void);		// MIDI packets since boot
	static void LogStats (CLogger *pLogger);	// once a second, task level
	static void LogNotes (CLogger *pLogger);	// sparse note log, task level

	// counter ticks (CNTPCT_EL0) and their rate
	static u64 Ticks (void);
	static u64 TickRate (void);
};

#endif

// pi/engine/selftest.cpp - see selftest.h. Authorised addition beyond the plan (the Rytm was off while Task 4 was
// built): it proves the cores, the output stage, the MIDI path and the capture without a Rytm.
#include "selftest.h"

#include <circle/logger.h>

#include "ob_usb.h"
#include "shared.h"
#include "sidecar_gadget.h"
#include "rec_pi.h"

static const char From[] = "selftest";

static inline u64 Ticks (void)
{
	u64 t;
	asm volatile ("isb; mrs %0, cntpct_el0" : "=r" (t) :: "memory");
	return t;
}

void CSelfTest::Start (void)
{
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (m_nFreq));
	g_Output.Reset ();				// a new stream: fade in again (the consumer's side: here)
	m_nT0 = m_nNoteAt = m_nLastLog = Ticks ();
	m_nSinceT0 = 0;
	m_nBlocks = m_nNotes = m_nSlips = 0;
	m_bOn = TRUE;
	CLogger::Get ()->Write (From, LogNotice, "on: %u Hz in %u-frame blocks paced by the generic timer, "
				"note %u on MIDI channel 1 every 0.5 s; nothing is sent to USB", Rate, FramesPerBlock, Note);
}

// Note-off then note-on on channel 1 (voice 0), as MIDI bytes into the control core's queue.
void CSelfTest::SendNote (void)
{
	static const u8 Bytes[6] = {0x80, Note, 0, 0x90, Note, Velocity};
	for (unsigned i = 0; i < sizeof Bytes; i++) g_MidiIn.push (Bytes[i]);
	CObUsb::CaptureMarkMIDI ();
	m_nNotes++;
}

// The recorder's IN stand-in (no stream in the self-test): IN channel c is a triangle at 110 * (c + 1) Hz, -12 dBFS,
// and every second (frame % 48000 == 0) a full-scale one-frame click on all 12 at once: the files' alignment marker.
static void RecordSynthetic (void)
{
	static u32 s_nFrame, s_nPhase[rec::kInChannels];
	s32 I[CSelfTest::FramesPerBlock][rec::kInChannels];
	for (unsigned f = 0; f < CSelfTest::FramesPerBlock; f++, s_nFrame++)
		for (int c = 0; c < rec::kInChannels; c++)
		{
			u32 p = s_nPhase[c] += (u32) (110ull * (c + 1) * 4294967296ull / 48000);
			s32 tri = (s32) (p ^ (u32) ((s32) p >> 31)) - 0x40000000;	// -2^30 .. 2^30
			I[f][c] = s_nFrame % 48000 == 0 ? 0x7FFFFF00 : tri / 2;
		}
	g_Rec.in_block (I, CSelfTest::FramesPerBlock);
}

void CSelfTest::Poll (void)
{
	m_nDropped += g_ToHost.Drain ();		// nothing is sent anywhere (whole records)
	if (!m_bOn) return;

	u64 now = Ticks ();
	if (!m_bQuiet && now - m_nNoteAt >= m_nFreq / 2)
	{
		m_nNoteAt = now;
		SendNote ();
	}

	// Blocks due by the clock, at most 4 per poll (core 0's loop runs every ~20 us while the self-test is on). More
	// than 0.5 ms behind (a log line, the dump's waits): the clock slips instead of draining the FIFO (96 frames) in
	// a burst, counted; the rendered audio itself stays continuous (the capture has no gap).
	for (int k = 0; k < 4; k++)
	{
		u64 at = m_nT0 + m_nSinceT0 * FramesPerBlock * m_nFreq / Rate;
		if (now < at) break;
		s32 F[FramesPerBlock][Output::kChannels], L[FramesPerBlock];
		s16 V[FramesPerBlock][Output::kVoices];
		g_Output.Pull (F, FramesPerBlock, V);
		if (CSidecarGadget *pS = CSidecarGadget::Get ())	// the Rytm's stand-in: OUT mixed in, then back as IN
		{
			pS->ToRytm (F, V, FramesPerBlock, TRUE);
			pS->FromEngine (F, FramesPerBlock);
		}
		for (unsigned f = 0; f < FramesPerBlock; f++) L[f] = F[f][0];
		CObUsb::CaptureFeed (L, FramesPerBlock);
		if (g_Rec.wants_side ()) g_Rec.out_block (V, 0, FramesPerBlock);	// the recorder: the voices as rendered
		if (g_Rec.wants_in ()) RecordSynthetic ();
		m_nSinceT0++;
		m_nBlocks++;
	}
	asm volatile ("dsb sy; sev");			// the control core refills the FIFO
	u64 at = m_nT0 + m_nSinceT0 * FramesPerBlock * m_nFreq / Rate;	// the next block's time
	if (now > at && now - at > m_nFreq / 2000)
	{
		m_nT0 = now;
		m_nSinceT0 = 0;
		m_nSlips++;
	}
}

void CSelfTest::LogStats (CLogger *pLogger)
{
	if (!m_bOn) return;
	u64 now = Ticks ();
	unsigned nMs = (unsigned) ((now - m_nLastLog) * 1000 / m_nFreq);
	m_nLastLog = now;
	if (nMs == 0) nMs = 1;
	pLogger->Write (From, LogNotice, "OUT %u/s notes %u slips %u%s", m_nBlocks * 1000 / nMs, m_nNotes, m_nSlips,
			m_bQuiet ? " (quiet: dumping)" : "");
	m_nBlocks = m_nNotes = 0;
}

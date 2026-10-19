//
// synth.cpp - see synth.h.
//
#include "synth.h"
#include "plaitsvoice.h"
#include "rytmobdevice.h"
#include <circle/koptions.h>
#include <circle/synchronize.h>
#include <circle/util.h>

static const char From[] = "synth";

static const unsigned kRate = 48000;
static const unsigned kFadeFrames = kRate / 5;			// 200 ms
static const float kLimit = 0.25f;				// -12 dBFS, hard
static const unsigned kFifo = 32;				// > 11 + 7

static CSynth::TSource s_Source = CSynth::SourcePlaits;
static unsigned s_nEngine = 20;
static unsigned s_nChannel = 1;					// 0 = any
static float s_fVoiceGain = 0.125f;				// -18 dBFS

static CPlaitsVoice *s_pVoice;
static CPlaitsVoice *s_pBenchVoice;

// the FIFO between the render and the OUT fill (both IRQ, core 0)
static float s_Fifo[kFifo];
static unsigned s_nFifoRd, s_nFifoWr;			// free-running
static unsigned s_nFaded;				// frames since Reset (fade-in)
static float s_fLevel = 1.0f;				// velocity
static boolean s_bPendingTrig;
static u8 s_nPendingNote;
static u64 s_nPendingAt;
static u32 s_nTonePhase;
static unsigned s_nTrigFifoIdx;			// FIFO index of the last trigger's first sample
static boolean s_bTrigMark;

// statistics (IRQ writes, LogStats reads and clears the per-second ones)
static volatile unsigned s_nRenders, s_nFifoMin = ~0u, s_nFifoMax;
static volatile u64 s_nRenderSum, s_nRenderMax;
static volatile unsigned s_nMIDIPackets, s_nClock, s_nTransport, s_nNoteOn, s_nNoteOff, s_nOtherCh, s_nOther;
static volatile unsigned s_nTotalPackets, s_nTotalNoteOn;
static volatile u64 s_nTrigLatMax, s_nTrigLatSum;
static volatile unsigned s_nTrigs;
static volatile float s_fPeak;

// sparse note log: the first notes in full, then one in 16
struct TNoteEv { u64 nAt; u64 nLat; u8 nStatus, nNote, nVel; boolean bRendered; };
static const unsigned kNoteLog = 32;
static TNoteEv s_NoteLog[kNoteLog];
static volatile unsigned s_nNoteLogWr, s_nNoteLogRd;
static volatile unsigned s_nNoteLogDropped;
static volatile int s_nLastLoggedTrig = -1;		// index into s_NoteLog waiting for its render time

u64 CSynth::Ticks (void)
{
	u64 n;
	asm volatile ("isb; mrs %0, cntpct_el0" : "=r" (n) :: "memory");
	return n;
}

u64 CSynth::TickRate (void)
{
	u64 n;
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (n));
	return n;
}

static unsigned long long BenchTicks (void)
{
	return CSynth::Ticks ();
}

void CSynth::Init (void)
{
	CKernelOptions *pOpt = CKernelOptions::Get ();
	const char *pSrc = pOpt->GetAppOptionString ("obsrc", "plaits");
	s_Source = strcmp (pSrc, "tone") == 0 ? SourceTone : strcmp (pSrc, "off") == 0 ? SourceOff : SourcePlaits;
	s_nEngine = pOpt->GetAppOptionDecimal ("plaitseng", 20);
	if (s_nEngine >= CPlaitsVoice::Engines) s_nEngine = 20;
	s_nChannel = pOpt->GetAppOptionDecimal ("midich", 1);
	if (s_nChannel > 16) s_nChannel = 1;
	unsigned nDb = pOpt->GetAppOptionDecimal ("voicedb", 18);
	if (nDb < 12) nDb = 12;
	if (nDb > 60) nDb = 60;
	float g = 1.0f;
	for (unsigned i = 0; i < nDb; i++) g *= 0.891250938f;	// -1 dB
	s_fVoiceGain = g;

	CPlaitsVoice *pVoice = new CPlaitsVoice;
	pVoice->Init ();
	pVoice->SetEngine (s_nEngine);
	// a medium, percussive patch: Plaits' ping envelope, DECAY 0.5
	pVoice->SetParams (0.5f, 0.5f, 0.5f, 0.5f, 0.5f);
	Reset ();
	DataMemBarrier ();
	s_pVoice = pVoice;

	static const char *Srcs[] = {"Plaits", "1 kHz triangle", "silence"};
	CLogger::Get ()->Write (From, LogNotice, "MAIN L/R source: %s; engine %u %s, MIDI channel %u%s, "
				"voice -%u dBFS peak, fade-in 200 ms, limiter -12 dBFS",
				Srcs[s_Source], s_nEngine, CPlaitsVoice::EngineName (s_nEngine), s_nChannel,
				s_nChannel == 0 ? " (any)" : "", nDb);
}

void CSynth::Bench (CLogger *pLogger)
{
	s_pBenchVoice = new CPlaitsVoice;
	s_pBenchVoice->Init ();
	const double fBlockTicks = (double) TickRate () * CPlaitsVoice::BlockSize / kRate;
	double fTotal = 0, fWorst = 0;
	unsigned nWorst = 0;
	pLogger->Write (From, LogNotice, "bench: Plaits per voice (100 %% = one A72 core in real time), "
			"12-sample blocks, 2 s per engine, counter %u Hz", (unsigned) TickRate ());
	for (unsigned e = 0; e < CPlaitsVoice::Engines; e++)
	{
		unsigned long long nSum, nMax;
		unsigned nBlocks;
		s_pBenchVoice->Bench (e, BenchTicks, &nSum, &nMax, &nBlocks);
		double fAvg = 100.0 * nSum / nBlocks / fBlockTicks;
		double fMax = 100.0 * nMax / fBlockTicks;
		fTotal += fAvg;
		if (fAvg > fWorst) { fWorst = fAvg; nWorst = e; }
		pLogger->Write (From, LogNotice, "bench: %2u %-12s avg %3u.%02u %%  max %3u.%02u %%",
				e, CPlaitsVoice::EngineName (e), (unsigned) fAvg, (unsigned) (fAvg * 100) % 100,
				(unsigned) fMax, (unsigned) (fMax * 100) % 100);
	}
	double fMean = fTotal / CPlaitsVoice::Engines;
	pLogger->Write (From, LogNotice, "bench: mean %u.%02u %% per voice; worst %s %u.%02u %% -> %u voices of it per core",
			(unsigned) fMean, (unsigned) (fMean * 100) % 100, CPlaitsVoice::EngineName (nWorst),
			(unsigned) fWorst, (unsigned) (fWorst * 100) % 100, (unsigned) (100.0 / fWorst));
	delete s_pBenchVoice;
	s_pBenchVoice = 0;
}

void CSynth::Reset (void)
{
	EnterCritical (IRQ_LEVEL);
	s_nFifoRd = s_nFifoWr = 0;
	s_nFaded = 0;
	s_nTonePhase = 0;
	LeaveCritical ();
}

static inline float Limit (float x)
{
	return x > kLimit ? kLimit : x < -kLimit ? -kLimit : x;
}

static void RenderBlock (void)
{
	if (s_bPendingTrig)
	{
		s_bPendingTrig = FALSE;
		s_pVoice->Trigger (s_nPendingNote);
		s_nTrigFifoIdx = s_nFifoWr;		// this block's first sample
		s_bTrigMark = TRUE;
		u64 nLat = CSynth::Ticks () - s_nPendingAt;
		s_nTrigs++;
		s_nTrigLatSum += nLat;
		if (nLat > s_nTrigLatMax) s_nTrigLatMax = nLat;
		int i = s_nLastLoggedTrig;
		if (i >= 0)
		{
			s_NoteLog[i].nLat = nLat;
			s_NoteLog[i].bRendered = TRUE;
			s_nLastLoggedTrig = -1;
		}
	}
	float Buf[CPlaitsVoice::BlockSize];
	u64 t0 = CSynth::Ticks ();
	s_pVoice->Render (Buf);
	u64 dt = CSynth::Ticks () - t0;
	s_nRenders++;
	s_nRenderSum += dt;
	if (dt > s_nRenderMax) s_nRenderMax = dt;

	float g = s_fVoiceGain * s_fLevel;
	for (unsigned i = 0; i < CPlaitsVoice::BlockSize; i++)
	{
		s_Fifo[s_nFifoWr++ % kFifo] = Buf[i] * g;
	}
}

int CSynth::Fill (s32 *pL, s32 *pR, unsigned nFrames)
{
	int nTrigFrame = -1;
	if (s_pVoice == 0)		// not initialised yet: silence
	{
		for (unsigned f = 0; f < nFrames; f++) pL[f] = pR[f] = 0;
		return -1;
	}
	if (s_Source != SourcePlaits && s_bPendingTrig)	// no voice: the note is only counted
	{
		s_bPendingTrig = FALSE;
		int i = s_nLastLoggedTrig;
		if (i >= 0) { s_NoteLog[i].bRendered = TRUE; s_nLastLoggedTrig = -1; }
	}
	if (s_Source == SourcePlaits)
	{
		while (s_nFifoWr - s_nFifoRd < nFrames) RenderBlock ();
	}
	for (unsigned f = 0; f < nFrames; f++)
	{
		float x = 0.0f;
		if (s_Source == SourcePlaits)
		{
			if (s_bTrigMark && s_nFifoRd == s_nTrigFifoIdx)
			{
				nTrigFrame = f;
				s_bTrigMark = FALSE;
			}
			x = s_Fifo[s_nFifoRd++ % kFifo];
		}
		else if (s_Source == SourceTone)
		{
			// spike2's triangle: 48 samples per period = 1 kHz, -18 dBFS
			unsigned ph = (s_nTonePhase++) % 48;
			int tri = ph < 24 ? (int) ph - 12 : 36 - (int) ph;
			x = tri * (0.125f / 12);
		}
		if (s_nFaded < kFadeFrames)
		{
			x *= (float) s_nFaded / kFadeFrames;
			s_nFaded++;
		}
		x = Limit (x);
		float a = x < 0 ? -x : x;
		if (a > s_fPeak) s_fPeak = a;
		s32 v = (s32) (x * 2147483648.0f);
		pL[f] = v;
		pR[f] = v;
	}
	unsigned nLevel = s_nFifoWr - s_nFifoRd;
	if (nLevel < s_nFifoMin) s_nFifoMin = nLevel;
	if (nLevel > s_nFifoMax) s_nFifoMax = nLevel;
	return nTrigFrame;
}

static void LogNote (u8 nStatus, u8 nNote, u8 nVel, boolean bTrig)
{
	unsigned n = s_nTotalNoteOn;
	if (!(n <= 8 || n % 16 == 0) && nStatus >= 0x90) return;
	if ((nStatus & 0xF0) == 0x80 && s_nTotalNoteOn > 8) return;
	if (s_nNoteLogWr - s_nNoteLogRd >= kNoteLog) { s_nNoteLogDropped++; return; }
	unsigned i = s_nNoteLogWr % kNoteLog;
	s_NoteLog[i].nAt = CSynth::Ticks ();
	s_NoteLog[i].nLat = 0;
	s_NoteLog[i].nStatus = nStatus;
	s_NoteLog[i].nNote = nNote;
	s_NoteLog[i].nVel = nVel;
	s_NoteLog[i].bRendered = FALSE;
	if (bTrig) s_nLastLoggedTrig = i;
	s_nNoteLogWr++;
}

void CSynth::MIDIPacket (unsigned nCable, const u8 *pPacket, unsigned nLength)
{
	s_nMIDIPackets++;
	s_nTotalPackets++;
	if (nLength == 0) return;
	u8 s = pPacket[0];
	if (s == 0xF8) { s_nClock++; return; }
	if (s == 0xFA || s == 0xFB || s == 0xFC) { s_nTransport++; return; }
	u8 t = s & 0xF0;
	if ((t == 0x90 || t == 0x80) && nLength >= 3)
	{
		unsigned ch = (s & 0x0F) + 1;
		if (s_nChannel != 0 && ch != s_nChannel) { s_nOtherCh++; return; }
		u8 nNote = pPacket[1] & 0x7F, nVel = pPacket[2] & 0x7F;
		if (t == 0x90 && nVel > 0)
		{
			s_nNoteOn++;
			s_nTotalNoteOn++;
			s_fLevel = nVel / 127.0f;
			s_nPendingNote = nNote;
			s_nPendingAt = Ticks ();
			s_bPendingTrig = TRUE;			// fires on the next rendered block
			CRytmObDevice::CaptureMarkMIDI ();
			LogNote (s, nNote, nVel, TRUE);
		}
		else
		{
			// Plaits' ping envelope decays by itself (as voiceloop's trig-driven voices)
			s_nNoteOff++;
			LogNote (s, nNote, nVel, FALSE);
		}
		return;
	}
	s_nOther++;
}

void CSynth::LogNotes (CLogger *pLogger)
{
	const u64 nRate = TickRate ();
	while (s_nNoteLogRd != s_nNoteLogWr)
	{
		unsigned i = s_nNoteLogRd % kNoteLog;
		TNoteEv e;
		EnterCritical (IRQ_LEVEL);
		e = s_NoteLog[i];
		LeaveCritical ();
		boolean bOn = (e.nStatus & 0xF0) == 0x90 && e.nVel > 0;
		if (bOn && !e.bRendered && (int) i == s_nLastLoggedTrig) break;	// wait for its render
		unsigned nAtUs = (unsigned) (e.nAt * 1000000 / nRate);
		if (bOn)
		{
			pLogger->Write (From, LogNotice, "midi: note on  ch %u note %u vel %u at %u us, rendered %u us later",
					(e.nStatus & 0x0F) + 1, e.nNote, e.nVel, nAtUs, (unsigned) (e.nLat * 1000000 / nRate));
		}
		else
		{
			pLogger->Write (From, LogNotice, "midi: note off ch %u note %u at %u us",
					(e.nStatus & 0x0F) + 1, e.nNote, nAtUs);
		}
		s_nNoteLogRd++;
	}
}

unsigned CSynth::TotalPackets (void)
{
	return s_nTotalPackets;
}

void CSynth::LogStats (CLogger *pLogger)
{
	EnterCritical (IRQ_LEVEL);
	unsigned nRenders = s_nRenders, nMin = s_nFifoMin, nMax = s_nFifoMax;
	u64 nSum = s_nRenderSum, nRMax = s_nRenderMax;
	unsigned nPk = s_nMIDIPackets, nClk = s_nClock, nTr = s_nTransport, nOn = s_nNoteOn, nOff = s_nNoteOff;
	unsigned nOtherCh = s_nOtherCh, nOther = s_nOther, nTrigs = s_nTrigs;
	u64 nLatMax = s_nTrigLatMax, nLatSum = s_nTrigLatSum;
	float fPeak = s_fPeak;
	s_nRenders = 0; s_nRenderSum = 0; s_nRenderMax = 0; s_nFifoMin = ~0u; s_nFifoMax = 0;
	s_nMIDIPackets = s_nClock = s_nTransport = s_nNoteOn = s_nNoteOff = s_nOtherCh = s_nOther = 0;
	s_nTrigs = 0; s_nTrigLatMax = 0; s_nTrigLatSum = 0;
	s_fPeak = 0;
	LeaveCritical ();

	const double fBlockTicks = (double) TickRate () * CPlaitsVoice::BlockSize / kRate;
	double fAvg = nRenders ? 100.0 * nSum / nRenders / fBlockTicks : 0;
	double fMax = 100.0 * nRMax / fBlockTicks;
	unsigned nMaxUs = (unsigned) (nRMax * 1000000 / TickRate ());
	unsigned nMaxNs = (unsigned) (nRMax * 1000000000ull / TickRate ()) % 1000;
	unsigned nLatAvg = nTrigs ? (unsigned) (nLatSum / nTrigs * 1000000 / TickRate ()) : 0;
	unsigned nLatMx = (unsigned) (nLatMax * 1000000 / TickRate ());
	int nPkDb = 0;		// peak in -dBFS (rounded down)
	for (float p = fPeak; p > 0 && p < 1.0f && nPkDb < 99; p *= 1.122018454f) nPkDb++;	// +1 dB
	if (fPeak <= 0) nPkDb = 99;

	pLogger->Write (From, LogNotice,
			"voice %u/s cpu %u.%02u%% max %u.%02u%% (%u.%03u us of 250) fifo %u-%u out pk -%u dB"
			" | midi %u/s clk %u tr %u on %u off %u och %u oth %u (total %u, on %u) trig lat %u/%u us",
			nRenders, (unsigned) fAvg, (unsigned) (fAvg * 100) % 100, (unsigned) fMax,
			(unsigned) (fMax * 100) % 100, nMaxUs, nMaxNs, nMin == ~0u ? 0 : nMin, nMax, nPkDb,
			nPk, nClk, nTr, nOn, nOff, nOtherCh, nOther, (unsigned) s_nTotalPackets,
			(unsigned) s_nTotalNoteOn, nLatAvg, nLatMx);
}

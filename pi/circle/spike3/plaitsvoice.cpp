//
// plaitsvoice.cpp - see plaitsvoice.h. Compiled with Plaits' own flags (Makefile: PLAITS_CPPFLAGS),
// no Circle headers.
//
#include "plaitsvoice.h"

#include <math.h>
#include <string.h>

#include "plaits/dsp/voice.h"
#include "stmlib/utils/buffer_allocator.h"

struct CPlaitsVoice::TImpl
{
	plaits::Voice Voice;
	plaits::Patch Patch;
	plaits::Modulations Mod;
	plaits::Voice::Frame Frames[plaits::kMaxBlockSize];
	char Shared[16384];		// Plaits' scratch (as voiceloop's shared[v])
	bool bTrig;
};

static const char *const s_Names[CPlaitsVoice::Engines] = {
	"VA VCF", "PHASE DIST", "6-OP FM 1", "6-OP FM 2", "6-OP FM 3", "WAVE TERRAIN",
	"STRING MACH", "CHIPTUNE", "VA", "WAVESHAPE", "2-OP FM", "GRANULAR", "HARMONIC",
	"WAVETABLE", "CHORDS", "SPEECH", "SWARM", "NOISE", "PARTICLE", "STRING", "MODAL",
	"BASS DRUM", "SNARE", "HI-HAT"};

CPlaitsVoice::CPlaitsVoice (void) : m_pImpl (new TImpl) {}

CPlaitsVoice::~CPlaitsVoice (void) { delete m_pImpl; }

const char *CPlaitsVoice::EngineName (unsigned nEngine)
{
	return nEngine < Engines ? s_Names[nEngine] : "?";
}

void CPlaitsVoice::Init (void)
{
	TImpl *p = m_pImpl;
	stmlib::BufferAllocator Allocator (p->Shared, sizeof p->Shared);
	p->Voice.Init (&Allocator);
	memset (&p->Patch, 0, sizeof p->Patch);
	memset (&p->Mod, 0, sizeof p->Mod);
	p->Patch.note = 48.0f;
	p->Patch.harmonics = 0.5f;
	p->Patch.timbre = 0.5f;
	p->Patch.morph = 0.5f;
	p->Patch.decay = 0.5f;
	p->Patch.lpg_colour = 0.5f;
	p->Mod.trigger_patched = true;		// voiceloop: Plaits' ping envelope per trig
	p->Mod.level_patched = false;
	p->Mod.level = 0.8f;
	p->bTrig = false;
}

void CPlaitsVoice::SetEngine (unsigned nEngine)
{
	m_pImpl->Patch.engine = nEngine < Engines ? nEngine : 0;
}

void CPlaitsVoice::SetParams (float fHarmonics, float fTimbre, float fMorph, float fDecay, float fColour)
{
	plaits::Patch &P = m_pImpl->Patch;
	P.harmonics = fHarmonics;
	P.timbre = fTimbre;
	P.morph = fMorph;
	P.decay = fDecay;
	P.lpg_colour = fColour;
}

void CPlaitsVoice::Trigger (float fNote)
{
	m_pImpl->Patch.note = fNote;
	m_pImpl->bTrig = true;
}

void CPlaitsVoice::SetNote (float fNote)
{
	m_pImpl->Patch.note = fNote;
}

void CPlaitsVoice::Render (float *pOut)
{
	TImpl *p = m_pImpl;
	p->Mod.trigger = p->bTrig ? 1.0f : 0.0f;
	p->bTrig = false;
	p->Voice.Render (p->Patch, p->Mod, p->Frames, plaits::kBlockSize);
	for (unsigned i = 0; i < plaits::kBlockSize; i++)
	{
		pOut[i] = p->Frames[i].out * (1.0f / 32768.0f);
	}
}

void CPlaitsVoice::Bench (unsigned nEngine, unsigned long long (*pTicks) (void),
			  unsigned long long *pSum, unsigned long long *pMax, unsigned *pBlocks)
{
	TImpl *p = m_pImpl;
	const int nBlocks = 2 * 48000 / plaits::kBlockSize;
	p->Patch.engine = nEngine;
	unsigned long long nSum = 0, nMax = 0;
	for (int b = 0; b < nBlocks; b++)
	{
		p->Mod.trigger = (b % 1000 == 0) ? 1.0f : 0.0f;	// every 0.25 s
		p->Patch.timbre = 0.5f + 0.4f * sinf (b * 0.001f);
		p->Patch.morph = 0.5f + 0.4f * cosf (b * 0.0013f);
		unsigned long long t0 = pTicks ();
		p->Voice.Render (p->Patch, p->Mod, p->Frames, plaits::kBlockSize);
		unsigned long long dt = pTicks () - t0;
		nSum += dt;
		if (dt > nMax) nMax = dt;
	}
	*pSum = nSum;
	*pMax = nMax;
	*pBlocks = nBlocks;
}

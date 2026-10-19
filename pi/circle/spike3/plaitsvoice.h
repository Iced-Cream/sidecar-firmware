//
// plaitsvoice.h - one Plaits voice behind a plain interface, so the Circle side never includes
// Plaits' (libstdc++) headers and Plaits never sees Circle's. plaitsvoice.cpp is built with the
// toolchain's C++ headers and newlib's libm (see the Makefile).
//
// Driven like teensy/voiceloop drives a PLAITS voice: trigger_patched (Plaits' own ping
// envelope / LPG, DECAY sets its length), level not patched (velocity is applied after the
// render, as voiceloop's output gain), patch.note = the MIDI note, 12-sample blocks.
//
#ifndef _plaitsvoice_h
#define _plaitsvoice_h

class CPlaitsVoice
{
public:
	static const unsigned BlockSize = 12;		// plaits::kBlockSize
	static const unsigned Engines = 24;

	CPlaitsVoice (void);
	~CPlaitsVoice (void);

	void Init (void);
	void SetEngine (unsigned nEngine);		// 0..23 (17, 21-23 are silent: not built, as on the Teensy)
	void SetParams (float fHarmonics, float fTimbre, float fMorph, float fDecay, float fColour);
	void Trigger (float fNote);			// fires on the next Render
	void SetNote (float fNote);

	// one block: BlockSize mono samples in -1..1 (Plaits' OUT, int16 full scale = 1)
	void Render (float *pOut);

	// plaits_bench's measurement: 2 s per engine, retrigger every 0.25 s, timbre/morph moving;
	// pTicks(): a free-running counter; returns the sum and the largest of the per-block counts
	void Bench (unsigned nEngine, unsigned long long (*pTicks) (void),
		    unsigned long long *pSum, unsigned long long *pMax, unsigned *pBlocks);

	static const char *EngineName (unsigned nEngine);

private:
	struct TImpl;
	TImpl *m_pImpl;
};

#endif

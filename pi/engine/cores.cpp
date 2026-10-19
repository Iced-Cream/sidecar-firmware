// pi/engine/cores.cpp - see cores.h. The block handshake: core 3 bumps s_nGen and signals (sev); cores 1 and 2
// render their halves of s_Block and publish s_nDone; core 3 waits (spinning, with a 20 ms limit), then pushes the
// block into the output stage. Nothing else touches the engine core while a block renders.
#include "cores.h"

#include <circle/logger.h>
#include <circle/synchronize.h>
#include <circle/util.h>

#include "bridge.h"
#include "guard.h"
#include "midi_in.h"
#include "notes.h"
#include "sendfx_pi.h"
#include "rec_pi.h"
#include "shared.h"
#include "user/lists.h"
#include "user/um.h"

static const char From[] = "cores";

static inline u64 Ticks (void)
{
	u64 t;
	asm volatile ("isb; mrs %0, cntpct_el0" : "=r" (t) :: "memory");
	return t;
}
static inline u64 TickRate (void)
{
	u64 f;
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (f));
	return f;
}

static s16 s_Block[bridge::kBlock][bridge::kVoices];
static volatile u32 s_nGen;
static volatile u32 s_nDone[3];
static u64 s_Mach[3][bridge::kMachines];
static volatile u64 s_nBusy[4];
static volatile unsigned s_nBlocks, s_nMaxBlock, s_nMaxPass, s_nFifoMin = ~0u, s_nFifoMax, s_nMidiBytes, s_nNotes;
static volatile unsigned s_nNoteChans;	// bit c: a note-on on MIDI channel c+1 this second
static u64 s_nLastTake;
static unsigned s_nTarget = 96;
static volatile boolean s_bTargetFixed = FALSE;	// a fifo= set it: the link's default does not
static boolean s_bNotes = TRUE;
static boolean s_bReportLog = FALSE;
static int s_nDrone = -1;			// -1 off, 0..7 one voice, 8 all
static unsigned s_nSplit = 4;			// voices 0..s_nSplit-1 on core 1, the rest on core 2
static volatile u32 s_nBenchTurn = 4;		// the core whose bench runs now (4: none, every core done)
static CEngineCores::TBench s_Bench[4][CEngineCores::BenchMachines];
static u64 s_nDroneAt;

static void ToCore (const u8 *pData, int nLength)
{
	if (s_bReportLog && nLength >= 23 && (pData[4] == 0x04 || pData[4] == 0x03))
	{
		const u8 *p = pData + (pData[4] == 0x04 ? 8 : 6);
		CLogger::Get ()->Write (From, LogNotice, "report %02x voice %u flags %02x P1..P4 %u %u %u %u", pData[4],
					pData[5] & 7, pData[5] & 0x78, p[0], p[2], p[4], p[6]);
	}
	bridge::sysex (pData, nLength);
}
static core::NoteAdapter s_Notes (ToCore);
// A Rytm running the mods (any F0 7D 52 54 from it) reports its voices itself (0021's 03 / 04, 0026's 33): the note
// adapter (a stock Rytm's notes on channels 1..8 as PLAITS reports) then steps aside, or each MIDI note the Rytm
// sends out over USB turned the voice it just set to JUNO / DSP SAMPLE / ... back into the adapter's PLAITS (the
// sound, and the voice pages' waveform / GRAIN view, which read the voice's state).
static volatile boolean s_bModded = FALSE;
static void FromRytm (const u8 *pData, int nLength)
{
	if (nLength >= 5 && pData[1] == 0x7D && pData[2] == 0x52 && pData[3] == 0x54) s_bModded = TRUE;
	ToCore (pData, nLength);
}
static void OnChannel (u8 s, u8 a, u8 b)
{
	if ((s & 0xF0) == 0x90 && b) { s_nNotes++; s_nNoteChans |= 1u << (s & 0x0F); }
	bridge::lfo_midi (s, a, b);		// the LFOs: a Rytm track's trig (an FX target's TRIG, core/lfo.h)
	if (s_bNotes && !s_bModded) s_Notes.Channel (s, a, b);
	else if ((s & 0xF0) == 0xB0 && a == 120) sendfx_pi::AllSoundOff ();	// the send FX hear it with the notes off too
}
static MidiIn s_Midi (FromRytm, OnChannel, rec_pi::Transport);	// PLAY/STOP: an armed recorder; clock / SPP: STUTTER's grid

static unsigned ClampTarget (unsigned n) { return n < 24 ? 24 : n > 1024 ? 1024 : n; }

boolean CEngineCores::NotesOn (void)	{ return s_bNotes; }
int CEngineCores::DroneVoice (void)		{ return s_nDrone; }

void CEngineCores::Configure (unsigned nFifoTarget, boolean bNotes)
{
	s_nTarget = ClampTarget (nFifoTarget);
	s_bNotes = bNotes;
	if (s_nLastTake == 0) s_nLastTake = Ticks ();	// the first stats line's interval: from the boot's Configure
}

unsigned CEngineCores::FifoTarget (void) { return s_nTarget; }


// core 0 (UseLink): a single aligned word, read by core 3 each pass
void CEngineCores::LinkFifoTarget (unsigned n)
{
	if (!s_bTargetFixed) s_nTarget = ClampTarget (n);
}

void CEngineCores::FixFifoTarget (void) { s_bTargetFixed = TRUE; }

void CEngineCores::SetSplit (unsigned n) { s_nSplit = n < 1 ? 1 : n > 7 ? 7 : n; }

const char *const CEngineCores::BenchMachine[BenchMachines] = {"plaits", "juno", "fm", "daisy", "dsps", "dd"};

void CEngineCores::BenchCore (unsigned nCore)
{
	for (unsigned m = 0; m < BenchMachines; m++)
	{
		s_Bench[nCore][m].nTicks = bridge::bench (BenchMachine[m], &s_Bench[nCore][m].nWorst,
						       &s_Bench[nCore][m].nPeak);
	}
}

void CEngineCores::BenchStart (boolean bOn)
{
	__atomic_store_n (&s_nBenchTurn, bOn ? 1 : 4, __ATOMIC_RELEASE);
}

boolean CEngineCores::BenchWait (unsigned nMs)
{
	u64 t0 = Ticks (), nLimit = TickRate () / 1000 * nMs;
	while (__atomic_load_n (&s_nBenchTurn, __ATOMIC_ACQUIRE) != 4)
	{
		if (Ticks () - t0 > nLimit) return FALSE;
	}
	return TRUE;
}

unsigned CEngineCores::BenchTurn (void) { return __atomic_load_n (&s_nBenchTurn, __ATOMIC_ACQUIRE); }

const CEngineCores::TBench &CEngineCores::BenchResult (unsigned nCore, unsigned nMachine)
{
	return s_Bench[nCore & 3][nMachine % BenchMachines];
}

void CEngineCores::Run (unsigned nCore)
{
	// the boot bench: cores 1, 2, 3 in turn (core 0 benched before they started), then each core's loop
	while (__atomic_load_n (&s_nBenchTurn, __ATOMIC_ACQUIRE) < nCore) asm volatile ("wfe");
	if (__atomic_load_n (&s_nBenchTurn, __ATOMIC_ACQUIRE) == nCore)
	{
		BenchCore (nCore);
		__atomic_store_n (&s_nBenchTurn, nCore + 1, __ATOMIC_RELEASE);
		asm volatile ("dsb sy; sev");
	}
	if (nCore == 1 || nCore == 2) Worker (nCore);
	else if (nCore == 3) Control ();
	// core 0: the kernel's own loop
}

void CEngineCores::Worker (unsigned nCore)
{
	const unsigned v0 = nCore == 1 ? 0 : s_nSplit, v1 = nCore == 1 ? s_nSplit : bridge::kVoices;
	// static: they survive the guard's longjmp
	static volatile u32 s_nSeen[3];		// the last block generation this core finished
	static volatile u32 s_nGenNow[3];	// the one it renders
	static volatile unsigned s_nResume[3];	// after a fault: the voice to go on with (0: none)
	static volatile u64 s_nT0[3];		// the block's start, for the busy time
	CLogger::Get ()->Write (From, LogNotice, "core %u up: renders voices %u-%u", nCore, v0, v1 - 1);
	if (__builtin_setjmp (g_Guard.JumpBuffer (nCore)))
	{
		// a fault in a voice's render (guard.cpp returned from the exception into here, at task level): that
		// voice is muted and silent in this block; the block's voices before it keep what they rendered, the
		// ones after it render below
		unsigned v = g_Guard.Recover (nCore);
		for (unsigned s = 0; s < bridge::kBlock; s++) s_Block[s][v] = 0;
		u64 nSPSel;
		asm volatile ("mrs %0, spsel" : "=r" (nSPSel));
		g_Guard.NoteResumed (nCore, CurrentExecutionLevel () == TASK_LEVEL && nSPSel == 0);
		s_nResume[nCore] = v + 1;
	}
	for (;;)
	{
		u32 nGen;
		unsigned vFirst;
		if (s_nResume[nCore])
		{
			nGen = s_nGenNow[nCore];
			vFirst = s_nResume[nCore];
			s_nResume[nCore] = 0;
		}
		else
		{
			while ((nGen = __atomic_load_n (&s_nGen, __ATOMIC_ACQUIRE)) == s_nSeen[nCore]) asm volatile ("wfe");
			s_nGenNow[nCore] = nGen;
			s_nT0[nCore] = Ticks ();
			vFirst = v0;
		}
		for (unsigned v = vFirst; v < v1; v++)
		{
			if (g_Guard.Muted (v))
			{
				for (unsigned s = 0; s < bridge::kBlock; s++) s_Block[s][v] = 0;
				continue;
			}
			u64 t0 = Ticks ();
			g_Guard.Enter (nCore, v);
			bridge::render (s_Block, v, v + 1, s_Mach[nCore]);
			g_Guard.Leave (nCore, v, (u32) (Ticks () - t0));
		}
		s_nBusy[nCore] += Ticks () - s_nT0[nCore];
		s_nSeen[nCore] = nGen;
		__atomic_store_n (&s_nDone[nCore], nGen, __ATOMIC_RELEASE);
		asm volatile ("dsb sy; sev");
		if (nCore == 2) sendfx_pi::Render ();	// the send FX, after the block (sendfx_pi.h)
	}
}

void CEngineCores::RenderBlock (void)
{
	u32 nGen = s_nGen + 1;
	__atomic_store_n (&s_nGen, nGen, __ATOMIC_RELEASE);
	asm volatile ("dsb sy; sev");
	u64 t0 = Ticks (), nLimit = TickRate () / 50;		// 20 ms
	while (__atomic_load_n (&s_nDone[1], __ATOMIC_ACQUIRE) != nGen
	       || __atomic_load_n (&s_nDone[2], __ATOMIC_ACQUIRE) != nGen)
	{
		if (Ticks () - t0 > nLimit)
		{
			g_Guard.Fatal ("a render core did not finish its block in 20 ms");
			for (;;) asm volatile ("wfe");		// core 0 stops the stream and reboots
		}
	}
	unsigned nUs = (unsigned) ((Ticks () - t0) * 1000000 / TickRate ());
	if (nUs > s_nMaxBlock) s_nMaxBlock = nUs;
	g_Output.Push (s_Block, bridge::kBlock);
	s_nBlocks++;
}

// The engine's own notes for tests (:drone=V or all): every 0.5 s a note-off and a note-on (note 48 + 2v) on the
// voice(s), through the note adapter (each voice's machine as :notemach set it).
static void Drone (void)
{
	u64 now = Ticks ();
	if (s_nDrone < 0 || now - s_nDroneAt < TickRate () / 2) return;
	s_nDroneAt = now;
	for (int v = 0; v < 8; v++)
	{
		if (s_nDrone != 8 && s_nDrone != v) continue;
		s_Notes.Channel (0x80 | v, 48 + 2 * v, 0);
		s_Notes.Channel (0x90 | v, 48 + 2 * v, 100);
	}
}

void CEngineCores::Control (void)
{
	// The generic timer's event stream on this core: an event every 2^13 counter ticks (152 us at the Pi 4's
	// 54 MHz), so the wfe below never sleeps longer than that. A sev from core 0 (a pull from the FIFO) can be
	// consumed by a wfe inside this core's own work (a spinlock: the logger, the card), and the next sev may be
	// a poll of core 0 away (1.2..3.3 ms measured on the TCP link): the FIFO then stayed empty and the link sent
	// silence (docs/pi-engine.md "FIFO target").
	u64 nKctl;
	asm volatile ("mrs %0, cntkctl_el1" : "=r" (nKctl));
	nKctl = (nKctl & ~(u64) 0xF0) | (12 << 4) | (1 << 2);	// EVNTI 12 (an event as bit 12 rises: every 8192 ticks), EVNTEN
	asm volatile ("msr cntkctl_el1, %0; isb" : : "r" (nKctl));
	CLogger::Get ()->Write (From, LogNotice, "core 3 up: control (fifo target %u frames, MIDI notes %s)", s_nTarget,
				s_bNotes ? "on" : "off");
	for (;;)
	{
		u64 t0 = Ticks ();
		boolean bWorked = FALSE;
		u8 b;
		while (g_MidiIn.pop (&b))
		{
			s_Midi.Feed (b);
			s_nMidiBytes++;
			bWorked = TRUE;
		}
		TCmd Cmd;
		while (g_Cmds.pop (&Cmd))
		{
			Command (Cmd.text);
			bWorked = TRUE;
		}
		Drone ();
		bridge::tick ();
		unsigned nLevel = g_Output.Level ();
		if (nLevel < s_nFifoMin) s_nFifoMin = nLevel;
		if (nLevel > s_nFifoMax) s_nFifoMax = nLevel;
		while (g_Output.Level () + bridge::kBlock <= s_nTarget)
		{
			RenderBlock ();
			bWorked = TRUE;
		}
		if (bridge::idle_prepare () || bridge::idle_save ()) bWorked = TRUE;
		else
		{
			bridge::idle_stream ();
			if (um::idle ()) bWorked = TRUE;	// the user machines: one bounded step (an 8 KB read at most)
			lists::idle ();			// the pool / lists JSON for the web page, when it changed (<= 4 a second)
		}
		if (bWorked)
		{
			u64 dt = Ticks () - t0;
			s_nBusy[3] += dt;
			unsigned nUs = (unsigned) (dt * 1000000 / TickRate ());	// the longest pass: the FIFO's refill waits that long
			if (nUs > s_nMaxPass) s_nMaxPass = nUs;
		}
		else asm volatile ("wfe");	// woken by the OUT fill, a MIDI byte or a command (sev)
	}
}

// The console's commands (console.cpp: ':' lines), run here so the engine core is touched by one core only.
void CEngineCores::Command (const char *p)
{
	CLogger *L = CLogger::Get ();
	if (strncmp (p, "notemach=", 9) == 0 && p[9] >= '0' && p[9] <= '7' && p[10] == ':')
	{
		int v = p[9] - '0';
		if (s_Notes.SetMachine (v, p + 11)) L->Write (From, LogNotice, "notes: voice %d plays %s", v, p + 11);
		else L->Write (From, LogWarning, "notes: no machine \"%s\" (plaits juno daisy dsps dd fm)", p + 11);
	}
	else if (strncmp (p, "drone=", 6) == 0)
	{
		s_nDrone = strcmp (p + 6, "all") == 0 ? 8 : strcmp (p + 6, "off") == 0 ? -1 : atoi (p + 6);
		if (s_nDrone > 8) s_nDrone = -1;
		L->Write (From, LogNotice, "drone %s", p + 6);
	}
	else if (strncmp (p, "notes=", 6) == 0)
	{
		s_bNotes = p[6] == '1';
		L->Write (From, LogNotice, "MIDI notes %s%s", s_bNotes ? "on" : "off", s_bNotes && s_bModded ? " (idle: the Rytm runs the mods and reports its voices)" : "");
	}
	else if (strncmp (p, "out=", 4) == 0)
	{
		g_Output.SetMode (strcmp (p + 4, "tracks") == 0 ? Output::kTracks : Output::kMain);
		L->Write (From, LogNotice, "output: %s", g_Output.GetMode () == Output::kTracks ? "tracks" : "main");
	}
	else if (strncmp (p, "fifo=", 5) == 0)
	{
		Configure (atoi (p + 5), s_bNotes);
		s_bTargetFixed = TRUE;		// kept across link switches
		L->Write (From, LogNotice, "fifo target %u frames", s_nTarget);
	}
	else if (strcmp (p, "unmute") == 0)
	{
		g_Guard.Unmute ();
		L->Write (From, LogNotice, "guard: every voice unmuted");
	}
	else if (strncmp (p, "faulttest=", 10) == 0)
	{
		g_Guard.ArmFault (atoi (p + 10) & 7);
		L->Write (From, LogNotice, "guard: voice %d faults on its next render", atoi (p + 10) & 7);
	}
	else if (strncmp (p, "overtest=", 9) == 0)
	{
		g_Guard.ArmOverrun (atoi (p + 9) & 7, 5);
		L->Write (From, LogNotice, "guard: voice %d runs 300 us long for 5 blocks", atoi (p + 9) & 7);
	}
	else if (strncmp (p, "hangtest=", 9) == 0)
	{
		g_Guard.ArmHang (atoi (p + 9) & 7);
		L->Write (From, LogNotice, "guard: voice %d runs 30 ms long once (fatal: clean stop, reboot)", atoi (p + 9) & 7);
	}
	else if (strncmp (p, "pool=", 5) == 0 || strncmp (p, "juno=", 5) == 0 || strncmp (p, "fm=", 3) == 0)
	{
		char Out[160];		// the pool and the JUNO / FM lists (core/user/lists.h)
		lists::command (p, Out, sizeof Out);
		L->Write (From, LogNotice, "%s", Out);
	}
	else if (strncmp (p, "reportlog=", 10) == 0)
	{
		s_bReportLog = p[10] == '1';
		L->Write (From, LogNotice, "report log %s", s_bReportLog ? "on" : "off");
	}
	else if (char Fx[120]; bridge::fxin_command (p, Fx, sizeof Fx))	// fxin, fxin=1|0|?: IN L/R source (core/host_link.h)
	{
		L->Write (From, LogNotice, "%s", Fx);
	}
	else if (char Lo[200]; bridge::lfo_command (p, Lo, sizeof Lo))	// lfo, lfo=N:KEY:VALUE: the LFOs (core/lfo.h)
	{
		L->Write (From, LogNotice, "%s", Lo);
	}
	else if (sendfx_pi::Command (p))		// sendfx, sendfxout=, sendlvl=, sendknob= (sendfx_pi.h)
	{
	}
	else if (strncmp (p, "um", 2) == 0 && (p[2] == 0 || p[2] == '='))	// the user machines (core/user/um.h)
	{
		char Out[120];
		if (um::command (p, Out, sizeof Out)) L->Write (From, LogNotice, "um: %s", Out);
		else L->Write (From, LogWarning, "unknown command \"%s\" (um, um=rescan, um=assign:S:ID, um=clear:S, um=save:S:NAME, um=presets)", p);
	}
	else if (bridge::rui_command (p))		// rui, rui=on|off: the remote UI (core/ui/remote.h)
	{
	}
	else
	{
		L->Write (From, LogWarning, "unknown command \"%s\"", p);
	}
}

void CEngineCores::TakeStats (TStats *s)
{
	u64 now = Ticks ();
	s->nTicks = now > s_nLastTake ? now - s_nLastTake : 1;
	s_nLastTake = now;
	for (int c = 0; c < 4; c++)
	{
		s->nBusy[c] = s_nBusy[c];
		s_nBusy[c] = 0;
	}
	for (int k = 0; k < bridge::kMachines; k++)
	{
		s->nMach[k] = s_Mach[1][k] + s_Mach[2][k];
		s_Mach[1][k] = s_Mach[2][k] = 0;
	}
	s->nBlocks = s_nBlocks; s_nBlocks = 0;
	s->nMaxBlockUs = s_nMaxBlock; s_nMaxBlock = 0;
	s->nMaxPassUs = s_nMaxPass; s_nMaxPass = 0;
	if (s_nFifoMin == ~0u)		// no control-loop pass this second: the level now
	{
		s->nFifoMin = s->nFifoMax = g_Output.Level ();
	}
	else
	{
		s->nFifoMin = s_nFifoMin;
		s->nFifoMax = s_nFifoMax;
	}
	s_nFifoMin = ~0u; s_nFifoMax = 0;
	s->nMidiBytes = s_nMidiBytes; s_nMidiBytes = 0;
	s->nSysEx = s_Midi.SysExCount ();
	s->nNotes = s_nNotes; s_nNotes = 0;
	s->nNoteChans = s_nNoteChans; s_nNoteChans = 0;
}

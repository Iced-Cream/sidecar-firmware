// pi/engine/cores.cpp - cores.h. Nothing renders: cores 1 and 2 start and sleep; core 3 runs the control loop - the
// Rytm's MIDI bytes into the host link's router (core/generic: the hello, the clock, the screen, STORE, the card browse), the USB-A MIDI devices (core/devmidi), the console's
// commands, the generic tick (the remote screen), the pairing code's popup and the card's idle saves. The audio
// return's processor slots run on core 0 in the OUT fill (audio_return.h).
#include "cores.h"
#include "crumb.h"

#include <circle/logger.h>
#include <circle/synchronize.h>
#include <circle/util.h>

#include "bridge.h"
#include "idlewait.h"
#include "midi_in.h"
#include "paircode_pi.h"
#include "shared.h"
#include "stor_pi.h"

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

static volatile u64 s_nBusy[4];
static volatile u64 s_nSleep[4];
static volatile unsigned s_nWakes[4];
static volatile boolean s_bIdleWfe = TRUE;
static volatile unsigned s_nMaxPass, s_nMidiBytes, s_nNotes;
static u64 s_nLastTake;
static unsigned s_nTarget = 96;			// the links' FIFO target: kept for /status (no FIFO is filled here)
static volatile boolean s_bTargetFixed = FALSE;
static volatile boolean s_bPoll;

// The SysEx messages from the Rytm that took core 3 over 1 ms
static volatile unsigned s_SlowSx[CEngineCores::kSlowSx][4], s_nSlowSx;
unsigned CEngineCores::SlowSysex (TSlowSx *p)
{
	unsigned n = s_nSlowSx, k = n < kSlowSx ? n : kSlowSx;
	for (unsigned i = 0; i < k; i++)
	{
		const volatile unsigned *r = s_SlowSx[(n - k + i) % kSlowSx];
		p[i].cmd = r[0], p[i].arg = r[1], p[i].len = r[2], p[i].us = r[3];
	}
	return k;
}

static void FromRytm (const u8 *pData, int nLength)
{
	u64 t0 = Ticks ();
	bridge::sysex (pData, nLength);
	unsigned nUs = (unsigned) ((Ticks () - t0) * 1000000 / TickRate ());
	if (nUs > 1000)
	{
		volatile unsigned *r = s_SlowSx[s_nSlowSx % CEngineCores::kSlowSx];
		r[0] = nLength > 4 ? pData[4] : 0, r[1] = nLength > 5 ? pData[5] : 0, r[2] = (unsigned) nLength, r[3] = nUs;
		s_nSlowSx++;
	}
}
// The Rytm's channel messages: counted for the stats line (nothing in this engine plays them)
static void OnChannel (u8 s, u8 a, u8 b)
{
	if ((s & 0xF0) == 0x90 && b) s_nNotes++;
}
// The Rytm's realtime bytes: playing or not, for the storage bench's refusal (stor_pi.h)
static void OnRealtime (uint8_t s, uint16_t) { stor_pi::Transport (s); }
static MidiIn s_Midi (FromRytm, OnChannel, OnRealtime);
// The USB-A MIDI devices (core/devmidi.h): their messages from g_KeysIn (source, status, data; F4 a plug or unplug, its
// name in g_KeyNames), what goes on to the Rytm one USB MIDI event packet each into g_KeysToRytm (core 0's transport
// sends them between whole messages; a full queue drops it)
static volatile unsigned s_nToRytmLost;
static void DevToRytm (u8 s, u8 a, u8 b)
{
	u32 w = (u32) (s >> 4) | (u32) s << 8 | (u32) a << 16 | (u32) b << 24;
	if (!g_KeysToRytm.push (w)) s_nToRytmLost++;
}
static void DevIn (u32 w)
{
	int src = (int) (w >> 24);
	u8 s = (u8) (w >> 16), a = (u8) (w >> 8), b = (u8) w;
	if (s == 0xF4) bridge::devmidi_plugged (src, a != 0, src < 4 ? g_KeyNames[src] : "");
	else bridge::devmidi (src, s, a, b);
}

static volatile CEngineCores::TStall s_Stall;
CEngineCores::TStall CEngineCores::Stall (void)
{
	TStall s;
	for (unsigned i = 0; i < sizeof s / sizeof (unsigned); i++) ((unsigned *) &s)[i] = ((volatile unsigned *) &s_Stall)[i];
	return s;
}
const char *CEngineCores::StallStep (void) { return ""; }
boolean CEngineCores::WaitPolled (void) { return s_bPoll; }
unsigned CEngineCores::Gaps (TGap *) { return 0; }

static inline void Wfe (unsigned nCore)
{
	u64 t = Ticks ();
	asm volatile ("wfe");
	s_nSleep[nCore] += Ticks () - t;
	s_nWakes[nCore]++;
}
void CEngineCores::Sleep (void)
{
	u64 t = Ticks ();
	idlewait::nap ([] { asm volatile ("sevl"); }, [] { asm volatile ("wfe"); });
	s_nSleep[0] += Ticks () - t;
	s_nWakes[0]++;
}
void CEngineCores::SetIdleWfe (boolean bOn) { s_bIdleWfe = bOn; }
boolean CEngineCores::IdleWfe (void) { return s_bIdleWfe; }

void CEngineCores::EventStream (void)
{
	u64 nKctl;
	asm volatile ("mrs %0, cntkctl_el1" : "=r" (nKctl));
	nKctl = (nKctl & ~(u64) 0xF0) | (12 << 4) | (1 << 2);	// an event every 8192 ticks (152 us), EVNTEN
	asm volatile ("msr cntkctl_el1, %0; isb" : : "r" (nKctl));
}

static unsigned ClampTarget (unsigned n) { return n < 24 ? 24 : n > 1024 ? 1024 : n; }
void CEngineCores::Configure (unsigned nFifoTarget)
{
	s_nTarget = ClampTarget (nFifoTarget);
	if (s_nLastTake == 0) s_nLastTake = Ticks ();
}
unsigned CEngineCores::FifoTarget (void) { return s_nTarget; }
void CEngineCores::LinkFifoTarget (unsigned n) { if (!s_bTargetFixed) s_nTarget = ClampTarget (n); }
void CEngineCores::FixFifoTarget (void) { s_bTargetFixed = TRUE; }

void CEngineCores::Run (unsigned nCore)
{
	if (nCore == 1 || nCore == 2) Worker (nCore);
	else if (nCore == 3) Control ();
}

// Cores 1 and 2: nothing to render in this engine. They sleep in wfe (woken by the other cores' events, at once
// asleep again), so a processor slot that needs a core of its own can be given one later.
void CEngineCores::Worker (unsigned nCore)
{
	CLogger::Get ()->Write (From, LogNotice, "core %u up: idle (the generic engine renders no voices)", nCore);
	for (;;)
	{
		crumb::BeatEnter (nCore, 201);
		Wfe (nCore);
	}
}

void CEngineCores::Control (void)
{
	EventStream ();		// the wfe below never sleeps longer than 152 us
	CLogger::Get ()->Write (From, LogNotice, "core 3 up: control (the host link's generic services)");
	bridge::devmidi_set_forward (DevToRytm);
	u64 nPrevT0 = 0;
	unsigned nSec[6] = {};
	for (;;)
	{
		u64 t0 = Ticks ();
		crumb::Beat (3);
		crumb::Enter (100);
		{	// the longest gap between two passes, blamed on the last pass's sections (midi, -, cmds, tick, -, idle)
			unsigned nGap = nPrevT0 ? (unsigned) ((t0 - nPrevT0) * 1000000 / TickRate ()) : 0;
			if (nGap > 2000) s_Stall.gaps_over_2ms++;
			if (nGap > s_Stall.gap_us)
			{
				s_Stall.gap_us = nGap;
				for (unsigned i = 0; i < 6; i++) s_Stall.sec_us[i] = nSec[i];
			}
			nPrevT0 = t0;
			for (unsigned i = 0; i < 6; i++) nSec[i] = 0;
		}
		u64 tS = t0;
		auto Mark = [&] (unsigned i) { u64 t = Ticks (); nSec[i] = (unsigned) ((t - tS) * 1000000 / TickRate ()); tS = t; crumb::Enter (101 + i); };
		boolean bWorked = FALSE;
		u8 b;
		while (g_MidiIn.pop (&b))
		{
			s_Midi.Feed (b);
			s_nMidiBytes++;
			bWorked = TRUE;
		}
		Mark (0);
		u32 nKey;
		while (g_KeysIn.pop (&nKey))
		{
			DevIn (nKey);
			bWorked = TRUE;
		}
		Mark (1);
		TCmd Cmd;
		while (g_Cmds.pop (&Cmd))
		{
			Command (Cmd.text);
			bWorked = TRUE;
		}
		Mark (2);
		if (g_bRytmGone) { g_bRytmGone = false; bridge::rytm_gone (); }
		if (g_bRytmResync) { g_bRytmResync = false; bridge::rytm_resync (); }
		bridge::tick ();
		// the pairing code on the Rytm's screen when it was asked for (paircode_pi.h): the screen is this core's
		char PairLine[24];
		static uint32_t s_nHellos = 0;			// a new hello (a connect): the code on the Rytm again
		if (bridge::rytm_hellos () != s_nHellos)
		{
			s_nHellos = bridge::rytm_hellos ();
			paircode_pi::RytmHello ();
		}
		if (paircode_pi::TakeHide ()) bridge::pair_popup_end ();
		if (paircode_pi::TakeShow (PairLine, sizeof PairLine))
		{
			if (bridge::pair_popup (PairLine)) paircode_pi::ShowDone ();
			else if (bridge::rytm_greeted ()) paircode_pi::ShowFailed ();
			memset (PairLine, 0, sizeof PairLine);
		}
		Mark (3);
		Mark (4);
		if (bridge::idle_save ()) bWorked = TRUE;
		Mark (5);
		if (bWorked)
		{
			u64 dt = Ticks () - t0;
			s_nBusy[3] += dt;
			unsigned nUs = (unsigned) (dt * 1000000 / TickRate ());
			if (nUs > s_nMaxPass) s_nMaxPass = nUs;
		}
		else if (s_bPoll) for (u64 u = Ticks (); Ticks () - u < TickRate () / 20000;) {}
		else Wfe (3);			// woken by a MIDI byte or a command (sev), or the event stream
	}
}

// The console's ':' commands, run here (the core's services are this core's)
void CEngineCores::Command (const char *p)
{
	CLogger *L = CLogger::Get ();
	char Out[240];
	if (bridge::generic_command (p, Out, sizeof Out)) L->Write (From, LogNotice, "%s", Out);
	else if (strncmp (p, "fifo=", 5) == 0)
	{
		Configure (atoi (p + 5));
		s_bTargetFixed = TRUE;
		L->Write (From, LogNotice, "fifo target %u frames (the TCP link's pacing)", s_nTarget);
	}
	else if (strncmp (p, "ctlwait=", 8) == 0)
	{
		s_bPoll = strcmp (p + 8, "poll") == 0;
		L->Write (From, LogNotice, "control loop waits by %s", s_bPoll ? "polling" : "wfe");
	}
	else if (strncmp (p, "idlewfe=", 8) == 0)
	{
		s_bIdleWfe = p[8] != '0';
		L->Write (From, LogNotice, "idle wfe %s", s_bIdleWfe ? "on" : "off");
	}
	else L->Write (From, LogWarning, "unknown command \"%s\" (gen, clock, kv, proc, rui, rui=on|off, usba, usba=thru|off, browse, fifo=, ctlwait=, idlewfe=)", p);
}

void CEngineCores::TakeStats (TStats *s)
{
	memset (s, 0, sizeof *s);
	u64 now = Ticks ();
	s->nTicks = now > s_nLastTake ? now - s_nLastTake : 1;
	s_nLastTake = now;
	for (int c = 0; c < 4; c++)
	{
		s->nBusy[c] = s_nBusy[c], s_nBusy[c] = 0;
		s->nSleep[c] = s_nSleep[c], s_nSleep[c] = 0;
		s->nWakes[c] = s_nWakes[c], s_nWakes[c] = 0;
	}
	s->nMaxPassUs = s_nMaxPass; s_nMaxPass = 0;
	s->nMidiBytes = s_nMidiBytes; s_nMidiBytes = 0;
	s->nSysEx = s_Midi.SysExCount ();
	s->nNotes = s_nNotes; s_nNotes = 0;
}

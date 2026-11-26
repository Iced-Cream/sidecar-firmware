// pi/engine/tests/test_remote_views.cpp - the remote UI's views with no display (remote_views_pi.cpp, the kernel's view section):
// a fake engine, the real service (remote_srv.cpp) and the real client (ui/remote/rmt_cli.c) over an in-memory link.
// With no Display every subscribed view reaches the client, an edit sent as a command lands and comes back, and nothing is asked of
// a screen: this binary links no LVGL and no sidecar_*_set, so a call into the display's setters could not even link. With a Display
// the local pages are fed as before (only the page that shows), and the client gets the same builds. Both at two clock starts (the
// engine's microsecond clock wraps).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "check.h"
#include "remote_srv.h"
#include "remote_views_pi.h"
#include "../../ui/remote/rmt_cli.h"

using namespace remote;

struct Pipe : Conn
{
	std::vector<uint8_t> c2s, s2c;
	int Recv (uint8_t *p, size_t n) override
	{
		size_t k = c2s.size () < n ? c2s.size () : n;
		memcpy (p, c2s.data (), k);
		c2s.erase (c2s.begin (), c2s.begin () + (long) k);
		return (int) k;
	}
	int Send (const uint8_t *p, size_t n) override { s2c.insert (s2c.end (), p, p + n); return (int) n; }
};

// ---- the fake engine ("core 3" and the rest): the state the views are built from ---------------------------------------------

struct FakeEngine : remote_views::Engine
{
	uint32_t startUs = 0, nowMs = 0;
	std::string project = "SHARED SET";
	// the sequencer, as core 3 keeps it
	sidecar_seq_view_t seq;
	bool seqNew = false;
	int playMs = -1;
	unsigned showReq = 0;
	// the Rytm pattern
	rytm_pattern_t pat;
	bool patNew = false;
	int patIndex = 5;
	std::string where = "from the Rytm";
	int wantPage = -1;
	// counters: what was built
	int nSeq = 0, nPat = 0, nVoice = 0, nStatus = 0, nIns = 0, nMfx = 0, nWhere = 0;
	const rytm_pattern_t *voicePat = 0;
	int voiceIdx = -99;
	sidecar_status_view_t status;
	FakeEngine () { memset (&seq, 0, sizeof seq); memset (&pat, 0, sizeof pat); memset (&status, 0, sizeof status); }
	unsigned NowUs () override { return (unsigned) (startUs + nowMs * 1000u); }
	const char *Project () override { return project.c_str (); }
	bool SeqView (sidecar_seq_view_t *v, unsigned *) override
	{
		if (!seqNew) return false;
		seqNew = false, nSeq++;
		*v = seq;
		return true;
	}
	int Playhead () override { return playMs; }
	void RytmPatWant (int n) override { wantPage = n; }
	bool RytmPattern (rytm_pattern_t *p, int *i, unsigned *) override
	{
		if (!patNew) return false;
		patNew = false, nPat++;
		*p = pat, *i = patIndex;
		return true;
	}
	void RytmPatWhere (remote_views::PatWhere *w) override { nWhere++, strncpy (w->text, where.c_str (), sizeof w->text - 1); w->stale = where != "from the Rytm"; }
	void VoiceFill (sidecar_voice_view_t *v, const rytm_pattern_t *p, int idx) override
	{
		nVoice++, voicePat = p, voiceIdx = idx;
		v->valid = 1, v->track = 3, v->val[1] = (unsigned char) (10 + nVoice % 100), strcpy (v->name, "SD PLAITS");
	}
	sidecar_status_view_t *StatusView () override { return &status; }
	void StatusFill (sidecar_status_view_t *v) override { nStatus++, v->valid = 1, v->temp_c = 41, strcpy (v->ip, "172.31.77.1"), v->underruns = nStatus; }
	void InsFill (sidecar_insfx_view_t *v) override { nIns++, v->valid = 1, v->slot[2][0].type = 3, strcpy (v->slot[2][0].name, "FILTER"), v->slot[2][0].v[1] = (unsigned char) (50 + nIns % 50); }
	void MfxFill (sidecar_mfx_view_t *v) override { nMfx++, v->valid = 1, v->lev = 100, v->slot[1].type = 4, strcpy (v->slot[1].name, "COMP"); }
	// what core 3 does with a command line (the console's queue): the sequencer's add
	void Command (const char *line)
	{
		int t, s, n, m, v, l;
		if (sscanf (line, "seqed=add:%d:%d:%d:%d:%d:%d", &t, &s, &n, &m, &v, &l) == 6 && seq.n < 1000)
		{
			seq.note[seq.n].step = (unsigned short) s, seq.note[seq.n].pitch = (unsigned char) n, seq.note[seq.n].vel = (unsigned char) v, seq.note[seq.n].len = (unsigned short) l;
			seq.n++, seqNew = true;
		}
	}
};

// ---- the local display: records every call --------------------------------------------------------------------------------

struct FakeDisplay : remote_views::Display
{
	bool seqS = false, patS = false, voiceS = false, statusS = false, insS = false, mfxS = false;
	int seqView = 0, showSeq = 0, playhead = 0, rytmPat = 0, where = 0, voice = 0, status = 0, ins = 0, mfx = 0;
	bool SeqShown () override { return seqS; }
	bool PatShown () override { return patS; }
	bool VoiceShown () override { return voiceS; }
	bool StatusShown () override { return statusS; }
	bool InsShown () override { return insS; }
	bool MfxShown () override { return mfxS; }
	void SeqView (const sidecar_seq_view_t *) override { seqView++; }
	void ShowSeq () override { showSeq++; }
	void Playhead (int) override { playhead++; }
	void RytmPat (const rytm_pattern_t *, int, const char *) override { rytmPat++; }
	void RytmPatWhere (const char *, int) override { where++; }
	void VoiceSet (const sidecar_voice_view_t *) override { voice++; }
	unsigned UiMaxUs () override { return 1234; }
	void StatusSet (const sidecar_status_view_t *) override { status++; }
	void InsSet (const sidecar_insfx_view_t *) override { ins++; }
	void MfxSet (const sidecar_mfx_view_t *) override { mfx++; }
};

// ---- the rig: the service with remote_pi's gate rule, a client, a clock -----------------------------------------------------

static FakeEngine *g_Eng;
static std::vector<std::string> g_Cmds;
static void hook_cmd (void *, const char *line) { g_Cmds.push_back (line); if (g_Eng) g_Eng->Command (line); }
static const char *hook_code (void *) { return "12345678"; }
static void hook_log (void *, const char *) {}

struct Rig : remote_views::Remote
{
	Refs refs;
	Server srv;
	Pipe pipe;
	rmt_cli_t cli;
	rmt_cli_cfg_t cfg;
	remote_views::Feed feed;
	FakeEngine eng;
	uint32_t now = 1000;
	bool local[RMT_V_COUNT] = {};
	Rig (uint32_t startUs) : srv (Hooks{0, hook_cmd, hook_code, hook_log, 0}, &refs)
	{
		eng.startUs = startUs;
		g_Eng = &eng;
		cfg = rmt_cli_cfg_t{send, this, 1, 1280, 720, 1, "12345678", 0, 0};
		rmt_cli_init (&cli, &cfg);
	}
	static int send (void *ud, const uint8_t *d, size_t n) { Rig *r = (Rig *) ud; r->pipe.c2s.insert (r->pipe.c2s.end (), d, d + n); return 0; }
	// remote_pi.cpp's rule: the display's hold is an edge on the reference count
	bool Gate (int v, bool bLocalShown) override
	{
		if (v > 0 && v < RMT_V_COUNT && local[v] != bLocalShown)
		{
			if (bLocalShown) refs.Add (v); else refs.Release (v);
			local[v] = bLocalShown;
		}
		return refs.On (v);
	}
	bool Offer (int v, const void *p) override { return srv.Offer (v, p, now); }
	bool Due (int v) override { return srv.Due (v); }
	void connect ()
	{
		uint8_t nonce[16] = {};
		srv.Accept (&pipe, now, 0x1234, nonce, false, "0.9.9", 0);
		rmt_cli_connected (&cli, now);
	}
	// one 10 ms loop of the kernel (the view feed), then the service's pump and the client
	void loop (remote_views::Display *d, int n = 1)
	{
		for (int i = 0; i < n; i++)
		{
			now += 10, eng.nowMs += 10;
			feed.Run (*this, eng, d);
			if (srv.Connected () && !srv.Pump (now)) srv.Release ();
			if (!pipe.s2c.empty ()) { rmt_cli_feed (&cli, pipe.s2c.data (), pipe.s2c.size (), now); pipe.s2c.clear (); }
		}
	}
	void subscribeAll ()
	{
		rmt_cli_want (&cli, RMT_V_STATUS, 2), rmt_cli_want (&cli, RMT_V_VOICE, 10), rmt_cli_want (&cli, RMT_V_INSFX, 10);
		rmt_cli_want (&cli, RMT_V_MFX, 10), rmt_cli_want (&cli, RMT_V_SEQ, 10), rmt_cli_want (&cli, RMT_V_SEQPOS, 30), rmt_cli_want (&cli, RMT_V_PAT, 10);
	}
	~Rig () { g_Eng = 0; }
};

static void seed (FakeEngine &e)
{
	e.seq.valid = 1, e.seq.track = 2, e.seq.n = 3, e.seq.length = 32, e.seq.bpm10 = 1203;
	for (int i = 0; i < 3; i++) e.seq.note[i].step = (unsigned short) i, e.seq.note[i].pitch = (unsigned char) (60 + i), e.seq.note[i].vel = 100, e.seq.note[i].len = 96;
	e.seqNew = true;
	e.pat.valid = 1, e.pat.nlocks = 4, e.pat.t[4].trig[9] = 1, e.pat.t[4].note[9] = 60;
	e.patNew = true;
	e.playMs = 1500;
}

// ---- no display ---------------------------------------------------------------------------------------------------------------

static void test_headless (uint32_t startUs)
{
	Rig r (startUs);
	seed (r.eng);
	// nobody subscribed, no display: nothing is built (the core 0 budget); core 3's publications are only polled
	r.connect ();
	r.loop (0, 30);
	CHECK (rmt_cli_ready (&r.cli));
	CHECK (r.eng.nVoice == 0 && r.eng.nStatus == 0 && r.eng.nIns == 0 && r.eng.nMfx == 0 && r.eng.nWhere == 0 && r.eng.wantPage == 0);
	r.subscribeAll ();
	r.loop (0, 3);
	CHECK (r.refs.On (RMT_V_SEQ) && r.refs.On (RMT_V_PAT) && r.refs.On (RMT_V_VOICE) && !r.local[RMT_V_VOICE]);		// the client's references only: nothing is "shown" locally
	r.loop (0, 60);
	CHECK (r.eng.wantPage == 1);										// a remote PAT and VOICE: the Rytm pattern fetch is held, as PAT (as on the Pi's own screen)
	// every subscribed view reaches the client
	CHECK (rmt_cli_take (&r.cli, RMT_V_SEQ) && r.cli.sq.valid && r.cli.sq.n == 3 && r.cli.sq.note[2].pitch == 62 && !strcmp (r.cli.sq.project, "SHARED SET"));
	CHECK (rmt_cli_take (&r.cli, RMT_V_SEQPOS) && r.cli.seqpos.pos32 == (1500 * 32 + 500) / 1000);
	CHECK (rmt_cli_take (&r.cli, RMT_V_PAT) && r.cli.pat.pat.valid && r.cli.pat.index == 5 && r.cli.pat.pat.t[4].note[9] == 60 && !strcmp (r.cli.pat.project, "SHARED SET"));
	CHECK (!strcmp (r.cli.pat.where, "from the Rytm"));
	CHECK (rmt_cli_take (&r.cli, RMT_V_VOICE) && r.cli.voice.valid && !strcmp (r.cli.voice.name, "SD PLAITS") && r.cli.voice.track == 3);
	CHECK (r.eng.voicePat != 0 && r.eng.voiceIdx == 5);							// VOICE's p-locks come from the Rytm pattern
	CHECK (rmt_cli_take (&r.cli, RMT_V_STATUS) && r.cli.status.temp_c == 41 && !strcmp (r.cli.status.ip, "172.31.77.1") && r.cli.status.ui_max_us == 0 && r.cli.status.gen != 0);
	CHECK (rmt_cli_take (&r.cli, RMT_V_INSFX) && !strcmp (r.cli.insfx.slot[2][0].name, "FILTER"));
	CHECK (rmt_cli_take (&r.cli, RMT_V_MFX) && !strcmp (r.cli.mfx.slot[1].name, "COMP") && r.cli.mfx.lev == 100);
	// the builds run at their own rates: VOICE / INSFX / MFX ~10 a second, STATUS ~2 a second, SEQ only when core 3 published
	int v0 = r.eng.nVoice, s0 = r.eng.nStatus, i0 = r.eng.nIns, m0 = r.eng.nMfx, q0 = r.eng.nSeq;
	r.loop (0, 200);									// 2 s
	CHECK (r.eng.nVoice - v0 >= 18 && r.eng.nVoice - v0 <= 21);
	CHECK (r.eng.nIns - i0 >= 18 && r.eng.nIns - i0 <= 21 && r.eng.nMfx - m0 >= 18 && r.eng.nMfx - m0 <= 21);
	CHECK (r.eng.nStatus - s0 >= 3 && r.eng.nStatus - s0 <= 5);
	CHECK (r.eng.nSeq == q0);
	// the play head moves; a still one sends nothing
	rmt_cli_take (&r.cli, RMT_V_SEQPOS);
	uint32_t f = r.srv.Sess ().Frames ();
	r.eng.playMs = 1500;
	r.loop (0, 20);
	r.eng.playMs = 2250;
	r.loop (0, 5);
	CHECK (rmt_cli_take (&r.cli, RMT_V_SEQPOS) && r.cli.seqpos.pos32 == (2250 * 32 + 500) / 1000);
	r.eng.playMs = -1;
	r.loop (0, 5);
	CHECK (rmt_cli_take (&r.cli, RMT_V_SEQPOS) && r.cli.seqpos.pos32 == -1);
	(void) f;
	// an edit: the app's tap sends a line, the engine applies it (core 3), the new view comes back
	g_Cmds.clear ();
	CHECK (rmt_cli_cmd (&r.cli, "seqed=add:2:9:65:0:100:96") > 0);
	r.loop (0, 40);
	CHECK (g_Cmds.size () == 1 && g_Cmds[0] == "seqed=add:2:9:65:0:100:96");
	CHECK (rmt_cli_take (&r.cli, RMT_V_SEQ) && r.cli.sq.n == 4 && r.cli.sq.note[3].pitch == 65 && r.cli.sq.note[3].step == 9);
	// a line off the list still goes nowhere, headless or not
	CHECK (rmt_cli_cmd (&r.cli, "reboot") > 0);
	r.loop (0, 20);
	CHECK (g_Cmds.size () == 1);
	// a new pattern and a changed where-text
	r.eng.pat.t[4].note[9] = 61, r.eng.patNew = true, r.eng.where = "fetching...";
	r.loop (0, 60);
	CHECK (rmt_cli_take (&r.cli, RMT_V_PAT) && r.cli.pat.pat.t[4].note[9] == 61 && !strcmp (r.cli.pat.where, "fetching...") && r.cli.pat.stale == 1);
	// a client that unsubscribes from everything: the references go, the builders stop, the pattern fetch is released
	for (int v : {RMT_V_STATUS, RMT_V_VOICE, RMT_V_INSFX, RMT_V_MFX, RMT_V_SEQ, RMT_V_SEQPOS, RMT_V_PAT}) rmt_cli_want (&r.cli, v, 0);
	r.loop (0, 5);
	v0 = r.eng.nVoice, s0 = r.eng.nStatus, i0 = r.eng.nIns, m0 = r.eng.nMfx;
	r.loop (0, 100);
	CHECK (r.eng.nVoice == v0 && r.eng.nStatus == s0 && r.eng.nIns == i0 && r.eng.nMfx == m0 && r.eng.wantPage == 0);
	// a client that leaves: the references are released with it
	r.srv.Release ();
	r.loop (0, 5);
	for (int v = 1; v < RMT_V_COUNT; v++) CHECK (!r.refs.On (v));
}

// only PAT: the fetch is held as PAT (1), VOICE not built; and one view at a time reaches only the subscriber
static void test_headless_pat_only (uint32_t startUs)
{
	Rig r (startUs);
	seed (r.eng);
	r.connect ();
	rmt_cli_want (&r.cli, RMT_V_PAT, 10);
	r.loop (0, 40);
	CHECK (r.eng.wantPage == 1 && r.eng.nVoice == 0 && r.eng.nStatus == 0);
	CHECK (rmt_cli_take (&r.cli, RMT_V_PAT) && r.cli.pat.pat.valid && !rmt_cli_take (&r.cli, RMT_V_VOICE));
	// a client that subscribes after the pattern was published still gets a snapshot (the Due rule)
	Rig r2 (startUs);
	seed (r2.eng);
	r2.connect ();
	r2.loop (0, 30);
	rmt_cli_want (&r2.cli, RMT_V_SEQ, 10);
	r2.loop (0, 30);
	CHECK (rmt_cli_take (&r2.cli, RMT_V_SEQ) && r2.cli.sq.n == 3);
	r2.srv.Release ();
}

// ---- with a display: unchanged for the local pages -------------------------------------------------------------------------

static void test_display (uint32_t startUs)
{
	Rig r (startUs);
	FakeDisplay d;
	seed (r.eng);
	// no client, nothing shown: the display gets what it always got (the sequencer's view and the play head), nothing is built for pages that do not show
	r.loop (&d, 40);
	CHECK (d.seqView == 1 && d.playhead > 0 && d.voice == 0 && d.status == 0 && d.ins == 0 && d.mfx == 0 && d.rytmPat == 1);
	CHECK (r.eng.nVoice == 0 && r.eng.nStatus == 0 && r.eng.nIns == 0 && r.eng.nMfx == 0 && r.eng.wantPage == 0);
	// the Rytm's TRK + TRIG asks for SEQ: once per request
	r.eng.seq.show_req = 1, r.eng.seqNew = true;
	r.loop (&d, 3);
	CHECK (d.showSeq == 1);
	r.eng.seq.n = 3, r.eng.seqNew = true;
	r.loop (&d, 3);
	CHECK (d.showSeq == 1);
	// pages that show are built and fed to the display; the others not
	d.voiceS = true, d.statusS = true;
	r.loop (&d, 60);
	CHECK (d.voice >= 5 && d.status >= 1 && d.ins == 0 && d.mfx == 0 && r.eng.nIns == 0 && r.eng.nMfx == 0);
	CHECK (r.eng.wantPage == 2);
	CHECK (d.status == r.eng.nStatus && d.voice == r.eng.nVoice);
	d.voiceS = false, d.statusS = false, d.insS = true, d.mfxS = true, d.patS = true;
	r.loop (&d, 60);
	CHECK (d.ins >= 5 && d.mfx >= 5 && r.eng.wantPage == 1 && d.where >= 1);
	// a client now too: it is given the same builds (one build feeds both)
	r.connect ();
	r.subscribeAll ();
	int ins0 = d.ins, nIns0 = r.eng.nIns, voice0 = d.voice;
	r.loop (&d, 60);
	CHECK (rmt_cli_ready (&r.cli) && rmt_cli_take (&r.cli, RMT_V_INSFX) && rmt_cli_take (&r.cli, RMT_V_VOICE) && rmt_cli_take (&r.cli, RMT_V_SEQ) && rmt_cli_take (&r.cli, RMT_V_STATUS));
	CHECK (d.ins - ins0 == r.eng.nIns - nIns0 && r.eng.nIns - nIns0 >= 5);		// not built twice
	CHECK (r.cli.status.ui_max_us == 1234);						// the display's cost is in the view when there is a display
	CHECK (d.voice == voice0);							// the local VOICE page is not shown: no setter for the client's build
	// the local page leaves: the client's hold keeps the build, the display setter stops
	d.insS = false;
	ins0 = d.ins;
	r.loop (&d, 60);
	CHECK (d.ins == ins0 && r.eng.nIns > nIns0 + 5);
	r.srv.Release ();
}

int main ()
{
	for (uint32_t start : {0u, 0xFFFFF000u, 0xFFFF0000u})
	{
		test_headless (start);
		test_headless_pat_only (start);
		test_display (start);
	}
	puts ("test_remote_views: ok");
	return 0;
}

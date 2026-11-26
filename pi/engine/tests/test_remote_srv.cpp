// pi/engine/tests/test_remote_srv.cpp - the remote UI service's portable part (remote_srv.cpp): the command allowlist, the
// reference counts, the pairing proof and its lockout, the session against the real client (ui/remote/rmt_cli.c) over an
// in-memory link, deltas and rates, the drag released on a disconnect, one client at a time, a slow reader, a dead link,
// garbage. A made-up pairing code only.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include "check.h"
#include "remote_srv.h"
#include "../../ui/remote/rmt_cli.h"

using namespace remote;

// ---- an in-memory link -------------------------------------------------------------------------------------------

struct Pipe : Conn
{
	std::vector<uint8_t> c2s, s2c;			// client -> server, server -> client
	bool closed = false;
	size_t sendMax = 1u << 30;			// bytes the server may send per call (0: not now)
	int Recv (uint8_t *p, size_t n) override
	{
		if (closed) return -1;
		size_t k = c2s.size() < n ? c2s.size() : n;
		memcpy (p, c2s.data(), k);
		c2s.erase (c2s.begin(), c2s.begin() + (long) k);
		return (int) k;
	}
	int Send (const uint8_t *p, size_t n) override
	{
		if (closed) return -1;
		size_t k = n < sendMax ? n : sendMax;
		s2c.insert (s2c.end(), p, p + k);
		return (int) k;
	}
};

static std::vector<std::string> g_Cmds;
static const char *g_Code = "12345678";
static std::vector<std::string> g_Log;
static void hook_cmd (void *, const char *line) { g_Cmds.push_back (line); }
static const char *hook_code (void *) { return g_Code; }
static void hook_log (void *, const char *what) { g_Log.push_back (what); }
static Hooks hooks () { return Hooks{0, hook_cmd, hook_code, hook_log, 0}; }

struct Rig
{
	Refs refs;
	Server srv;
	Pipe pipe;
	rmt_cli_t cli;
	rmt_cli_cfg_t cfg;
	uint32_t now = 1000;
	int results = 0, lastOk = -1, lastCode = 0, pages = 0, lastPage = -1;
	bool serverUp = false;
	Rig () : srv (hooks (), &refs)
	{
		cfg = rmt_cli_cfg_t{send, this, 1, 1280, 720, 1, g_Code, result, page};
		rmt_cli_init (&cli, &cfg);
	}
	static int send (void *ud, const uint8_t *d, size_t n) { Rig *r = (Rig *) ud; r->pipe.c2s.insert (r->pipe.c2s.end(), d, d + n); return 0; }
	static void result (void *ud, uint32_t, int ok, int code, const char *) { Rig *r = (Rig *) ud; r->results++, r->lastOk = ok, r->lastCode = code; }
	static void page (void *ud, int p) { Rig *r = (Rig *) ud; r->pages++, r->lastPage = p; }
	bool connect (bool auth = false, int page = 8)
	{
		uint8_t nonce[16];
		for (int i = 0; i < 16; i++) nonce[i] = (uint8_t) (i * 7 + 1);
		serverUp = srv.Accept (&pipe, now, 0x1234, nonce, auth, "0.9.9", page);
		rmt_cli_connected (&cli, now);
		return serverUp;
	}
	// one round: server reads and answers, the client reads what came; false: either side ended it
	bool step (uint32_t ms = 1)
	{
		now += ms;
		bool ok = srv.Connected () ? srv.Pump (now) : false;
		if (!ok && srv.Connected ()) srv.Release ();		// as the service's loop does when Pump says the end
		bool cok = true;
		if (!pipe.s2c.empty ()) { cok = rmt_cli_feed (&cli, pipe.s2c.data (), pipe.s2c.size (), now) == 0; pipe.s2c.clear (); }
		return ok && cok;
	}
	void run (int n, uint32_t ms = 1) { for (int i = 0; i < n; i++) step (ms); }
};

// ---- tests -----------------------------------------------------------------------------------------------------

static void test_allowlist (void)
{
	static const char *const yes[] = {"ins=V1:A:p1:64", "ins=V3:send:2:100", "ins=V8:B:type:CHORUS", "out=V1:post", "out=V8:lev:127", "mfx=1:p2:50",
		"mfx=lev:100", "mfx=byp:1", "mfx=src:ret", "vparam=5:3:64", "vparam=11:7:127:0", "vparam=0:0:0", "vparam=4:12:64", "vparam=4:15:0:0", "ins=V1:A:type:FILTER", "ins=V2:B:p4:127",
		"out=V3:pre", "out=V3:trk", "out=V2:pan:64", "out=V2:wid:0", "mfx=2:type:COMP", "mfx=3:pair:1", "mfx=4:mix:99", "mfx=1:p4:0"};
	static const char *const no[] = {"", ":reboot", "reboot", "sdf", "out=main", "out=tracks", "out=V9:lev:1", "out=V1", "out=Vx:post", "keys=mac:on",
		"notes=1", "uistyle=oled", "paircode=1", "pair=show", "ins", "ins=", "=x", "ins=V1:A;reboot", "ins=V1 A", "ins=V1:A\n", "vparam=12:1:1",
		"vparam=1:1:128", "vparam=1:1:1:1", "vparam=1:1:1:0:0", "vparam=1:1", "vparam=a:b:c", "vparam=1:1:", "xins=V1:A:p1:1", "insx=V1",
		"ins=V1:A:p1:64 ", "mfx=\x7f", "net=usb", "usbmode=overbridge", "web=0", "xy=pos:1:2", "voicepick=3",
		"scene=fader:64", "scene=lock:1:CUT:3", "ins=V1:A:reset", "ins=V1:reset", "ins=V9:A:p1:1", "ins=V0:A:p1:1", "ins=V1:C:p1:1", "ins=V1:A:p5:1", "ins=V1:A:p0:1",
		"ins=V1:A:p1:128", "ins=V1:A:p1:-1", "ins=V1:A:p1", "ins=V1:A:p1:1:2", "ins=V1:A:type:", "ins=V1:A:type:filter", "ins=V1:A:type:AAAAAAAAAAA", "ins=V1:send:4:1",
		"ins=V1:send:0:1", "ins=V1:send:1", "out=V1:lev", "out=V1:lev:128", "out=V1:post:1", "out=V1:fx", "mfx=reset", "mfx=byp:2", "mfx=src:x", "mfx=lev", "mfx=lev:128", "mfx=5:p1:1",
		"mfx=0:p1:1", "mfx=1:p5:1", "mfx=1:reset", "mfx=1:pair:2", "mfx=1:mix", "mfx=1:type:", "vparam=4:64:1", "vparam=4:1:128:0", "vparam=4:1:1:00", "vparam=12:1:1"};
	// the SEQ page: exactly what its controls send (sidecar_seqed.c), and neighbours that are not
	static const char *const seqYes[] = {"seqed=add:0:0:60:0:100:96", "seqed=add:11:127:127:-96:127:12288", "seqed=add:3:15:48:96:1:1", "seqed=del:2:5:60", "seqed=move:2:5:60:6:62:-12",
		"seqed=len:2:5:60:192", "seqed=vel:2:5:60:90", "seqed=chord:2:0:60:7:0:100:192:6", "seqed=chord:2:127:127:15:3:127:12288:-96", "seqed=strum:2:0:15:-24",
		"seqed=strum:2:0:127:48", "seqed=human:2:0:15:4:12:65535", "seqed=human:2:0:15:0:0:0", "seqed=undo:2", "seqed=redo:11", "seqed=show:0",
		"seq=key:C#:MAJ:edit", "seq=key:B:WHOLE:off", "seq=key:A:BLUES:play", "seq=spd:1:follow", "seq=spd:12:7/8", "seq=spd:3:2/3", "seq=spd:3:2", "seq=dir:2:brown", "seq=dir:2:fwd",
		"seq=rnd:2:mode=steps,oct=2,wght=flat", "seq=rnd:12:mode=track,oct=4,wght=step", "seq=alock:2:16:3:-1", "seq=alock:2:128:4:5",
		"seq=rtrg:2:3:off", "seq=rtrg:2:3:cnt=2,mode=chord,rate=auto", "seq=rtrg:12:128:cnt=0,mode=updn,rate=1/4,oct=2", "seq=rtrg:2:3:cnt=8,mode=rnd,rate=1/4,oct=2",
		"seq=fx:2:3:1:OCT:-1", "seq=fx:2:3:1:off", "seq=fx:2:3:1:SKP:1", "seq=lcond:2:3:all:50%", "seq=lcond:2:3:all:---", "seq=lcond:2:3:all:1:2", "seq=lcond:2:3:all:fill",
		"seq=slide:2:3:all:1", "seq=slide:12:128:all:0", "seq=rndstep:2:3:1", "seq=rndstep:2:3:0", "seq=seed:free", "seq=seed:0", "seq=seed:9999", "seq=print:2", "seq=swing:2:50", "seq=swing:12:80", "seq=poly:2:voices=0", "seq=poly:2:voices=8", "seq=poly:2:alloc=rotate",
		"seq=poly:2:unison=4", "seq=poly:2:unison=1"};
	static const char *const seqNo[] = {"seqed=", "seqed=add", "seqed=add:0:0:60:0:100", "seqed=add:0:0:60:0:100:96:1", "seqed=add:12:0:60:0:100:96", "seqed=add:0:128:60:0:100:96", "seqed=add:0:0:128:0:100:96",
		"seqed=add:0:0:60:97:100:96", "seqed=add:0:0:60:-97:100:96", "seqed=add:0:0:60:0:128:96", "seqed=add:0:0:60:0:100:12289", "seqed=add:0:0:60:0:100:123456", "seqed=add:-1:0:60:0:100:96", "seqed=add:0:0:60:0:100:-1",
		"seqed=add:0:0:60:+1:100:96", "seqed=add:0:0:60: 0:100:96", "seqed=add:0:0:60:0x1:100:96", "seqed=add:0:0:60:0:100:96 ", "seqed=add:0::60:0:100:96", "seqed=addx:0:0:60:0:100:96",
		"seqed=del:0:0", "seqed=del:0:0:60:1", "seqed=move:0:0:60:128:60:0", "seqed=move:0:0:60:1:128:0", "seqed=move:0:0:60:1:60", "seqed=len:0:0:60", "seqed=vel:0:0:60:128",
		"seqed=chord:0:0:60:16:0:100:96:0", "seqed=chord:0:0:60:7:4:100:96:0", "seqed=chord:0:0:60:7:0:100:96", "seqed=strum:0:0:15", "seqed=strum:0:0:128:1", "seqed=strum:0:0:15:97", "seqed=human:0:0:15:97:0:0",
		"seqed=human:0:0:15:0:128:0", "seqed=human:0:0:15:0:0:65536", "seqed=human:0:0:15:0:0", "seqed=undo", "seqed=undo:", "seqed=undo:12", "seqed=redo:0:0", "seqed=show:12", "seqed=show:-1", "seqed=nuke:0",
		"seq=", "seq=run", "seq=slide:2:3:all:2", "seq=slide:2:3:one:1", "seq=slide:2:3:1", "seq=rndstep:2:3", "seq=rndstep:2:3:2", "seq=rndstep:2:3:all:1", "seq=fill:2:3:all:1", "seq=key:H:MAJ:edit", "seq=key:C:MAJ", "seq=key:C:MAJ:EDIT", "seq=key:C:MAJOR:edit", "seq=key:C:MAJ:edit:1", "seq=key:c:MAJ:edit",
		"seq=spd:0:follow", "seq=spd:13:follow", "seq=spd:1:3/4x", "seq=spd:1:1/16", "seq=spd:1:", "seq=dir:1:backwards", "seq=rnd:1:mode=on,oct=2,wght=flat", "seq=rnd:1:mode=steps,oct=0,wght=flat",
		"seq=rnd:1:mode=steps,oct=5,wght=flat", "seq=rnd:1:mode=steps,oct=2", "seq=rnd:1:mode=steps,oct=2,wght=flat,x=1", "seq=alock:1:0:3:-1", "seq=alock:1:129:3:-1", "seq=alock:1:1:5:0",
		"seq=alock:1:1:3:-2", "seq=alock:1:1:3:8", "seq=rtrg:1:1:cnt=9,mode=chord,rate=auto", "seq=rtrg:1:1:cnt=2,mode=chord,rate=auto,x", "seq=rtrg:1:1:", "seq=fx:1:1:2:off", "seq=fx:1:1:1:BAD:1",
		"seq=fx:1:1:1:OCT:2", "seq=lcond:1:1:one:50%", "seq=lcond:1:1:all:75%", "seq=seed:", "seq=seed:10000", "seq=seed:-1", "seq=seed:free:1", "seq=seed:FREE", "seq=print:0", "seq=print:13", "seq=print:1:2",
		"seq=swing:1:49", "seq=swing:1:81", "seq=swing:1:-50", "seq=swing:1", "seq=poly:1:voices=9", "seq=poly:1:voices=10", "seq=poly:1:alloc=roundrobin", "seq=poly:1:unison=0", "seq=poly:1:unison=5",
		"seq=poly:1:voices=2,alloc=rotate", "seq=poly:1:", "seq=poly:1", "seq=poly:0:voices=2", "seqed", "seqed:0", "seqq=print:1", "seq =print:1"};
	for (const char *s : seqYes) { if (!CommandAllowed (s)) fprintf (stderr, "should pass: %s\n", s); CHECK (CommandAllowed (s)); }
	for (const char *s : seqNo) { if (CommandAllowed (s)) fprintf (stderr, "should fail: %s\n", s); CHECK (!CommandAllowed (s)); }
	for (const char *s : yes) { if (!CommandAllowed (s)) fprintf (stderr, "should pass: %s\n", s); CHECK (CommandAllowed (s)); }
	for (const char *s : no) { if (CommandAllowed (s)) fprintf (stderr, "should fail: %s\n", s); CHECK (!CommandAllowed (s)); }
	CHECK (!CommandAllowed (0));
	std::string big = "ins=V1:" + std::string (60, 'a');
	CHECK (!CommandAllowed (big.c_str ()));			// 67 characters
	CHECK (!CommandAllowed (big.substr (0, 63).c_str ()));		// (right length, but not a line of the pages)
}

static void test_parts (void)
{
	Refs r;
	CHECK (!r.On (RMT_V_VOICE));
	r.Add (RMT_V_VOICE), r.Add (RMT_V_VOICE);
	CHECK (r.On (RMT_V_VOICE) && r.Count (RMT_V_VOICE) == 2);
	r.Release (RMT_V_VOICE);
	CHECK (r.On (RMT_V_VOICE));
	r.Release (RMT_V_VOICE), r.Release (RMT_V_VOICE);	// (a release too many does not wrap)
	CHECK (!r.On (RMT_V_VOICE) && r.Count (RMT_V_VOICE) == 0);
	r.Add (0), r.Add (99), r.Release (-1);
	CHECK (!r.On (0) && !r.On (99));

	static Ring ring;
	uint8_t buf[5000], out[5000];
	for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (uint8_t) (i * 31);
	for (int round = 0; round < 20; round++)		// wraps around the end many times
	{
		CHECK (ring.Put (buf, sizeof buf));
		size_t got = 0;
		while (got < sizeof buf)
		{
			const uint8_t *p;
			size_t n = ring.Peek (&p);
			CHECK (n > 0);
			if (n > 777) n = 777;
			if (n > sizeof buf - got) n = sizeof buf - got;
			memcpy (out + got, p, n), ring.Drop (n), got += n;
		}
		CHECK (!memcmp (buf, out, sizeof buf) && ring.Used () == 0);
	}
	CHECK (ring.Put (buf, 5000) && ring.Put (buf, 5000) && ring.Put (buf, 5000) && ring.Put (buf, 5000));
	CHECK (!ring.Put (buf, 5000) && ring.Used () == 20000);	// all or nothing
	ring.Clear ();

	AuthLimiter l;
	for (unsigned i = 0; i < AuthLimiter::kMaxFails; i++) { CHECK (l.Allowed (100)); l.Fail (100); }
	CHECK (!l.Allowed (100 + 59000) && l.Allowed (100 + 60000));
	l.Ok ();
	CHECK (l.Allowed (200));
}

static void subscribe_voice (Rig &r)
{
	rmt_cli_want (&r.cli, RMT_V_VOICE, 10);
	rmt_cli_want (&r.cli, RMT_V_INSFX, 10);
	rmt_cli_want (&r.cli, RMT_V_MFX, 10);
	rmt_cli_want (&r.cli, RMT_V_STATUS, 2);
}

static void test_session (void)
{
	g_Cmds.clear ();
	static Rig r;
	subscribe_voice (r);
	CHECK (r.connect ());
	r.run (5);
	CHECK (rmt_cli_ready (&r.cli) && r.cli.pi_page == 8 && !strcmp (r.cli.fw, "0.9.9"));
	CHECK (r.refs.On (RMT_V_VOICE) && r.refs.On (RMT_V_INSFX) && r.refs.On (RMT_V_MFX) && r.refs.On (RMT_V_STATUS));
	CHECK (r.srv.Sess ().Subscribed (RMT_V_VOICE));

	// the engine's fills arrive, then only the changes, at the subscription's rate
	static sidecar_voice_view_t v;
	memset (&v, 0, sizeof v);
	v.valid = 1, v.track = 4, v.val[1] = 20;
	strcpy (v.name, "SD PLAITS");
	r.srv.Offer (RMT_V_VOICE, &v, r.now);
	r.run (3);
	CHECK (rmt_cli_take (&r.cli, RMT_V_VOICE) && !strcmp (r.cli.voice.name, "SD PLAITS") && r.cli.voice.val[1] == 20 && r.cli.voice.track == 4);
	uint32_t frames = r.srv.Sess ().Frames ();
	r.srv.Offer (RMT_V_VOICE, &v, r.now + 200);		// unchanged: nothing is sent
	r.run (3);
	CHECK (r.srv.Sess ().Frames () == frames && !rmt_cli_take (&r.cli, RMT_V_VOICE));
	v.val[1] = 21;
	r.srv.Offer (RMT_V_VOICE, &v, r.now + 400);
	r.run (3);
	CHECK (r.srv.Sess ().Frames () == frames + 1 && rmt_cli_take (&r.cli, RMT_V_VOICE) && r.cli.voice.val[1] == 21);
	// 10 Hz: an Offer every 10 ms is sent once per 100 ms
	frames = r.srv.Sess ().Frames ();
	uint32_t t0 = r.now;
	int sent = 0;
	for (int i = 0; i < 100; i++)
	{
		v.val[2] = (unsigned char) i;
		uint32_t f = r.srv.Sess ().Frames ();
		r.srv.Offer (RMT_V_VOICE, &v, t0 + 1000 + (uint32_t) i * 10);
		sent += r.srv.Sess ().Frames () != f;
	}
	CHECK (sent >= 9 && sent <= 11);

	// the other views
	static sidecar_insfx_view_t iv;
	memset (&iv, 0, sizeof iv);
	iv.valid = 1, iv.slot[2][0].type = 3, strcpy (iv.slot[2][0].name, "FILTER"), iv.slot[2][0].v[1] = 99;
	r.srv.Offer (RMT_V_INSFX, &iv, r.now);
	static sidecar_mfx_view_t mv;
	memset (&mv, 0, sizeof mv);
	mv.valid = 1, mv.lev = 100, mv.slot[1].type = 4, strcpy (mv.slot[1].name, "COMP");
	r.srv.Offer (RMT_V_MFX, &mv, r.now);
	static sidecar_status_view_t sv;
	memset (&sv, 0, sizeof sv);
	sv.valid = 1, sv.temp_c = 55, strcpy (sv.ip, "172.31.77.1");
	r.srv.Offer (RMT_V_STATUS, &sv, r.now);
	r.run (3);
	CHECK (r.cli.insfx.slot[2][0].type == 3 && !strcmp (r.cli.insfx.slot[2][0].name, "FILTER") && r.cli.insfx.slot[2][0].v[1] == 99);
	CHECK (!strcmp (r.cli.mfx.slot[1].name, "COMP") && r.cli.mfx.lev == 100);
	CHECK (r.cli.status.temp_c == 55 && !strcmp (r.cli.status.ip, "172.31.77.1"));

	// the page the Pi shows follows
	r.srv.SetPage (9);
	r.run (2);
	CHECK (r.pages == 1 && r.lastPage == 9);
	r.srv.SetPage (9);
	r.run (2);
	CHECK (r.pages == 1);

	// RESYNC from the client: everything again
	frames = r.srv.Sess ().Frames ();
	rmt_cli_resync (&r.cli);
	r.run (2);
	r.srv.Offer (RMT_V_VOICE, &v, r.now + 5000);
	r.run (2);
	CHECK (r.srv.Sess ().Frames () == frames + (uint32_t) rmt_view_nsi (RMT_V_VOICE));

	// unsubscribe: no more frames, the reference released
	rmt_cli_want (&r.cli, RMT_V_VOICE, 0);
	r.run (3);
	CHECK (!r.refs.On (RMT_V_VOICE) && !r.srv.Sess ().Subscribed (RMT_V_VOICE));
	frames = r.srv.Sess ().Frames ();
	v.val[5] = 5;
	r.srv.Offer (RMT_V_VOICE, &v, r.now + 9000);
	CHECK (r.srv.Sess ().Frames () == frames);
	CHECK (r.refs.On (RMT_V_INSFX));
	r.srv.Release ();
	CHECK (!r.refs.On (RMT_V_INSFX) && !r.refs.On (RMT_V_MFX) && !r.refs.On (RMT_V_STATUS));
	CHECK (g_Cmds.empty ());
}

static void g_Eng_dummy_clear () {}
static void test_commands (void)
{
	g_Cmds.clear ();
	static Rig r;
	CHECK (r.connect ());
	r.run (3);
	int seq = rmt_cli_cmd (&r.cli, "ins=V2:A:p1:64");
	r.run (2);
	CHECK (seq > 0 && g_Cmds.size () == 1 && g_Cmds[0] == "ins=V2:A:p1:64" && r.results == 1 && r.lastOk == 1);
	rmt_cli_cmd (&r.cli, "reboot");
	rmt_cli_cmd (&r.cli, "out=main");
	rmt_cli_cmd (&r.cli, "ins=V1:A;reboot");
	r.run (2);
	CHECK (g_Cmds.size () == 1 && r.results == 4 && r.lastOk == 0 && r.lastCode == RMT_E_CMD);
	// a raw frame the client library would not send: a line with a control character, and an empty line
	uint8_t f[64];
	size_t n = rmt_frame (f, sizeof f, RMT_CMD, 99, "ins=V1:\x01", 8);
	r.pipe.c2s.insert (r.pipe.c2s.end (), f, f + n);
	n = rmt_frame (f, sizeof f, RMT_CMD, 100, "", 0);
	r.pipe.c2s.insert (r.pipe.c2s.end (), f, f + n);
	r.run (2);
	CHECK (g_Cmds.size () == 1 && r.lastOk == 0 && r.srv.Connected ());
	// a flood is limited, a drag's pace is not
	g_Cmds.clear ();
	for (int i = 0; i < 400; i++) rmt_cli_cmd (&r.cli, "mfx=lev:100");
	r.run (2);
	CHECK (g_Cmds.size () < 200 && g_Cmds.size () >= 100 && r.lastCode == RMT_E_RATE);
	r.run (20, 100);
	g_Cmds.clear ();
	for (int i = 0; i < 100; i++) { rmt_cli_cmd (&r.cli, "vparam=3:2:64:0"); r.step (30); }
	CHECK (g_Cmds.size () == 100);
	// an embedded NUL: the part before it is a fine line, the whole is not one
	g_Eng_dummy_clear ();
	{
		uint8_t pl[24];
		memcpy (pl, "ins=V1:A:p1:5\0reboot", 20);
		n = rmt_frame (f, sizeof f, RMT_CMD, 101, pl, 20);
		size_t before = g_Cmds.size ();
		r.pipe.c2s.insert (r.pipe.c2s.end (), f, f + n);
		r.run (3);
		CHECK (g_Cmds.size () == before && r.lastOk == 0 && r.lastCode == RMT_E_BAD);
	}
	// the lift of a held drag is never refused for speed: drain the bucket, then lift
	r.run (20, 100);
	g_Cmds.clear ();
	rmt_cli_cmd (&r.cli, "vparam=3:2:40:0");
	for (int i = 0; i < 400; i++) rmt_cli_cmd (&r.cli, "mfx=lev:100");
	r.run (2);
	CHECK (r.lastCode == RMT_E_RATE);				// the bucket is empty
	g_Cmds.clear ();
	rmt_cli_cmd (&r.cli, "vparam=3:2:41");			// the lift
	r.run (2);
	CHECK (g_Cmds.size () == 1 && g_Cmds[0] == "vparam=3:2:41");
	rmt_cli_cmd (&r.cli, "vparam=3:2:42");			// a "lift" of nothing held is a plain line: limited
	r.run (2);
	CHECK (g_Cmds.size () == 1 && r.lastCode == RMT_E_RATE);
	// a start line refused for speed is not held: its final line is refused too (no way round the limiter by alternating the two)
	r.run (20, 100);
	for (int i = 0; i < 400; i++) rmt_cli_cmd (&r.cli, "mfx=lev:100");			// the bucket is empty again
	r.run (2);
	g_Cmds.clear ();
	rmt_cli_cmd (&r.cli, "vparam=3:2:50:0");						// refused (RATE)
	r.run (2);
	CHECK (g_Cmds.empty () && r.lastCode == RMT_E_RATE);
	rmt_cli_cmd (&r.cli, "vparam=3:2:51");							// its final line: refused too
	r.run (2);
	CHECK (g_Cmds.empty () && r.lastCode == RMT_E_RATE);
	for (int i = 0; i < 20; i++) { rmt_cli_cmd (&r.cli, i % 2 ? "vparam=3:2:41" : "vparam=3:2:40:0"); r.run (1); }	// alternating start / final
	CHECK (g_Cmds.size () < 20);								// still limited, not 20 through
	r.srv.Release ();
}

static void test_drag_release (void)
{
	g_Cmds.clear ();
	{	// a held drag: the finger's lines without the lift
		static Rig r;
		r.connect ();
		r.run (3);
		rmt_cli_cmd (&r.cli, "vparam=3:2:40:0");
		rmt_cli_cmd (&r.cli, "vparam=3:2:55:0");
		rmt_cli_cmd (&r.cli, "vparam=7:0:10:0");			// a second finger
		rmt_cli_cmd (&r.cli, "vparam=7:0:11");			// ... which lifted
		rmt_cli_cmd (&r.cli, "ins=V1:A:p1:5");
		r.run (3);
		g_Cmds.clear ();
		r.pipe.closed = true;					// the cable is pulled
		CHECK (!r.srv.Pump (r.now + 1));
		r.srv.Release ();
		CHECK (g_Cmds.size () == 1 && g_Cmds[0] == "vparam=3:2:55");	// the lift's line, with the last value
		r.srv.Release ();						// (idempotent)
		CHECK (g_Cmds.size () == 1);
	}
	{	// a lifted drag leaves nothing to release; a BYE ends it the same way
		static Rig r;
		r.connect ();
		r.run (3);
		rmt_cli_cmd (&r.cli, "vparam=1:1:9:0");
		rmt_cli_cmd (&r.cli, "vparam=1:1:9");
		r.run (3);
		g_Cmds.clear ();
		uint8_t f[16];
		size_t n = rmt_frame (f, sizeof f, RMT_BYE, 5, "\0\0", 2);
		r.pipe.c2s.insert (r.pipe.c2s.end (), f, f + n);
		CHECK (!r.srv.Pump (r.now + 1));
		r.srv.Release ();
		CHECK (g_Cmds.empty ());
	}
}

static void test_auth (void)
{
	{	// the right code
		static Rig r;
		subscribe_voice (r);
		CHECK (r.connect (true));
		r.run (5);
		CHECK (rmt_cli_ready (&r.cli) && r.srv.Sess ().Ready ());
		r.srv.Release ();
	}
	{	// before the proof nothing is fed and no command passes
		static Rig r;
		g_Cmds.clear ();
		static rmt_cli_cfg_t nocode;
		nocode = r.cfg;
		nocode.code = 0;					// this client sends no proof (and says so by refusing)
		rmt_cli_init (&r.cli, &nocode);
		r.connect (true);
		r.run (2);
		CHECK (r.cli.state == RMT_CLI_REFUSED && r.srv.Connected () && !r.srv.Sess ().Ready ());
		uint8_t f[64];
		size_t n = rmt_frame (f, sizeof f, RMT_CMD, 7, "ins=V1:A:p1:1", 13);
		r.pipe.c2s.insert (r.pipe.c2s.end (), f, f + n);
		CHECK (!r.srv.Pump (r.now + 1));
		r.srv.Release ();
		CHECK (g_Cmds.empty ());
	}
	{	// a wrong code: refused and counted; five in a row lock the proof out
		g_Code = "12345678";
		AuthLimiter dummy;
		(void) dummy;
		static Rig r;
		static rmt_cli_cfg_t wrong;
		for (int i = 0; i < 5; i++)
		{
			wrong = r.cfg;
			wrong.code = "00000000";
			rmt_cli_init (&r.cli, &wrong);
			r.pipe.closed = false, r.pipe.c2s.clear (), r.pipe.s2c.clear ();
			CHECK (r.connect (true));
			r.run (4);
			CHECK (r.cli.state == RMT_CLI_REFUSED && r.cli.refused_code == RMT_E_AUTH);
			CHECK (!r.srv.Connected ());
			r.now += 100;
		}
		r.pipe.closed = false, r.pipe.c2s.clear (), r.pipe.s2c.clear ();
		CHECK (!r.connect (true));				// locked out: refused with a BYE
		CHECK (!r.pipe.s2c.empty () && r.pipe.s2c[0] == RMT_BYE);
		r.now += 61000;
		r.pipe.s2c.clear (), r.pipe.c2s.clear ();
		rmt_cli_init (&r.cli, &r.cfg);
		CHECK (r.connect (true));
		r.run (4);
		CHECK (rmt_cli_ready (&r.cli));				// a minute later the right code works
		r.srv.Release ();
	}
	{	// the code is never on the wire: nothing the server sent holds it
		static Rig r;
		r.connect (true);
		std::vector<uint8_t> all;
		for (int i = 0; i < 6; i++)
		{
			r.step ();
		}
		std::string wire (r.pipe.c2s.begin (), r.pipe.c2s.end ());
		CHECK (wire.find (g_Code) == std::string::npos);
		r.srv.Release ();
	}
	for (const std::string &l : g_Log) CHECK (l.find (g_Code) == std::string::npos);
}

static void test_one_client (void)
{
	static Rig r;
	CHECK (r.connect ());
	r.run (3);
	Pipe second;
	uint8_t nonce[16] = {0};
	CHECK (!r.srv.Accept (&second, r.now, 2, nonce, false, "x", 1));
	CHECK (second.s2c.size () > RMT_HDR && second.s2c[0] == RMT_BYE && second.s2c[RMT_HDR] == RMT_E_BUSY);
	CHECK (r.srv.Connected () && r.srv.Sess ().Ready ());	// the first is untouched
	r.srv.Release ();
	CHECK (r.srv.Accept (&second, r.now, 2, nonce, false, "x", 1));	// free again
	r.srv.Release ();
}

static void test_slow_reader (void)
{
	static Rig r;
	rmt_cli_want (&r.cli, RMT_V_INSFX, 30);
	r.connect ();
	r.run (3);
	r.pipe.sendMax = 0;					// the client reads nothing
	static sidecar_insfx_view_t iv;
	memset (&iv, 0, sizeof iv);
	uint32_t t = r.now + 100;
	for (int i = 0; i < 40; i++)				// 40 changed views x 2.5 KB overflow the 24 KB ring
	{
		for (int s = 0; s < SIF_CHAINS; s++) iv.slot[s][0].v[0] = (unsigned char) (i + s), iv.slot[s][1].v[2] = (unsigned char) (i * 3 + s);
		r.srv.Offer (RMT_V_INSFX, &iv, t += 100);
		r.srv.Pump (t);
	}
	CHECK (r.srv.Connected ());				// not closed: the engine never waited for it
	r.pipe.sendMax = 1u << 30;
	r.run (400, 1);						// it catches up; the next feed carries everything
	iv.slot[0][0].v[0] = 77, iv.slot[10][1].v[3] = 66;
	r.srv.Offer (RMT_V_INSFX, &iv, t += 100);
	r.run (50);
	CHECK (r.srv.Sess ().Resyncs () >= 1);
	CHECK (r.cli.insfx.slot[0][0].v[0] == 77 && r.cli.insfx.slot[10][1].v[3] == 66);
	CHECK (!memcmp (r.cli.insfx.slot[5][0].v, iv.slot[5][0].v, 4) && r.cli.insfx.slot[9][1].v[2] == iv.slot[9][1].v[2]);
	r.srv.Release ();
}

// the sequencer's views through a session: Offer / Due for a change-driven builder, a dropped frame, a PAT, the play head
static void subscribe_seq (Rig &r)
{
	rmt_cli_want (&r.cli, RMT_V_SEQ, 10);
	rmt_cli_want (&r.cli, RMT_V_SEQPOS, 30);
	rmt_cli_want (&r.cli, RMT_V_PAT, 10);
}

static void test_seq_views (void)
{
	static Rig r;
	subscribe_seq (r);
	r.connect ();
	r.run (5);
	CHECK (rmt_cli_ready (&r.cli) && rmt_cli_view_ok (&r.cli, RMT_V_SEQ) && rmt_cli_view_ok (&r.cli, RMT_V_SEQPOS) && rmt_cli_view_ok (&r.cli, RMT_V_PAT));
	CHECK (r.refs.On (RMT_V_SEQ) && r.refs.On (RMT_V_SEQPOS) && r.refs.On (RMT_V_PAT) && !r.refs.On (RMT_V_VOICE));
	CHECK (r.srv.Due (RMT_V_SEQ) && r.srv.Due (RMT_V_PAT) && !r.srv.Due (RMT_V_VOICE));		// a new subscription has no snapshot yet
	static sidecar_seq_view_t sv;
	memset (&sv, 0, sizeof sv);
	sv.valid = 1, sv.track = 2, sv.n = 70, sv.length = 32, sv.bpm10 = 1203;
	for (int i = 0; i < 70; i++) sv.note[i].step = (unsigned short) (i / 3), sv.note[i].pitch = (unsigned char) (40 + i), sv.note[i].vel = 100, sv.note[i].len = 96;
	snprintf (sv.project, sizeof sv.project, "PROJ");
	CHECK (r.srv.Offer (RMT_V_SEQ, &sv, r.now += 5));
	CHECK (!r.srv.Due (RMT_V_SEQ));
	r.run (10);
	CHECK (rmt_cli_take (&r.cli, RMT_V_SEQ) && r.cli.sq.valid && r.cli.sq.track == 2 && r.cli.sq.n == 70 && r.cli.sq.note[69].pitch == 109 && r.cli.sq.note[69].step == 23);
	CHECK (!strcmp (r.cli.sq.project, "PROJ") && r.cli.sq.bpm10 == 1203 && r.cli.sq.note[64].pitch == 104);
	// the rate: an Offer within the period is held back (false), so a builder that offers on change offers again
	sv.note[0].vel = 55;
	CHECK (!r.srv.Offer (RMT_V_SEQ, &sv, r.now + 10));
	CHECK (r.srv.Offer (RMT_V_SEQ, &sv, r.now + 150));
	r.run (5);
	CHECK (r.cli.sq.note[0].vel == 55);
	// not subscribed (the view is not wanted): nothing to send, taken
	CHECK (r.srv.Offer (RMT_V_VOICE, &sv, r.now + 500));
	// the play head: quantised steps x 32, -1 stopped
	rmt_seqpos_view_t pos = {-1, 0};
	r.srv.Offer (RMT_V_SEQPOS, &pos, r.now += 100);
	r.run (3);
	CHECK (rmt_cli_take (&r.cli, RMT_V_SEQPOS) && r.cli.seqpos.pos32 == -1);
	pos.pos32 = 32 * 5 + 7;
	r.srv.Offer (RMT_V_SEQPOS, &pos, r.now += 100);
	r.run (3);
	CHECK (rmt_cli_take (&r.cli, RMT_V_SEQPOS) && r.cli.seqpos.pos32 == 167);
	uint32_t frames = r.srv.Sess ().Frames ();
	for (int i = 0; i < 30; i++) r.srv.Offer (RMT_V_SEQPOS, &pos, r.now += 40);			// the same position: nothing on the wire
	CHECK (r.srv.Sess ().Frames () == frames);
	// the PAT page: the pattern in tracks and lock slots, and the texts
	static rmt_pat_view_t pv;
	memset (&pv, 0, sizeof pv);
	pv.pat.valid = 1, pv.pat.nlocks = 11, pv.index = 17;
	pv.pat.t[4].trig[9] = 1, pv.pat.t[4].note[9] = 60, pv.pat.t[11].steps = 16, pv.pat.lock[10].type = 3, pv.pat.lock[10].val[63] = 99;
	snprintf (pv.project, sizeof pv.project, "SHARED SET"), snprintf (pv.where, sizeof pv.where, "from the Rytm");
	CHECK (r.srv.Offer (RMT_V_PAT, &pv, r.now += 200));
	r.run (10);
	CHECK (rmt_cli_take (&r.cli, RMT_V_PAT) && r.cli.pat.pat.valid && r.cli.pat.index == 17 && r.cli.pat.pat.t[4].trig[9] == 1 && r.cli.pat.pat.t[4].note[9] == 60);
	CHECK (r.cli.pat.pat.t[11].steps == 16 && r.cli.pat.pat.lock[10].type == 3 && r.cli.pat.pat.lock[10].val[63] == 99 && !strcmp (r.cli.pat.project, "SHARED SET"));
	CHECK (!strcmp (r.cli.pat.where, "from the Rytm") && r.cli.pat.pat.nlocks == 11);
	// a frame that does not fit the ring: the client is behind; Due says so until a full resync went out
	r.pipe.sendMax = 0;
	for (int i = 0; i < 12; i++) { memset (sv.note, i + 1, sizeof sv.note), sv.n = 1000 - i, r.srv.Offer (RMT_V_SEQ, &sv, r.now += 150); r.srv.Pump (r.now); }
	CHECK (r.srv.Connected () && !r.srv.Due (RMT_V_SEQ) && !r.srv.Due (RMT_V_PAT));	// lost, and the ring still full: nothing is due until it drains
	r.pipe.sendMax = 1u << 30;
	r.run (300);
	CHECK (r.srv.Due (RMT_V_SEQ) && r.srv.Due (RMT_V_PAT));					// room again: both views are due, not only the one that overflowed
	CHECK (r.srv.Offer (RMT_V_SEQ, &sv, r.now += 150));
	CHECK (r.srv.Offer (RMT_V_PAT, &pv, r.now += 150));
	r.run (30);
	CHECK (!r.srv.Due (RMT_V_SEQ) && !r.srv.Due (RMT_V_PAT) && r.srv.Sess ().Resyncs () >= 1);
	CHECK (r.cli.sq.n == sv.n && !memcmp (r.cli.sq.note, sv.note, 8 * (size_t) sv.n) && r.cli.pat.pat.lock[10].val[63] == 99 && r.cli.pat.index == 17);
	// a RESYNC from the client makes the views due again
	rmt_cli_resync (&r.cli);
	r.run (3);
	CHECK (r.srv.Due (RMT_V_SEQ) && r.srv.Due (RMT_V_PAT));
	r.srv.Release ();
	CHECK (!r.refs.On (RMT_V_SEQ) && !r.refs.On (RMT_V_SEQPOS) && !r.refs.On (RMT_V_PAT));	// the references go with the client
}


// a client that stops reading (cable pulled, no RST) while the sequencer keeps editing: the encoder runs at the view's rate, not every loop, and
// the full snapshot goes out once when the client drains again
static void test_seq_storm (void)
{
	static Rig r;
	subscribe_seq (r);
	r.connect ();
	r.run (5);
	static sidecar_seq_view_t sv;
	memset (&sv, 0, sizeof sv);
	sv.valid = 1, sv.n = 1000;
	for (int i = 0; i < 1000; i++) sv.note[i].step = (unsigned short) (i % 128), sv.note[i].pitch = (unsigned char) (i & 127), sv.note[i].len = 96, sv.note[i].vel = 100;
	r.srv.Offer (RMT_V_SEQ, &sv, r.now += 5);
	r.run (10);
	r.pipe.sendMax = 0;
	rmt_seqpos_view_t pos = {0, 0};
	int encodes = 0, loops = 0;
	bool bNew = false;
	for (int ms = 0; ms < 2000; ms++)			// 2 s, 10 kernel loops a ms, as the kernel loop offers
	{
		for (int k = 0; k < 10; k++, loops++)
		{
			pos.pos32 = ms / 10;
			r.srv.Offer (RMT_V_SEQPOS, &pos, r.now);
			if (ms % 50 == 0 && k == 0) bNew = true;
			if (bNew || r.srv.Due (RMT_V_SEQ))
			{
				if (r.srv.Offer (RMT_V_SEQ, &sv, r.now)) bNew = false, encodes++;
			}
			if (ms % 50 == 0 && k == 0) for (int j = 0; j < 1000; j++) sv.note[j].vel = (unsigned char) ((ms + j) & 127);
		}
		r.now++;
		if (ms % 5 == 0) r.srv.Pump (r.now);
	}
	CHECK (r.srv.Connected ());
	CHECK (encodes <= 2000 / 100 + 2);			// SEQ is subscribed at 10 Hz: at most one build per period, not one per loop
	CHECK (r.srv.Sess ().Resyncs () == 0);			// no resync while the ring stays full
	r.pipe.sendMax = 1u << 30;
	r.run (200);
	const uint32_t rs0 = r.srv.Sess ().Resyncs ();
	for (int i = 0; i < 20; i++)
	{
		if (r.srv.Due (RMT_V_SEQ) || i == 0) r.srv.Offer (RMT_V_SEQ, &sv, r.now += 120);
		r.run (20);
	}
	CHECK (!r.srv.Due (RMT_V_SEQ));
	CHECK (r.srv.Sess ().Resyncs () - rs0 <= 1);		// the snapshot arrived once
	CHECK (r.cli.sq.n == sv.n && !memcmp (r.cli.sq.note, sv.note, 8 * (size_t) sv.n));
	r.srv.Release ();
}

// version skew: a Pi that does not have the views (an older one, or a build without core/seq) does not list them; asking for one anyway is refused
static bool offers_no_seq (void *, int view) { return view != RMT_V_SEQ && view != RMT_V_SEQPOS && view != RMT_V_PAT; }

static void test_views_not_offered (void)
{
	static Refs refs;
	Hooks hk = hooks ();
	hk.offers = offers_no_seq;
	static Server srv (hk, &refs);
	static Pipe pipe;
	rmt_cli_t cli;
	struct Cap { Pipe *p; } cap = {&pipe};
	rmt_cli_cfg_t cfg = {[] (void *ud, const uint8_t *d, size_t n) -> int { Cap *c = (Cap *) ud; c->p->c2s.insert (c->p->c2s.end (), d, d + n); return 0; }, &cap, 1, 1280, 720, 1, g_Code, 0, 0};
	rmt_cli_init (&cli, &cfg);
	rmt_cli_want (&cli, RMT_V_VOICE, 10);
	rmt_cli_want (&cli, RMT_V_SEQ, 10);
	uint8_t nonce[16] = {1};
	CHECK (srv.Accept (&pipe, 1000, 5, nonce, false, "old", 8));
	rmt_cli_connected (&cli, 1000);
	uint32_t now = 1000;
	for (int i = 0; i < 6; i++)
	{
		now += 5;
		srv.Pump (now);
		if (!pipe.s2c.empty ()) rmt_cli_feed (&cli, pipe.s2c.data (), pipe.s2c.size (), now), pipe.s2c.clear ();
	}
	CHECK (rmt_cli_ready (&cli) && rmt_cli_view_ok (&cli, RMT_V_VOICE) && rmt_cli_view_ok (&cli, RMT_V_INSFX) && rmt_cli_view_ok (&cli, RMT_V_STATUS));
	CHECK (!rmt_cli_view_ok (&cli, RMT_V_SEQ) && !rmt_cli_view_ok (&cli, RMT_V_SEQPOS) && !rmt_cli_view_ok (&cli, RMT_V_PAT));	// greyed, never subscribed
	CHECK (refs.On (RMT_V_VOICE) && !refs.On (RMT_V_SEQ));
	// a client that asks anyway is told no and holds nothing
	uint8_t f[32], sub[2] = {RMT_V_SEQ, 0};
	size_t n = rmt_frame (f, sizeof f, RMT_SUB, 50, sub, 2);
	pipe.c2s.insert (pipe.c2s.end (), f, f + n);
	srv.Pump (now += 5);
	CHECK (pipe.s2c.size () >= RMT_HDR && pipe.s2c[0] == RMT_ERR && !refs.On (RMT_V_SEQ) && !srv.Sess ().Subscribed (RMT_V_SEQ));
	srv.Release ();
}

static void test_dead_link (void)
{
	static Rig r;
	r.connect ();
	r.run (3);
	size_t before = r.pipe.s2c.size ();
	(void) before;
	// the client answers pings: it lives; a client that stays silent is dropped after three missed pings
	for (int i = 0; i < 30 && r.srv.Connected (); i++)
	{
		bool ok = r.step (1000);				// the client library answers the PING itself
		(void) ok;
	}
	CHECK (r.srv.Connected () && rmt_cli_ready (&r.cli));
	// now it goes quiet (the cable is out; no RST ever comes)
	bool alive = true;
	uint32_t silent = 0;
	while (alive && silent < 60000)
	{
		r.now += 1000, silent += 1000;
		alive = r.srv.Pump (r.now);
		r.pipe.s2c.clear ();
	}
	CHECK (!alive && silent >= 15000 && silent <= 20000);
	r.srv.Release ();
	CHECK (!g_Log.empty ());
	// an unanswered HELLO is dropped after 10 s
	static Rig q;
	q.connect ();
	bool up = true;
	uint32_t t = 0;
	rmt_cli_lost (&q.cli);
	q.pipe.c2s.clear ();
	while (up && t < 20000) { q.now += 500, t += 500; up = q.srv.Pump (q.now); }
	CHECK (!up && t >= 10000 && t <= 10500);
	q.srv.Release ();
	// a peer that dribbles a byte now and then (so it is never silent) is still cut at the absolute deadline
	static Rig d;
	d.connect (true);
	rmt_cli_lost (&d.cli);
	d.pipe.c2s.clear ();
	up = true, t = 0;
	while (up && t < 30000)
	{
		d.now += 500, t += 500;
		uint8_t pf[RMT_HDR + 4];
		size_t pn = rmt_frame (pf, sizeof pf, RMT_PING, 1, "abcd", 4);	// a PING: allowed, answered by nothing, never silent
		d.pipe.c2s.insert (d.pipe.c2s.end (), pf, pf + pn);
		up = d.srv.Pump (d.now);
	}
	CHECK (!up && t >= 10000 && t <= 10500);
	d.srv.Release ();
}

static void test_garbage (void)
{
	uint32_t seed = 99;
	for (int round = 0; round < 200; round++)
	{
		static Rig r;
		r.pipe = Pipe ();
		rmt_cli_init (&r.cli, &r.cfg);
		r.connect (round % 2 == 0);
		r.run (2);
		std::vector<uint8_t> g (300);
		for (auto &b : g) { seed = seed * 1664525u + 1013904223u; b = (uint8_t) (seed >> 16); }
		if (round % 4 == 1) g[0] = RMT_CMD, g[1] = 0, g[2] = 20, g[3] = 0;
		if (round % 4 == 2) { uint8_t f[RMT_MAX_FRAME]; size_t n = rmt_frame (f, sizeof f, RMT_HELLO, 1, "SCRM\x01\x00\x01\x00", 8); g.insert (g.begin (), f, f + n); }
		r.pipe.c2s.insert (r.pipe.c2s.end (), g.begin (), g.end ());
		for (int i = 0; i < 20 && r.srv.Connected (); i++) { r.now += 100; if (!r.srv.Pump (r.now)) break; }
		r.pipe.s2c.clear ();
		r.srv.Release ();
		CHECK (!r.refs.On (RMT_V_VOICE) && !r.refs.On (RMT_V_INSFX));
	}
	for (const std::string &c : g_Cmds) CHECK (CommandAllowed (c.c_str ()));	// whatever got through is on the list
}

static void test_cost (void)
{
	// what the encoder costs per frame on this machine (the Pi's cores are several times slower: docs/remote-ui.md)
	static Rig r;
	subscribe_voice (r);
	subscribe_seq (r);
	r.connect ();
	r.run (4);
	static sidecar_voice_view_t v;
	static sidecar_insfx_view_t iv;
	static sidecar_mfx_view_t mv;
	static sidecar_status_view_t sv;
	static sidecar_seq_view_t qv;
	static rmt_seqpos_view_t pv;
	static rmt_pat_view_t tv;
	for (unsigned i = 0; i < sizeof qv; i++) ((uint8_t *) &qv)[i] = (uint8_t) (i * 13);
	qv.n = 1000;
	pv.pos32 = 99;
	for (unsigned i = 0; i < sizeof tv; i++) ((uint8_t *) &tv)[i] = (uint8_t) (i * 17);
	tv.pat.nlocks = 72;
	for (unsigned i = 0; i < sizeof v; i++) ((uint8_t *) &v)[i] = (uint8_t) (i * 7);
	for (unsigned i = 0; i < sizeof iv; i++) ((uint8_t *) &iv)[i] = (uint8_t) (i * 5);
	for (unsigned i = 0; i < sizeof mv; i++) ((uint8_t *) &mv)[i] = (uint8_t) (i * 3);
	for (unsigned i = 0; i < sizeof sv; i++) ((uint8_t *) &sv)[i] = (uint8_t) (i * 11);
	struct { int id; const void *p; const char *n; } views[] = {{RMT_V_VOICE, &v, "voice"}, {RMT_V_INSFX, &iv, "insfx"}, {RMT_V_MFX, &mv, "mfx"}, {RMT_V_STATUS, &sv, "status"}, {RMT_V_SEQ, &qv, "seq"}, {RMT_V_SEQPOS, &pv, "seqpos"}, {RMT_V_PAT, &tv, "pat"}};
	const int N = 2000;
	for (auto &w : views)
	{
		struct timespec a, b;
		double unchanged_us, changed_us;
		static uint8_t mut[16384];
		memcpy (mut, w.p, rmt_view_info (w.id)->size);
		r.srv.Offer (w.id, mut, r.now += 10000);	// the first full send
		r.pipe.s2c.clear ();
		r.srv.Pump (r.now);
		r.pipe.s2c.clear ();
		clock_gettime (CLOCK_MONOTONIC, &a);
		for (int i = 0; i < N; i++) r.srv.Offer (w.id, mut, r.now += 1000);			// nothing changed: encode + hash only
		clock_gettime (CLOCK_MONOTONIC, &b);
		unchanged_us = ((b.tv_sec - a.tv_sec) * 1e9 + (double) (b.tv_nsec - a.tv_nsec)) / 1e3 / N;
		r.srv.Pump (r.now);
		r.pipe.s2c.clear ();
		clock_gettime (CLOCK_MONOTONIC, &a);
		for (int i = 0; i < N; i++) { mut[8 + (i & 7)]++; r.srv.Offer (w.id, mut, r.now += 1000); if ((i & 15) == 0) { r.srv.Pump (r.now); r.pipe.s2c.clear (); } }
		clock_gettime (CLOCK_MONOTONIC, &b);
		changed_us = ((b.tv_sec - a.tv_sec) * 1e9 + (double) (b.tv_nsec - a.tv_nsec)) / 1e3 / N;
		printf ("  encode %-6s: %.1f us unchanged, %.1f us with a changed section (Mac, -O2 + sanitizers)\n", w.n, unchanged_us, changed_us);
	}
	r.srv.Release ();
}

int main ()
{
	test_allowlist ();
	test_parts ();
	test_session ();
	test_commands ();
	test_drag_release ();
	test_auth ();
	test_one_client ();
	test_slow_reader ();
	test_seq_views ();
	test_seq_storm ();
	test_views_not_offered ();
	test_dead_link ();
	test_garbage ();
	test_cost ();
	puts ("test_remote_srv: ok");
	return 0;
}

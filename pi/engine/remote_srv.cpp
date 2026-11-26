// pi/engine/remote_srv.cpp - see remote_srv.h
#include "remote_srv.h"
#include "../ui/remote/rmt_sha256.h"

#include <string.h>

namespace remote {

// ---- the allowlist ---------------------------------------------------------------------------------------------------

static bool IsDigit (char c)	{ return c >= '0' && c <= '9'; }

// The grammar of the lines the pages emit (pi/ui/sidecar_voice.c, sidecar_insfx.c, sidecar_mfx.c, sidecar_seqed.c), nothing wider:
//   vparam=T:K:V[:0]            T 0..11, K 0..63, V 0..127 (":0": a live line of a drag, the lift's line has none)
//   out=Vn:pre|post|trk         n 1..8
//   out=Vn:lev|pan|wid:V
//   ins=Vn:A|B:type:NAME        NAME: 1..10 of A-Z 0-9
//   ins=Vn:A|B:pK:V             K 1..4
//   ins=Vn:send:B:V             B 1..3
//   mfx=byp:0|1    mfx=src:ret|mix    mfx=lev:V
//   mfx=S:type:NAME  mfx=S:pair:0|1  mfx=S:mix:V  mfx=S:pK:V       S 1..4, K 1..4
// The SEQ page (t the Rytm's track 0..11 in seqed=, T the page's track 1..12 in seq=; s a step 0..127, S 1..128; P a MIDI note 0..127; M micro -96..96; V velocity
// 0..127; L length 0..12288 in 1/96 step; every number is plain digits, at most 5, a minus only where the range goes below 0):
//   seqed=add:t:s:P:M:V:L   seqed=del:t:s:P   seqed=move:t:s:P:s2:P2:M2   seqed=len:t:s:P:L   seqed=vel:t:s:P:V
//   seqed=chord:t:s:ROOT:CHD:INV:V:L:STRUM    CHD 0..15, INV 0..3, STRUM -96..96
//   seqed=strum:t:s0:s1:A    A -96..96            seqed=human:t:s0:s1:TIME:VEL:SEED     TIME 0..96, VEL 0..127, SEED 0..65535
//   seqed=undo:t   seqed=redo:t   seqed=show:t
//   seq=key:ROOT:SCALE:QNT         ROOT C C# D D# E F F# G G# A A# B, SCALE CHR MAJ MIN DOR PENT PHR LYD MIX LOC HMIN MMIN PMAJ BLUES WHOLE, QNT off|edit|play
//   seq=spd:T:W    W follow 1/8 1/4 1/2 3/4 3/2 2 2/3 5/4 7/8       seq=dir:T:W   W fwd rev pend ping rnd brown
//   seq=rnd:T:mode=off|steps|track,oct=1..4,wght=flat|root|chord|low|high|step
//   seq=alock:T:S:F:I   F 0..4, I -1..7             seq=rtrg:T:S:LINE   LINE one of the eight the RT key sends (off, cnt=2,mode=chord,rate=auto ...)
//   seq=fx:T:S:1:off|OCT:1|OCT:-1|RND:2|JMP:1|SKP:1       seq=lcond:T:S:all:---|50%|1:2|fill
//   seq=slide:T:S:all:0|1   seq=rndstep:T:S:0|1
//   seq=seed:free|N (N 0..9999)   seq=print:T   seq=swing:T:N (50..80)   seq=poly:T:voices=N (0..8)|alloc=reassign|rotate|reset|unison=N (1..4)
// Anything else (reset, a verb of the console, a longer tail, a NUL inside) is refused. scene= is not on a page of the remote yet.
namespace {
struct P
{
	const char *p;
	bool lit (const char *s)	{ size_t n = strlen (s); if (strncmp (p, s, n) != 0) return false; p += n; return true; }
	bool num (int nMax, int *pv, int nDigits = 3)	// digits (at most nDigits), value <= nMax
	{
		int v = 0, n = 0;
		while (IsDigit (*p)) { if (++n > nDigits) return false; v = v * 10 + (*p++ - '0'); }
		if (n == 0 || v > nMax) return false;
		*pv = v;
		return true;
	}
	bool rng (int nMin, int nMax, int *pv)		// lo..hi, a minus only when lo < 0, at most 5 digits
	{
		bool neg = false;
		if (nMin < 0 && *p == '-') neg = true, p++;
		int v;
		if (!num (neg ? -nMin : nMax, &v, 5)) return false;
		v = neg ? -v : v;
		if (v < nMin || v > nMax) return false;
		*pv = v;
		return true;
	}
	bool col ()			{ return lit (":"); }
	bool end () const		{ return *p == 0; }
	bool name ()			// 1..10 of A-Z 0-9
	{
		int n = 0;
		while ((*p >= 'A' && *p <= 'Z') || IsDigit (*p)) { p++; if (++n > 10) return false; }
		return n > 0;
	}
	// one of the words (a null-terminated list), whole: up to the separator sep (or the end of the line when sep is 0)
	bool word (const char *const *w, char sep)
	{
		const char *e = p;
		while (*e && *e != sep) e++;
		for (; *w; w++)
			if (strlen (*w) == (size_t) (e - p) && strncmp (*w, p, (size_t) (e - p)) == 0) { p = e; return true; }
		return false;
	}
};

// the fields of a line, joined by ':', from a spec: t track 0..11, T track 1..12, s step 0..127, S step 1..128, p pitch, m micro, v velocity,
// l length, c chord 0..15, i inversion 0..3, h human time 0..96, r seed 0..65535 (the human velocity is a v)
bool Fields (P &q, const char *spec)
{
	int v;
	for (const char *s = spec; *s; s++)
	{
		if (s != spec && !q.col ()) return false;
		bool ok;
		switch (*s)
		{
		case 't': ok = q.num (11, &v); break;
		case 'T': ok = q.num (12, &v) && v >= 1; break;		// (seq=: the page's number, Rytm track + 1)
		case 's': ok = q.num (127, &v); break;
		case 'S': ok = q.num (128, &v) && v >= 1; break;
		case 'p': ok = q.num (127, &v); break;
		case 'm': ok = q.rng (-96, 96, &v); break;
		case 'v': ok = q.num (127, &v); break;
		case 'l': ok = q.num (12288, &v, 5); break;
		case 'c': ok = q.num (15, &v); break;
		case 'i': ok = q.num (3, &v); break;
		case 'h': ok = q.num (96, &v); break;
		case 'r': ok = q.num (65535, &v, 5); break;
		default: ok = false;
		}
		if (!ok) return false;
	}
	return true;
}

const char *const kRoots[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", 0};
const char *const kScales[] = {"CHR", "MAJ", "MIN", "DOR", "PENT", "PHR", "LYD", "MIX", "LOC", "HMIN", "MMIN", "PMAJ", "BLUES", "WHOLE", 0};
const char *const kQnts[] = {"off", "edit", "play", 0};
const char *const kSpds[] = {"follow", "1/8", "1/4", "1/2", "3/4", "3/2", "2", "2/3", "5/4", "7/8", 0};
const char *const kDirs[] = {"fwd", "rev", "pend", "ping", "rnd", "brown", 0};
const char *const kRndModes[] = {"off", "steps", "track", 0};
const char *const kWghts[] = {"flat", "root", "chord", "low", "high", "step", 0};
const char *const kRtLines[] = {"off", "cnt=2,mode=chord,rate=auto", "cnt=3,mode=chord,rate=auto", "cnt=4,mode=chord,rate=auto",
				"cnt=8,mode=chord,rate=auto", "cnt=0,mode=up,rate=1/4,oct=2", "cnt=0,mode=updn,rate=1/4,oct=2",
				"cnt=8,mode=rnd,rate=1/4,oct=2", 0};
const char *const kFxLines[] = {"off", "OCT:1", "OCT:-1", "RND:2", "JMP:1", "SKP:1", 0};
const char *const kConds[] = {"---", "50%", "1:2", "fill", 0};
const char *const kAllocs[] = {"reassign", "rotate", "reset", 0};

bool SeqedAllowed (P &q)	// after "seqed="
{
	if (q.lit ("add:"))	return Fields (q, "tspmvl") && q.end ();
	if (q.lit ("del:"))	return Fields (q, "tsp") && q.end ();
	if (q.lit ("move:"))	return Fields (q, "tspspm") && q.end ();
	if (q.lit ("len:"))	return Fields (q, "tspl") && q.end ();
	if (q.lit ("vel:"))	return Fields (q, "tspv") && q.end ();
	if (q.lit ("chord:"))	return Fields (q, "tspcivlm") && q.end ();
	if (q.lit ("strum:"))	return Fields (q, "tssm") && q.end ();
	if (q.lit ("human:"))	return Fields (q, "tsshvr") && q.end ();
	if (q.lit ("undo:") || q.lit ("redo:") || q.lit ("show:"))	return Fields (q, "t") && q.end ();
	return false;
}

bool SeqAllowed (P &q)		// after "seq="
{
	int v;
	if (q.lit ("key:"))	return q.word (kRoots, ':') && q.col () && q.word (kScales, ':') && q.col () && q.word (kQnts, 0) && q.end ();
	if (q.lit ("spd:"))	return Fields (q, "T") && q.col () && q.word (kSpds, 0) && q.end ();
	if (q.lit ("dir:"))	return Fields (q, "T") && q.col () && q.word (kDirs, 0) && q.end ();
	if (q.lit ("rnd:"))
		return Fields (q, "T") && q.col () && q.lit ("mode=") && q.word (kRndModes, ',') && q.lit (",oct=") && q.num (4, &v, 1) && v >= 1 &&
		       q.lit (",wght=") && q.word (kWghts, 0) && q.end ();
	if (q.lit ("alock:"))	return Fields (q, "TS") && q.col () && q.num (4, &v, 1) && q.col () && q.rng (-1, 7, &v) && q.end ();
	if (q.lit ("rtrg:"))	return Fields (q, "TS") && q.col () && q.word (kRtLines, 0) && q.end ();
	if (q.lit ("fx:"))	return Fields (q, "TS") && q.lit (":1:") && q.word (kFxLines, 0) && q.end ();
	if (q.lit ("lcond:"))	return Fields (q, "TS") && q.lit (":all:") && q.word (kConds, 0) && q.end ();
	if (q.lit ("slide:"))	return Fields (q, "TS") && q.lit (":all:") && (q.lit ("0") || q.lit ("1")) && q.end ();
	if (q.lit ("rndstep:"))	return Fields (q, "TS") && q.col () && (q.lit ("0") || q.lit ("1")) && q.end ();
	if (q.lit ("seed:"))	return q.lit ("free") ? q.end () : q.num (9999, &v, 4) && q.end ();
	if (q.lit ("print:"))	return Fields (q, "T") && q.end ();
	if (q.lit ("swing:"))	return Fields (q, "T") && q.col () && q.rng (50, 80, &v) && q.end ();
	if (q.lit ("poly:"))
	{
		if (!Fields (q, "T") || !q.col ()) return false;
		if (q.lit ("voices="))	return q.num (8, &v, 1) && q.end ();
		if (q.lit ("alloc="))	return q.word (kAllocs, 0) && q.end ();
		if (q.lit ("unison="))	return q.num (4, &v, 1) && v >= 1 && q.end ();
	}
	return false;
}
}

bool CommandAllowed (const char *line)
{
	if (line == 0) return false;
	size_t n = strlen (line);
	if (!rmt_cmd_ok (line, n)) return false;
	P q = {line};
	int a, b, v;
	if (q.lit ("seqed="))	return SeqedAllowed (q);
	if (q.lit ("seq="))	return SeqAllowed (q);
	if (q.lit ("vparam="))
	{
		if (!q.num (11, &a) || !q.lit (":") || !q.num (63, &b) || !q.lit (":") || !q.num (127, &v)) return false;
		return q.end () || (q.lit (":0") && q.end ());
	}
	if (q.lit ("out=V"))
	{
		if (!q.num (8, &a) || a < 1 || !q.lit (":")) return false;
		if (q.lit ("pre") || q.lit ("post") || q.lit ("trk")) return q.end ();
		if (q.lit ("lev:") || q.lit ("pan:") || q.lit ("wid:")) return q.num (127, &v) && q.end ();
		return false;
	}
	if (q.lit ("ins=V"))
	{
		if (!q.num (8, &a) || a < 1 || !q.lit (":")) return false;
		if (q.lit ("send:")) return q.num (3, &b) && b >= 1 && q.lit (":") && q.num (127, &v) && q.end ();
		if (!(q.lit ("A:") || q.lit ("B:"))) return false;
		if (q.lit ("type:")) return q.name () && q.end ();
		return q.lit ("p") && q.num (4, &b) && b >= 1 && q.lit (":") && q.num (127, &v) && q.end ();
	}
	if (q.lit ("mfx="))
	{
		if (q.lit ("byp:")) return (q.lit ("0") || q.lit ("1")) && q.end ();
		if (q.lit ("src:")) return (q.lit ("ret") || q.lit ("mix")) && q.end ();
		if (q.lit ("lev:")) return q.num (127, &v) && q.end ();
		if (!q.num (4, &a) || a < 1 || !q.lit (":")) return false;
		if (q.lit ("type:")) return q.name () && q.end ();
		if (q.lit ("pair:")) return (q.lit ("0") || q.lit ("1")) && q.end ();
		if (q.lit ("mix:")) return q.num (127, &v) && q.end ();
		return q.lit ("p") && q.num (4, &b) && b >= 1 && q.lit (":") && q.num (127, &v) && q.end ();
	}
	return false;
}

// ---- the ring ------------------------------------------------------------------------------------------------------

bool Ring::Put (const uint8_t *p, size_t n)
{
	if (n > kSize - m_nUsed) return false;
	size_t tail = (m_nHead + m_nUsed) % kSize;
	size_t k = kSize - tail < n ? kSize - tail : n;
	memcpy (m_Buf + tail, p, k);
	if (n > k) memcpy (m_Buf, p + k, n - k);
	m_nUsed += n;
	return true;
}

size_t Ring::Peek (const uint8_t **pp) const
{
	*pp = m_Buf + m_nHead;
	return kSize - m_nHead < m_nUsed ? kSize - m_nHead : m_nUsed;
}

void Ring::Drop (size_t n)
{
	m_nHead = (m_nHead + n) % kSize;
	m_nUsed -= n;
	if (m_nUsed == 0) m_nHead = 0;
}

// ---- the session -------------------------------------------------------------------------------------------------

static const uint32_t kPingMs = RMT_PING_S * 1000u, kDeadMs = 3 * kPingMs + 2000u, kHelloMs = 10000u;
static const unsigned kCmdPerS = 120, kCmdBurst = 150;

void Session::Open (uint32_t now_ms, uint32_t session, const uint8_t nonce[RMT_NONCE], bool auth, const char *fw, int page, Ring *out)
{
	rmt_rx_init (&m_Rx);
	m_pOut = out;
	m_State = kHello;
	m_bAuth = auth, m_bLost = false, m_bBye = false;
	m_nOpenAt = now_ms;
	m_nSession = session, m_nTxSeq = 0, m_nLastRx = m_nLastPing = now_ms, m_nFrames = 0, m_nResync = 0;
	memcpy (m_Nonce, nonce, RMT_NONCE);
	strncpy (m_Fw, fw ? fw : "", sizeof m_Fw - 1);
	m_Fw[sizeof m_Fw - 1] = 0;
	m_nPage = page;
	memset (m_Rate, 0, sizeof m_Rate), memset (m_nLast, 0, sizeof m_nLast), memset (m_bSent, 0, sizeof m_bSent);
	memset (m_Held, 0, sizeof m_Held);
	for (int v = 0; v < RMT_V_COUNT; v++) rmt_vtx_reset (&m_Vtx[v]);
	m_nTokens = (uint64_t) kCmdBurst * 1000u, m_nTokAt = now_ms;
}

bool Session::Put (int type, uint32_t seq, const void *p, size_t n)
{
	uint8_t buf[RMT_MAX_FRAME];
	size_t fl = rmt_frame (buf, sizeof buf, type, seq, p, n);
	if (fl == 0 || m_pOut == 0 || !m_pOut->Put (buf, fl)) { m_bLost = true; return false; }
	m_nFrames++;
	return true;
}

void Session::Ok (uint32_t seq)	{ uint8_t b[4] = {(uint8_t) seq, (uint8_t) (seq >> 8), (uint8_t) (seq >> 16), (uint8_t) (seq >> 24)}; Put (RMT_OK, ++m_nTxSeq, b, 4); }

void Session::Err (uint32_t seq, int code, const char *text)
{
	uint8_t b[6 + 48];
	rmt_w w;
	rmt_w_init (&w, b, sizeof b);
	rmt_w_u32 (&w, seq), rmt_w_u16 (&w, (unsigned) code);
	size_t n = strlen (text);
	rmt_w_bytes (&w, text, n > 48 ? 48 : n);
	Put (RMT_ERR, ++m_nTxSeq, b, w.n);
}

void Session::Bye (int code, const char *text)
{
	uint8_t b[2 + 48];
	rmt_w w;
	rmt_w_init (&w, b, sizeof b);
	rmt_w_u16 (&w, (unsigned) code);
	size_t n = strlen (text);
	rmt_w_bytes (&w, text, n > 48 ? 48 : n);
	Put (RMT_BYE, ++m_nTxSeq, b, w.n);
	m_bBye = true;
}

bool Session::OnHello (const rmt_hdr_t &h, rmt_r *r)
{
	(void) h;
	char magic[4];
	rmt_r_bytes (r, magic, 4);
	unsigned pmin = rmt_r_u16 (r), pmax = rmt_r_u16 (r);
	if (r->err || memcmp (magic, "SCRM", 4) != 0) { Bye (RMT_E_BAD, "not a remote UI client"); return false; }
	if (pmin > RMT_PROTO || pmax < RMT_PROTO) { Bye (RMT_E_VERSION, "update the app or the Sidecar"); return false; }
	uint8_t b[256];
	rmt_w w;
	rmt_w_init (&w, b, sizeof b);
	rmt_w_u16 (&w, RMT_PROTO), rmt_w_u16 (&w, m_bAuth ? 1 : 0), rmt_w_u32 (&w, m_nSession);
	rmt_w_bytes (&w, m_Nonce, RMT_NONCE);
	rmt_w_u32 (&w, rmt_schema_hash ()), rmt_w_u8 (&w, (unsigned) m_nPage);
	size_t fl = strlen (m_Fw);
	rmt_w_u8 (&w, (unsigned) fl), rmt_w_bytes (&w, m_Fw, fl);
	unsigned nViews = 0;
	for (int v = 1; v < RMT_V_COUNT; v++) nViews += Offers (v);
	rmt_w_u8 (&w, nViews);
	for (int v = 1; v < RMT_V_COUNT; v++)
	{
		if (!Offers (v)) continue;
		const rmt_view_info_t *vi = rmt_view_info (v);
		rmt_w_u8 (&w, vi->id), rmt_w_u16 (&w, vi->schema), rmt_w_u8 (&w, vi->rate_hz);
	}
	if (!Put (RMT_WELCOME, ++m_nTxSeq, b, w.n)) return false;
	m_State = m_bAuth ? kAuth : kReady;
	return true;
}

bool Session::OnAuth (const rmt_hdr_t &h, rmt_r *r)
{
	uint8_t proof[32];
	rmt_r_bytes (r, proof, sizeof proof);
	const char *code = m_H.code ? m_H.code (m_H.ud) : 0;
	uint8_t want[32];
	bool ok = !r->err && code != 0 && *code != 0;
	if (ok) rmt_auth_proof (code, m_Nonce, m_nSession, want);
	ok = ok && rmt_ct_equal (proof, want, sizeof want);
	memset (want, 0, sizeof want);
	if (!ok)
	{
		m_pLim->Fail (m_nLastRx);
		Err (h.seq, RMT_E_AUTH, "wrong proof");
		Bye (RMT_E_AUTH, "wrong proof");
		if (m_H.log) m_H.log (m_H.ud, "remote: a client's pairing proof was wrong");
		return false;
	}
	m_pLim->Ok ();
	m_State = kReady;
	Ok (h.seq);
	return true;
}

// A vparam= line (already allowed) as its parts: false when it is not one. bFinal: no ":0", the lift's line.
static bool ParseVparam (const char *line, int *t, int *k, int *v, bool *bFinal)
{
	P q = {line};
	if (!q.lit ("vparam=") || !q.num (11, t) || !q.lit (":") || !q.num (63, k) || !q.lit (":") || !q.num (127, v)) return false;
	*bFinal = q.end ();
	return true;
}

// true when this line is the lift of a drag that is held (a start line that was accepted earlier)
bool Session::IsHeldLift (const char *line) const
{
	int t, k, v;
	bool bFinal;
	if (!ParseVparam (line, &t, &k, &v, &bFinal) || !bFinal) return false;
	for (unsigned i = 0; i < sizeof m_Held / sizeof m_Held[0]; i++)
		if (m_Held[i].used && m_Held[i].track == t && m_Held[i].k == k) return true;
	return false;
}

// the drag a vparam= line starts or ends: held until its final line arrives. Called only for a line that was accepted (passed the
// limiter and goes to the engine): a refused start is never held.
void Session::Track (const char *line)
{
	int t, k, v;
	bool bFinal;
	if (!ParseVparam (line, &t, &k, &v, &bFinal)) return;
	Held *pFree = 0, *pAt = 0;
	for (unsigned i = 0; i < sizeof m_Held / sizeof m_Held[0]; i++)
	{
		if (m_Held[i].used && m_Held[i].track == t && m_Held[i].k == k) pAt = &m_Held[i];
		if (!m_Held[i].used && !pFree) pFree = &m_Held[i];
	}
	if (bFinal) { if (pAt) pAt->used = false; return; }
	if (!pAt) pAt = pFree;
	if (pAt) pAt->used = true, pAt->track = (uint8_t) t, pAt->k = (uint8_t) k, pAt->v = (uint8_t) v;
}

void Session::OnCmd (const rmt_hdr_t &h, const uint8_t *p)
{
	uint32_t el = m_nLastRx - m_nTokAt;			// the bucket: kCmdPerS a second, kCmdBurst at once
	m_nTokAt = m_nLastRx;
	m_nTokens += (uint64_t) el * kCmdPerS;
	if (m_nTokens > (uint64_t) kCmdBurst * 1000u) m_nTokens = (uint64_t) kCmdBurst * 1000u;
	char line[RMT_CMD_MAX + 1];
	if (h.len == 0 || h.len > RMT_CMD_MAX) { Err (h.seq, RMT_E_BAD, "bad line"); return; }
	memcpy (line, p, h.len);
	line[h.len] = 0;
	if (strlen (line) != h.len) { Err (h.seq, RMT_E_BAD, "bad line"); return; }	// a NUL inside: not a line
	if (!CommandAllowed (line)) { Err (h.seq, RMT_E_CMD, "not allowed"); return; }
	// the lift of a drag that is held is never refused for speed (refusing it would leave the Rytm's knob mid-gesture), but only a
	// hold that was ACCEPTED counts: a start line refused for speed is not held, so its final line is limited like any other
	if (!IsHeldLift (line) && m_nTokens < 1000) { Err (h.seq, RMT_E_RATE, "too fast"); return; }
	m_nTokens = m_nTokens >= 1000 ? m_nTokens - 1000 : 0;
	Track (line);
	if (m_H.command) m_H.command (m_H.ud, line);
	Ok (h.seq);
}

bool Session::OnFrame (const rmt_hdr_t &h, const uint8_t *p)
{
	rmt_r r;
	rmt_r_init (&r, p, h.len);
	if (m_State == kHello)
	{
		if (h.type == RMT_HELLO) return OnHello (h, &r);
		if (h.type == RMT_PING || h.type == RMT_PONG) return true;
		Bye (RMT_E_BAD, "HELLO first");
		return false;
	}
	if (m_State == kAuth)
	{
		if (h.type == RMT_AUTH) return OnAuth (h, &r);
		if (h.type == RMT_PING || h.type == RMT_PONG) return true;
		Bye (RMT_E_AUTH, "the proof first");
		return false;
	}
	switch (h.type)
	{
	case RMT_SUB:
	{
		int view = (int) rmt_r_u8 (&r);
		unsigned rate = rmt_r_u8 (&r);
		const rmt_view_info_t *vi = rmt_view_info (view);
		if (r.err || vi == 0 || !Offers (view)) { Err (h.seq, RMT_E_VIEW, "unknown view"); return true; }
		if (rate == 0) rate = vi->rate_hz;
		if (rate > 30) rate = 30;
		if (m_Rate[view] == 0) m_pRefs->Add (view);
		m_Rate[view] = (uint8_t) rate;
		m_bSent[view] = false;				// a full snapshot at the next feed
		rmt_vtx_reset (&m_Vtx[view]);
		Ok (h.seq);
		return true;
	}
	case RMT_UNSUB:
	{
		int view = (int) rmt_r_u8 (&r);
		if (!r.err && Subscribed (view)) { m_pRefs->Release (view); m_Rate[view] = 0; }
		Ok (h.seq);
		return true;
	}
	case RMT_CMD:
		OnCmd (h, p);
		return true;
	case RMT_PING:
	{
		uint8_t tok[4] = {0};
		memcpy (tok, p, h.len < 4 ? h.len : 4);
		Put (RMT_PONG, h.seq, tok, 4);
		return true;
	}
	case RMT_RESYNC:
		for (int v = 1; v < RMT_V_COUNT; v++) rmt_vtx_reset (&m_Vtx[v]), m_bSent[v] = false;
		return true;
	case RMT_BYE:
		return false;
	default:
		return true;					// PONG, and a type this build does not know: ignored
	}
}

bool Session::Feed (const uint8_t *p, size_t n, uint32_t now_ms)
{
	if (m_State == kClosed) return false;
	m_nLastRx = now_ms;
	while (n > 0)
	{
		size_t took = rmt_rx_put (&m_Rx, p, n);
		p += took, n -= took;
		if (m_Rx.err) { Bye (RMT_E_BAD, "bad frame"); return false; }
		rmt_hdr_t h;
		const uint8_t *pl;
		while (rmt_rx_next (&m_Rx, &h, &pl))
			if (!OnFrame (h, pl)) return false;
		if (took == 0) return false;
	}
	return !m_bBye;
}

bool Session::Tick (uint32_t now_ms)
{
	if (m_State == kClosed) return false;
	if (m_State != kReady) return (uint32_t) (now_ms - m_nOpenAt) < kHelloMs;	// an absolute deadline to be ready: a silent or dribbling peer cannot hold the slot
	if ((uint32_t) (now_ms - m_nLastRx) > kDeadMs)
	{
		if (m_H.log) m_H.log (m_H.ud, "remote: the client stopped answering");
		return false;
	}
	if ((uint32_t) (now_ms - m_nLastPing) >= kPingMs)
	{
		m_nLastPing = now_ms;
		uint8_t b[4] = {(uint8_t) ++m_nPingTok, (uint8_t) (m_nPingTok >> 8), 0, 0};
		Put (RMT_PING, ++m_nTxSeq, b, 4);
	}
	return true;
}

void Session::Emit (void *ud, int view, int sec, int inst, const uint8_t *data, int len)
{
	Session *s = (Session *) ud;
	uint8_t b[4 + RMT_SEC_BUF];
	b[0] = (uint8_t) view, b[1] = (uint8_t) sec, b[2] = (uint8_t) inst, b[3] = 0;
	memcpy (b + 4, data, (size_t) len);
	s->Put (RMT_VIEW, ++s->m_nTxSeq, b, 4 + (size_t) len);
}

bool Session::HasRoom () const
{
	if (m_pOut == 0) return false;
	if (m_pOut->Used () == 0) return true;
	size_t need = 0;					// a resync sends a snapshot of every subscribed view
	for (int v = 1; v < RMT_V_COUNT; v++)
	{
		const rmt_view_info_t *vi = m_Rate[v] ? rmt_view_info (v) : 0;
		if (vi != 0) need += vi->size;
	}
	return m_pOut->Free () >= (need < Ring::kSize ? need : Ring::kSize);
}

bool Session::Offer (int view, const void *v, uint32_t now_ms)
{
	if (m_State != kReady || !Subscribed (view)) return true;
	const uint32_t nPeriod = 1000u / m_Rate[view];
	if (m_bSent[view] && (uint32_t) (now_ms - m_nLast[view]) < nPeriod) return false;	// the rate holds during a loss too
	if (m_bLost)						// a frame did not fit the ring: everything again, every view, once the ring has room
	{
		if (!HasRoom ()) return false;
		for (int i = 1; i < RMT_V_COUNT; i++) rmt_vtx_reset (&m_Vtx[i]), m_bSent[i] = false;
		m_bLost = false, m_nResync++;
	}
	m_nLast[view] = now_ms, m_bSent[view] = true;
	rmt_vtx_emit (&m_Vtx[view], view, v, Emit, this, 0);
	return true;
}

void Session::SetPage (int page)
{
	if (page == m_nPage) return;
	m_nPage = page;
	if (m_State != kReady) return;
	uint8_t b = (uint8_t) page;
	Put (RMT_PAGE, ++m_nTxSeq, &b, 1);
}

void Session::Close ()
{
	if (m_State == kClosed) return;
	m_State = kClosed;
	for (int v = 1; v < RMT_V_COUNT; v++)
		if (m_Rate[v]) m_pRefs->Release (v), m_Rate[v] = 0;
	for (unsigned i = 0; i < sizeof m_Held / sizeof m_Held[0]; i++)	// the drags still held end here: the lift's line, as a finger would send
		if (m_Held[i].used)
		{
			char line[24];
			unsigned n = 0;
			const char *pre = "vparam=";
			for (; pre[n]; n++) line[n] = pre[n];
			unsigned vals[3] = {m_Held[i].track, m_Held[i].k, m_Held[i].v};
			for (int j = 0; j < 3; j++)
			{
				char d[4];
				int nd = 0;
				unsigned x = vals[j];
				do { d[nd++] = (char) ('0' + x % 10); x /= 10; } while (x);
				while (nd) line[n++] = d[--nd];
				if (j < 2) line[n++] = ':';
			}
			line[n] = 0;
			if (m_H.command) m_H.command (m_H.ud, line);
			m_Held[i].used = false;
		}
}

// ---- the server ---------------------------------------------------------------------------------------------------

static void SendBye (Conn *c, int code, const char *text)
{
	uint8_t b[2 + 32], f[RMT_HDR + 2 + 32];
	rmt_w w;
	rmt_w_init (&w, b, sizeof b);
	rmt_w_u16 (&w, (unsigned) code), rmt_w_bytes (&w, text, strlen (text));
	size_t n = rmt_frame (f, sizeof f, RMT_BYE, 1, b, w.n);
	if (n) c->Send (f, n);
}

bool Server::Accept (Conn *c, uint32_t now_ms, uint32_t session, const uint8_t nonce[RMT_NONCE], bool auth, const char *fw, int page)
{
	if (m_pConn != 0) { SendBye (c, RMT_E_BUSY, "another client is connected"); return false; }
	if (auth && !m_Lim.Allowed (now_ms)) { SendBye (c, RMT_E_AUTH, "locked for a minute"); return false; }
	m_pConn = c;
	m_Out.Clear ();
	m_Sess.Open (now_ms, session, nonce, auth, fw, page, &m_Out);
	return true;
}

bool Server::Pump (uint32_t now_ms)
{
	if (m_pConn == 0) return false;
	bool ok = true;
	uint8_t buf[512];
	for (int i = 0; i < 8 && ok; i++)
	{
		int n = m_pConn->Recv (buf, sizeof buf);
		if (n < 0) { ok = false; break; }
		if (n == 0) break;
		ok = m_Sess.Feed (buf, (size_t) n, now_ms);
	}
	if (ok) ok = m_Sess.Tick (now_ms);
	for (int i = 0; i < 16; i++)				// what is queued, a BYE included, goes out before the end
	{
		const uint8_t *p;
		size_t n = m_Out.Peek (&p);
		if (n == 0) break;
		int k = m_pConn->Send (p, n);
		if (k < 0) { ok = false; break; }
		if (k == 0) break;
		m_Out.Drop ((size_t) k);
	}
	return ok;
}

Conn *Server::Release ()
{
	m_Sess.Close ();
	Conn *c = m_pConn;
	m_pConn = 0;
	m_Out.Clear ();
	return c;
}

}	// namespace remote

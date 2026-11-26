// pi/engine/remote_srv.h - the remote UI's service, the portable part (docs/remote-ui.md): the protocol session of one
// client, the one-client-at-a-time server around it, the command allowlist, the pairing proof's rate limit and the
// reference counts that feed the views. No Circle, no heap, no <new>: remote_pi.cpp supplies the socket and the engine,
// tests/test_remote_srv.cpp a socket on the Mac and a fake engine.
//
// What it guarantees (each is a test):
//  - a command line is passed on only when its verb is on the allowlist (ins= out=V.. mfx= vparam= scene=) and well
//    formed; everything else gets an ERR and goes nowhere;
//  - the views are fed from the engine's own fills (Offer) at the subscription's rate, only the sections that changed;
//  - a client that leaves (BYE, the socket gone, three missed pings, a protocol error) releases its references and the
//    drag it still held (vparam=T:K:V:0 without its final line gets the final line): nothing sticks;
//  - a slow reader never blocks the engine: frames go through a ring, a full ring drops the frame and the client is sent
//    everything again on the next feed (a resync);
//  - the pairing code is only ever an input to the HMAC: it is not logged, not copied, not sent.
#ifndef _pi_engine_remote_srv_h
#define _pi_engine_remote_srv_h

#include <stddef.h>
#include <stdint.h>

#include "../ui/remote/rmt_proto.h"
#include "../ui/remote/rmt_views.h"

namespace remote {

// ---- the allowlist ---------------------------------------------------------------------------------------------------

// Exactly the lines the four pages emit, sub-verb by sub-verb (the grammar is in remote_srv.cpp): vparam=T:K:V[:0],
// out=Vn:pre|post|trk and out=Vn:lev|pan|wid:V, ins=Vn:A|B:type:NAME, ins=Vn:A|B:pK:V, ins=Vn:send:B:V, mfx=byp, src, lev,
// mfx=S:type|pair|mix|pK. Everything else (a reset, the console's other verbs, a longer tail, a NUL inside) is refused.
bool CommandAllowed (const char *line);

// ---- reference counts -------------------------------------------------------------------------------------------------

// Who wants a view fed: the local display adds its page's (sidecar_*_shown ()), the remote client its subscriptions.
// The engine feeds a view while On(). Core 0 only (cooperative tasks: no lock).
class Refs
{
public:
	void Add (int view)	{ if (view > 0 && view < RMT_V_COUNT) m_n[view]++; }
	void Release (int view)	{ if (view > 0 && view < RMT_V_COUNT && m_n[view]) m_n[view]--; }
	bool On (int view) const	{ return view > 0 && view < RMT_V_COUNT && m_n[view] != 0; }
	unsigned Count (int view) const	{ return view > 0 && view < RMT_V_COUNT ? m_n[view] : 0; }
private:
	unsigned m_n[RMT_V_COUNT] = {};
};

// ---- the pairing proof's rate limit ----------------------------------------------------------------------------------

// Five wrong proofs in a row lock the proof out for a minute (the code is eight digits: a try a second would take years).
class AuthLimiter
{
public:
	static const unsigned kMaxFails = 5, kLockMs = 60000;
	bool Allowed (uint32_t now_ms) const	{ return m_nFails < kMaxFails || (uint32_t) (now_ms - m_nAt) >= kLockMs; }
	void Fail (uint32_t now_ms)		{ if (m_nFails >= kMaxFails && Allowed (now_ms)) m_nFails = 0; m_nFails++, m_nAt = now_ms; }
	void Ok ()				{ m_nFails = 0; }
private:
	unsigned m_nFails = 0;
	uint32_t m_nAt = 0;
};

// ---- a byte ring for the frames waiting for the socket -----------------------------------------------------------------

class Ring
{
public:
	static const size_t kSize = 24 * 1024;
	bool Put (const uint8_t *p, size_t n);			// all of it or none (false: not enough room)
	size_t Peek (const uint8_t **pp) const;			// the contiguous bytes at the head
	void Drop (size_t n);
	size_t Used () const	{ return m_nUsed; }
	size_t Free () const	{ return kSize - m_nUsed; }
	void Clear ()		{ m_nHead = m_nUsed = 0; }
private:
	uint8_t m_Buf[kSize];
	size_t m_nHead = 0, m_nUsed = 0;
};

// ---- what a session needs from the engine ---------------------------------------------------------------------------

struct Hooks
{
	void *ud;
	void (*command) (void *ud, const char *line);		// an accepted line (CKernel::UiCommand's path)
	const char *(*code) (void *ud);				// the pairing code for the proof; 0: none (the proof then fails)
	void (*log) (void *ud, const char *what);		// one short line, never a secret
	bool (*offers) (void *ud, int view);			// this build has the view (WELCOME lists only those); 0: all of them
};

class Session
{
public:
	Session (const Hooks &h, Refs *refs, AuthLimiter *lim) : m_H (h), m_pRefs (refs), m_pLim (lim) {}
	// The connection opened. auth: the proof of the pairing code is required before anything is fed. fw: the build.
	void Open (uint32_t now_ms, uint32_t session, const uint8_t nonce[RMT_NONCE], bool auth, const char *fw, int page, Ring *out);
	// Bytes from the client; false: the connection must be closed (a BYE with the reason is queued if there is one)
	bool Feed (const uint8_t *p, size_t n, uint32_t now_ms);
	bool Tick (uint32_t now_ms);				// pings, the dead link; false: close
	// The engine's fresh copy of a view (a full struct): the changed sections go out, if subscribed and due
	// true: taken (sent as changed sections, or nothing to send: not subscribed); false: not now, the subscription's rate holds it back - a
	// builder that offers only when its view changed offers again until this is true
	bool Offer (int view, const void *v, uint32_t now_ms);
	// the client wants a build of this view whatever changed: it is subscribed and has had no snapshot yet (a new subscription, a RESYNC, a frame
	// that did not fit the ring). A change-driven builder (SEQ, PAT) offers when its view changed or this is true.
	// (after a loss only while the ring has room for the snapshots: a full ring waits for the client to drain)
	bool Due (int view) const	{ return m_State == kReady && Subscribed (view) && (m_bLost ? HasRoom () : !m_bSent[view]); }
	void SetPage (int page);				// the Pi's local page changed: a PAGE frame
	void Close ();						// releases the references, then the held drags (idempotent)
	bool Ready () const	{ return m_State == kReady; }
	bool Subscribed (int view) const	{ return view > 0 && view < RMT_V_COUNT && m_Rate[view] != 0; }
	uint32_t Frames () const	{ return m_nFrames; }
	uint32_t Resyncs () const	{ return m_nResync; }
private:
	enum State { kClosed, kHello, kAuth, kReady };
	struct Held { uint8_t track, k, v; bool used; };
	static void Emit (void *ud, int view, int sec, int inst, const uint8_t *data, int len);
	bool Put (int type, uint32_t seq, const void *p, size_t n);
	void Bye (int code, const char *text);
	void Ok (uint32_t seq);
	void Err (uint32_t seq, int code, const char *text);
	bool OnFrame (const rmt_hdr_t &h, const uint8_t *p);
	bool OnHello (const rmt_hdr_t &h, rmt_r *r);
	bool OnAuth (const rmt_hdr_t &h, rmt_r *r);
	void OnCmd (const rmt_hdr_t &h, const uint8_t *p);
	bool Offers (int view) const	{ return rmt_view_info (view) != 0 && (m_H.offers == 0 || m_H.offers (m_H.ud, view)); }
	bool HasRoom () const;				// the ring is empty, or has room for a snapshot of every subscribed view
	bool IsHeldLift (const char *line) const;
	void Track (const char *line);
	Hooks m_H;
	Refs *m_pRefs;
	AuthLimiter *m_pLim;
	Ring *m_pOut = 0;
	State m_State = kClosed;
	rmt_rx_t m_Rx;
	bool m_bAuth = false, m_bLost = false, m_bBye = false;
	uint32_t m_nOpenAt = 0, m_nSession = 0, m_nTxSeq = 0, m_nLastRx = 0, m_nLastPing = 0, m_nFrames = 0, m_nResync = 0;
	uint8_t m_Nonce[RMT_NONCE] = {};
	char m_Fw[24] = {};
	int m_nPage = 0;
	uint8_t m_Rate[RMT_V_COUNT] = {};			// 0: not subscribed
	uint32_t m_nLast[RMT_V_COUNT] = {};
	bool m_bSent[RMT_V_COUNT] = {};
	rmt_vtx_t m_Vtx[RMT_V_COUNT];
	Held m_Held[8] = {};
	uint64_t m_nTokens = 0;					// the command bucket (x 1000)
	uint32_t m_nTokAt = 0;
	uint32_t m_nPingTok = 0;
};

// ---- one client at a time, around a socket ----------------------------------------------------------------------------

// The socket as the server sees it: Circle's CSocket on the Pi, a POSIX socket in the tests
class Conn
{
public:
	virtual ~Conn () {}
	virtual int Recv (uint8_t *p, size_t n) = 0;		// bytes (> 0), none now (0), the peer closed or failed (< 0)
	virtual int Send (const uint8_t *p, size_t n) = 0;	// bytes taken (>= 0; 0: not now), failed (< 0)
};

class Server
{
public:
	Server (const Hooks &h, Refs *refs) : m_Sess (h, refs, &m_Lim) {}
	// A connection arrived (already checked to be on the USB-C interface). false: refused (the caller closes it): a
	// BYE(busy) has been sent to it when there is a client already, or the proof is locked out.
	bool Accept (Conn *c, uint32_t now_ms, uint32_t session, const uint8_t nonce[RMT_NONCE], bool auth, const char *fw, int page);
	// Receive, parse, send what is queued, ping; false: the connection ended (the caller closes and deletes it)
	bool Pump (uint32_t now_ms);
	bool Offer (int view, const void *v, uint32_t now_ms)	{ return m_pConn ? m_Sess.Offer (view, v, now_ms) : true; }
	bool Due (int view) const				{ return m_pConn != 0 && m_Sess.Due (view); }
	void SetPage (int page)					{ if (m_pConn) m_Sess.SetPage (page); }
	bool Connected () const					{ return m_pConn != 0; }
	Conn *Release ();					// the connection ends: the session closed, the Conn handed back
	Session &Sess ()					{ return m_Sess; }
private:
	AuthLimiter m_Lim;
	Session m_Sess;
	Ring m_Out;
	Conn *m_pConn = 0;
};

}	// namespace remote

#endif

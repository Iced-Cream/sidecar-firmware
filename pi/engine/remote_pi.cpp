// pi/engine/remote_pi.cpp - see remote_pi.h. The protocol, the allowlist, the delta engine and the one-client rule are in
// remote_srv.cpp (tested on the Mac); this file is the socket, the two tasks and the engine's few hooks.
#include "remote_pi.h"
#include "remote_srv.h"
#include "console.h"
#include "paircode_pi.h"
#include "usbnet_pi.h"
#include "xypad_pi.h"

#include <circle/logger.h>
#include <circle/net/in.h>
#include <circle/net/ipaddress.h>
#include <circle/net/socket.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>
#include <circle/timer.h>
#include <circle/util.h>

static const char From[] = "remote";
static const unsigned kTxSoft = 32 * 1024;		// bytes queued in TCP and unacknowledged: past this the client is behind, nothing more is sent now

// ---- the engine's side of the session ------------------------------------------------------------------------------

static CConsole *s_pConsole;

// An accepted line goes the way a touch on the Pi's screen sends it (CKernel::UiCommand): the console's parser, which only
// queues (core 3 applies it). Not Activity (): the Pi's own screen stays as it was. Logged unless it is a drag's line.
static void HookCommand (void *, const char *pLine)
{
	if (s_pConsole == 0) return;
	if (!xypad_pi::Quiet (pLine)) CLogger::Get ()->Write (From, LogNotice, "command \"%s\"", pLine);
	s_pConsole->Command (pLine);
}

static const char *HookCode (void *)
{
	return paircode_pi::Gate ().gated () && paircode_pi::Gate ().rng_ok () ? paircode_pi::Gate ().code () : 0;
}

static void HookLog (void *, const char *pWhat)	{ CLogger::Get ()->Write (From, LogNotice, "%s", pWhat); }

// the views this build has: the sequencer's need core/seq (SIDECAR_SEQED); the others are in every build
static bool HookOffers (void *, int nView)
{
#if SIDECAR_SEQED
	return true;
#else
	return nView != RMT_V_SEQ && nView != RMT_V_SEQPOS && nView != RMT_V_PAT;
#endif
}

// ---- a CSocket as the server's connection --------------------------------------------------------------------------------

class CSockConn : public remote::Conn
{
public:
	explicit CSockConn (CSocket *pSocket) : m_pSocket (pSocket) {}
	~CSockConn () override	{ delete m_pSocket; }
	int Recv (u8 *p, size_t n) override
	{
		int r = m_pSocket->Receive (p, (unsigned) n, MSG_DONTWAIT);
		return r;					// 0: nothing now, < 0: closed or failed
	}
	int Send (const u8 *p, size_t n) override
	{
		if (m_pSocket->GetBytesQueued () > kTxSoft) return 0;	// the client reads slowly: the ring waits (a full one resyncs)
		if (n > 1400) n = 1400;
		int r = m_pSocket->Send (p, (unsigned) n, MSG_DONTWAIT);
		return r < 0 ? -1 : (int) n;
	}
private:
	CSocket *m_pSocket;
};

// ---- the state -------------------------------------------------------------------------------------------------------

static remote::Refs s_Refs;
static boolean s_bLocal[RMT_V_COUNT];
static boolean s_bRunning;
static int s_nPage = -1;
static const char *s_pFw = "";
static remote::Server *s_pServer;

static u32 NowMs (void)	{ return (u32) (CTimer::GetClockTicks64 () / 1000); }	// (32 bits of ms wrap in 49 days: the session only subtracts)

// accepts: the first connection takes the service, a second is told it is busy and closed (remote_srv.cpp Server::Accept)
class CRemoteAccept : public CTask
{
public:
	CRemoteAccept (CNetSubSystem *pNet) : m_pNet (pNet) {}
	void Run (void) override
	{
		CScheduler *S = CScheduler::Get ();
		while (!m_pNet->IsRunning ()) S->MsSleep (100);
		CSocket Listen (m_pNet, IPPROTO_TCP);
		if (Listen.Bind (RMT_PORT) < 0 || Listen.Listen (2) < 0)
		{
			CLogger::Get ()->Write (From, LogError, "cannot listen on port %u", (unsigned) RMT_PORT);
			return;
		}
		CLogger::Get ()->Write (From, LogNotice, "the remote UI on TCP port %u, USB-C cable only%s", (unsigned) RMT_PORT,
					usbnet_pi::Trusted () ? " (trusted: no pairing code on it)" : paircode_pi::Gate ().gated () ? " (a client proves the pairing code)"
								       : " (NOT GATED: cmdline paircode=0)");
		for (;;)
		{
			CIPAddress IP;
			u16 nPort;
			CSocket *pConn = Listen.Accept (&IP, &nPort);
			if (pConn == 0) { S->MsSleep (100); continue; }
			// the rule is per connection: it arrived on the interface the stack runs on, and that must be the cable
			if (usbnet_pi::Iface () != usbnet::kIfUsb) { delete pConn; continue; }
			CSockConn *pC = new CSockConn (pConn);
			u8 Nonce[RMT_NONCE];
			u8 Sess[4];
			paircode_pi::Random (Nonce, sizeof Nonce);
			paircode_pi::Random (Sess, sizeof Sess);
			u32 nSession = (u32) Sess[0] | (u32) Sess[1] << 8 | (u32) Sess[2] << 16 | (u32) Sess[3] << 24;
			const bool bAuth = !usbnet_pi::Trusted () && paircode_pi::Gate ().gated ();
			if (s_pServer->Accept (pC, NowMs (), nSession, Nonce, bAuth, s_pFw, s_nPage < 0 ? 0 : s_nPage))
				CLogger::Get ()->Write (From, LogNotice, "a client connected");
			else delete pC;				// busy, or locked out: it has been told
		}
	}
private:
	CNetSubSystem *m_pNet;
};

// reads, parses, sends: every few ms, never blocking (a client that stops reading is the ring's and TCP's business, not core 0's)
class CRemoteServe : public CTask
{
public:
	void Run (void) override
	{
		CScheduler *S = CScheduler::Get ();
		for (;;)
		{
			if (!s_pServer->Connected ()) { S->MsSleep (20); continue; }
			if (!s_pServer->Pump (NowMs ()))
			{
				remote::Conn *pC = s_pServer->Release ();	// the views' references and a held drag's lift, then the socket
				delete pC;
				CLogger::Get ()->Write (From, LogNotice, "the client left");	// once; nothing of the exchange is logged
				continue;
			}
			S->MsSleep (5);
		}
	}
};

void remote_pi::Start (CNetSubSystem *pNet, CConsole *pConsole, const char *pFw)
{
	s_pFw = pFw;
	s_pConsole = pConsole;
	if (usbnet_pi::Iface () != usbnet::kIfUsb)		// Wi-Fi or Ethernet: the port is never opened
	{
		CLogger::Get ()->Write (From, LogNotice, "off: the network is not the USB-C cable");
		return;
	}
	static remote::Hooks Hk = {0, HookCommand, HookCode, HookLog, HookOffers};
	static remote::Server Server (Hk, &s_Refs);
	s_pServer = &Server;
	new CRemoteAccept (pNet);
	new CRemoteServe;
	s_bRunning = TRUE;
}

boolean remote_pi::Running (void)	{ return s_bRunning; }

boolean remote_pi::Gate (int nView, boolean bLocalShown)
{
	if (nView > 0 && nView < RMT_V_COUNT && (s_bLocal[nView] != 0) != (bLocalShown != 0))	// the display's edge: its reference
	{
		if (bLocalShown) s_Refs.Add (nView);
		else s_Refs.Release (nView);
		s_bLocal[nView] = bLocalShown;
	}
	return s_Refs.On (nView);
}

boolean remote_pi::Offer (int nView, const void *pView)
{
	return s_bRunning ? s_pServer->Offer (nView, pView, NowMs ()) : TRUE;
}

boolean remote_pi::Due (int nView)
{
	return s_bRunning && s_pServer->Due (nView);
}

void remote_pi::Page (int nPage)
{
	if (nPage == s_nPage) return;
	s_nPage = nPage;
	if (s_bRunning) s_pServer->SetPage (nPage);
}

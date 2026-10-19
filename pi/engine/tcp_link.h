// pi/engine/tcp_link.h - the engine's host over Ethernet: rytm-emu's Overbridge link (rytm-emu src/oblink.rs) as a
// transport (transport.h). Connects to emuhost (retrying every 2 s), says hello (the USB MIDI pair, no R frames,
// voice frames), answers each 'N' from the output stage ('a': the 8 voices as int16 in out=tracks; 'A': 12
// channels otherwise), passes the Rytm's MIDI ('M') to the control core and the engine's ('M') to the emulator.
// Nothing is sent before the hello's answer, and 'M'/'a' only when granted (the emulator drops a host otherwise).
// The connect runs in its own task (Circle's CSocket::Connect has no timeout: ~189 s to an absent peer), so core 0's
// loop and the watchdog keep running. Each Poll sends at most PollMessages messages or PollBudgetUs of sending,
// whichever ends first (the rest waits for the next Poll); Circle's send timeout is per segment (CSocket::Send
// splits at the MSS and each segment may wait SendTimeoutUs), so a single Send that took longer than SendSlowUs in
// all loses the link too. Worst case per Poll: one message's segments x SendTimeoutUs (12.3 KB at the 536-byte
// minimum MSS: 23 x 250 ms, ~6 s), well inside the 15 s watchdog.
#ifndef _tcp_link_h
#define _tcp_link_h
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/sched/task.h>
#include <circle/types.h>

#include "emu_proto.h"
#include "transport.h"

// One connect attempt, filled by its CConnectTask (which the scheduler deletes when Run returns: nothing refers to
// the task itself). The transport owns the slot; it adopts the socket when bDone, or deletes it when the attempt
// was abandoned meanwhile (SetPeer to another peer, Stop, :link=usb).
struct TConnectSlot
{
	volatile boolean bBusy;			// a task runs (or ran) for this slot; FALSE: free
	volatile boolean bDone;			// the task's last write: pSocket and nResult are set
	CSocket *pSocket;
	int nResult;				// CSocket::Connect's
	u8 Peer[4];
	u16 nPort;
};

class CConnectTask : public CTask
{
public:
	CConnectTask (CNetSubSystem *pNet, TConnectSlot *pSlot) : m_pNet (pNet), m_pSlot (pSlot) {}
	void Run (void) override;

private:
	CNetSubSystem *m_pNet;
	TConnectSlot *m_pSlot;
};

class CTcpTransport : public CTransport, private EmuParser::Sink
{
public:
	static const unsigned SendTimeoutUs = 250000;	// per segment: a TX queue full this long fails the Send
	static const unsigned SendSlowUs = 500000;	// one whole Send this long: the link is lost (watchdog: 15 s)
	static const unsigned PollMessages = 8;		// messages sent per Poll at most (the loop polls every ~1 ms)
	static const unsigned PollBudgetUs = 5000;	// no new message once this Poll has spent this long sending

	CTcpTransport (CNetSubSystem *pNet);
	void SetPeer (const u8 IP[4], u16 nPort);
	void Reap (void);				// core 0's loop, any link: closes abandoned connects as they finish
	const char *Name (void) const override		{ return "tcp"; }
	boolean Running (void) const override		{ return m_pSocket != 0 && m_bHello; }
	void Poll (void) override;
	void Stop (void) override;
	void LogStats (CLogger *pLogger) override;
	unsigned MIDIOutDropped (void) const override	{ return m_nDropped; }

private:
	// N before the hello's answer is kept and answered after it (nothing is sent before it); m_nWanted is clamped at MaxWanted (1 s)
	void Want (u32 nFrames) override;
	void Midi (const u8 *pData, int nLength) override;
	void Hello (u8 nGranted) override		{ m_nGranted = nGranted; m_bHello = TRUE; }
	boolean PollRoom (void) const;			// another message fits this Poll's bounds
	boolean SendAll (const void *pData, unsigned nLen, const char *pWhat);	// FALSE: the link is lost
	void StartConnect (void);
	void Adopt (CSocket *pSocket);
	void Lost (const char *pWhy);
	void SendFrames (void);
	void SendMIDI (void);

	CNetSubSystem *m_pNet;
	CSocket *m_pSocket;
	static const int Slots = 4;		// connects in flight: the current one and abandoned ones still blocked
	TConnectSlot m_Slot[Slots];
	int m_nConnecting;			// the current attempt's slot, -1 none
	EmuParser m_Parser;
	u8 m_Peer[4];
	u16 m_nPort;
	unsigned m_nNextTry;			// CTimer ticks
	boolean m_bHello;
	u8 m_nGranted;
	u32 m_nWanted;
	unsigned m_nWaitSince;			// clock ticks: frames wanted, none in the FIFO
	unsigned m_nPollStart;			// clock ticks: this Poll's first send
	unsigned m_nPollSent;			// messages sent this Poll
	u8 m_Rx[8192];
	u8 m_Tx[5 + 256 * 48];
	u8 m_Msg[3 + 4096];			// one 'M': the engine's record (g_ToHost) after its head
	// counters (frames and requests per second in LogStats, the rest since boot)
	unsigned m_nFrames, m_nRequests, m_nMidiIn, m_nMidiInLost, m_nMidiOut, m_nDropped, m_nConnects, m_nSilence,
		 m_nEarlyN, m_nClamped;
	unsigned m_nLastPoll, m_nPollGapMax, m_nWantedMax;	// per LogStats: the longest gap between Polls (us), the most
							// frames wanted at once (the emulator's bursts)
};

#endif

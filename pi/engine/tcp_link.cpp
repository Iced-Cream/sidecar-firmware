// pi/engine/tcp_link.cpp - see tcp_link.h. Core 0, task level (Circle's scheduler runs the network).
#include "tcp_link.h"

#include <circle/logger.h>
#include <circle/net/in.h>
#include <circle/net/ipaddress.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/util.h>

#include "output.h"
#include "shared.h"
#include "sidecar_gadget.h"

static const char From[] = "tcp";
static const u8 HelloMidi = 1, HelloNoR = 2, HelloVoices = 4;		// oblink.rs's hello bits
static const u8 HelloFlags = HelloMidi | HelloNoR | HelloVoices;
static const unsigned MaxFramesPerMessage = 256;			// the emulator drops a host above 65536
static const u32 MaxWanted = 48000;					// 1 s of requests: more is clamped (counted)

CTcpTransport::CTcpTransport (CNetSubSystem *pNet)
:	m_pNet (pNet), m_pSocket (0), m_nConnecting (-1), m_Parser (this), m_nPort (7301), m_nNextTry (0),
	m_bHello (FALSE), m_nGranted (0), m_nWanted (0), m_nWaitSince (0), m_nPollStart (0), m_nPollSent (0),
	m_nComputerIn (0), m_nComputerOut (0), m_nFrames (0), m_nRequests (0),
	m_nMidiIn (0), m_nMidiInLost (0), m_nMidiOut (0), m_nDropped (0), m_nConnects (0), m_nSilence (0),
	m_nEarlyN (0), m_nClamped (0), m_nLastPoll (0), m_nPollGapMax (0), m_nWantedMax (0)
{
	m_Peer[0] = 192; m_Peer[1] = 168; m_Peer[2] = 77; m_Peer[3] = 1;
	for (int i = 0; i < Slots; i++)
	{
		m_Slot[i].bBusy = m_Slot[i].bDone = FALSE;
		m_Slot[i].pSocket = 0;
	}
}

// The connect task: blocks in Connect (the scheduler runs the network and core 0's loop meanwhile), then hands the
// result to its slot. Its last access to the slot is bDone; the scheduler deletes the task after Run.
void CConnectTask::Run (void)
{
	CSocket *s = new CSocket (m_pNet, IPPROTO_TCP);
	CIPAddress Peer (m_pSlot->Peer);
	int nResult = s->Connect (Peer, m_pSlot->nPort);
	m_pSlot->pSocket = s;
	m_pSlot->nResult = nResult;
	m_pSlot->bDone = TRUE;
}

void CTcpTransport::SetPeer (const u8 IP[4], u16 nPort)
{
	if ((m_pSocket != 0 || m_nConnecting >= 0) && (memcmp (m_Peer, IP, 4) != 0 || m_nPort != nPort))
	{
		Stop ();				// a new peer: reconnect (an attempt in flight is abandoned)
	}
	memcpy (m_Peer, IP, 4);
	m_nPort = nPort;
	m_nNextTry = CTimer::Get ()->GetTicks ();		// try now
}

// Abandoned attempts that have finished: their sockets closed (a late connect must not leave the emulator a silent
// host), their slots free. Any link (kernel.cpp's loop), so a :link=usb during a connect still closes it.
void CTcpTransport::Reap (void)
{
	for (int i = 0; i < Slots; i++)
	{
		TConnectSlot &r = m_Slot[i];
		if (!r.bBusy || !r.bDone || i == m_nConnecting) continue;
		delete r.pSocket;
		r.pSocket = 0;
		r.bDone = FALSE;
		r.bBusy = FALSE;
	}
}

void CTcpTransport::StartConnect (void)
{
	int i = 0;
	while (i < Slots && m_Slot[i].bBusy) i++;
	m_nNextTry = CTimer::Get ()->GetTicks () + 2 * HZ;
	if (i == Slots) return;			// every slot holds a blocked, abandoned connect: wait for one
	TConnectSlot &r = m_Slot[i];
	memcpy (r.Peer, m_Peer, 4);
	r.nPort = m_nPort;
	r.pSocket = 0;
	r.nResult = 0;
	r.bDone = FALSE;
	r.bBusy = TRUE;
	m_nConnecting = i;
	new CConnectTask (m_pNet, &r);		// runs at the next yield (the loop's MsSleep)
}

void CTcpTransport::Adopt (CSocket *pSocket)
{
	m_pSocket = pSocket;
	m_pSocket->SetOptionSendTimeout (SendTimeoutUs);	// Send waits at most this long on a full TX queue
	m_Parser.Reset ();
	m_bHello = FALSE;
	m_nGranted = 0;
	m_nWanted = 0;
	m_nWaitSince = 0;
	m_nConnects++;
	g_Output.Reset ();				// a new stream: fade in again
	u8 h[3];
	int nHello = emu_hello (h, HelloFlags);
	if (m_pSocket->Send (h, nHello, 0) != nHello)
	{
		Lost ("hello not sent");
		return;
	}
	CLogger::Get ()->Write (From, LogNotice, "connected to %u.%u.%u.%u:%u, hello sent", m_Peer[0], m_Peer[1], m_Peer[2],
				m_Peer[3], m_nPort);
}

void CTcpTransport::Lost (const char *pWhy)
{
	CLogger::Get ()->Write (From, LogWarning, "link lost: %s; reconnecting", pWhy);
	delete m_pSocket;
	m_pSocket = 0;
	m_bHello = FALSE;
	m_nWanted = 0;
}

void CTcpTransport::Want (u32 nFrames)
{
	// Before the answer: kept (and counted), answered once the hello's answer came (Poll sends nothing before
	// it). rytm-emu opens every connection with N 512 before its hello answer and asks for more only as those
	// frames arrive (measured on the Pi: dropping that first N left the link at 0 frames/s).
	if (!m_bHello) m_nEarlyN++;
	if (nFrames > MaxWanted - m_nWanted)
	{
		m_nClamped += nFrames - (MaxWanted - m_nWanted);
		m_nWanted = MaxWanted;
		return;
	}
	m_nWanted += nFrames;
}

// This Poll's bounds: at most PollMessages messages, and none started once PollBudgetUs has been spent sending.
boolean CTcpTransport::PollRoom (void) const
{
	if (m_nPollSent == 0) return TRUE;
	return m_nPollSent < PollMessages && CTimer::GetClockTicks () - m_nPollStart < PollBudgetUs;
}

// One whole message. Circle's Send waits up to SendTimeoutUs per segment, so it is also timed as a whole: a Send
// that failed, or took longer than SendSlowUs, loses the link (a link this slow cannot carry the stream anyway).
boolean CTcpTransport::SendAll (const void *pData, unsigned nLen, const char *pWhat)
{
	unsigned t0 = CTimer::GetClockTicks ();
	if (m_nPollSent++ == 0) m_nPollStart = t0;
	int n = m_pSocket->Send (pData, nLen, 0);
	if (n != (int) nLen)
	{
		CString Why;
		Why.Format ("send failed or timed out (%s, %d)", pWhat, n);
		Lost (Why);
		return FALSE;
	}
	if (CTimer::GetClockTicks () - t0 > SendSlowUs)
	{
		CString Why;
		Why.Format ("one send took %u ms (%s)", (CTimer::GetClockTicks () - t0) / 1000, pWhat);
		Lost (Why);
		return FALSE;
	}
	return TRUE;
}

void CTcpTransport::Midi (const u8 *pData, int nLength)
{
	for (int i = 0; i < nLength; i++)
	{
		if (!g_MidiIn.push (pData[i])) m_nMidiInLost++;
	}
	m_nMidiIn += nLength;
	asm volatile ("dsb sy; sev");
	// osupdate=1: the emulated Rytm's USB MIDI to the computer too, as a real Rytm's is (kernel.cpp
	// MIDIPacketHandler): Transfer and elkflash wait for its answers
	CSidecarGadget *pSidecar = CSidecarGadget::Get ();
	if (pSidecar == 0 || !pSidecar->OsUpdate ()) return;
	EnterCritical ();
	for (int i = 0; i < nLength; i++) m_ToComputer.Feed (pData[i], [pSidecar] (const u8 *p) { pSidecar->FromRytmMIDI (p); });
	LeaveCritical ();
	m_nComputerOut += nLength;
}

void CTcpTransport::Poll (void)
{
	unsigned now = CTimer::GetClockTicks ();
	if (m_nLastPoll != 0 && now - m_nLastPoll > m_nPollGapMax) m_nPollGapMax = now - m_nLastPoll;
	m_nLastPoll = now;
	Reap ();
	CSidecarGadget *pSidecar = CSidecarGadget::Get ();
	if (pSidecar != 0 && pSidecar->OsUpdate ())	// osupdate=1: the emulator takes the computer's MIDI (SendComputerMIDI)
		pSidecar->RytmMIDIConnected (m_pSocket != 0 && m_bHello && (m_nGranted & HelloMidi) != 0);
	if (m_pSocket == 0)
	{
		m_nDropped += g_ToHost.Drain ();	// no host: the engine's records go nowhere (whole, counted)
		if (m_nConnecting >= 0)
		{
			TConnectSlot &r = m_Slot[m_nConnecting];
			if (!r.bDone) return;		// still connecting: the loop and the watchdog run on
			CSocket *s = r.pSocket;
			int nResult = r.nResult;
			r.pSocket = 0;
			r.bDone = FALSE;
			r.bBusy = FALSE;
			m_nConnecting = -1;
			if (nResult < 0) delete s;	// refused or timed out: the next try at m_nNextTry
			else Adopt (s);
			return;
		}
		if ((int) (CTimer::Get ()->GetTicks () - m_nNextTry) >= 0) StartConnect ();
		return;
	}
	for (int nSegs = 0; nSegs < 16; nSegs++)	// one TCP segment per Receive: what has arrived, bounded
	{
		int n = m_pSocket->Receive (m_Rx, sizeof m_Rx, MSG_DONTWAIT);
		if (n == 0) break;
		if (n < 0)
		{
			Lost ("receive failed");
			return;
		}
		if (!m_Parser.Feed (m_Rx, n))
		{
			Lost ("a message this host does not know (an older rytm-emu without the hello?)");
			return;
		}
	}
	if (m_nWanted > m_nWantedMax) m_nWantedMax = m_nWanted;
	if (m_bHello)					// before the answer the records wait (ToHost drops whole ones when full)
	{
		m_nPollSent = 0;			// this Poll's bounds (PollRoom)
		SendFrames ();
		if (m_pSocket != 0) SendComputerMIDI ();
		// a SysEx of the computer's still open keeps the engine's records back, as on USB (ob_usb.cpp)
		if (m_pSocket != 0 && !(pSidecar != 0 && pSidecar->ForRytmInSysEx ())) SendMIDI ();
	}
}

// osupdate=1: the computer's packets for the Rytm (the Sidecar gadget, filtered there) as one 'M' of their bytes per
// send, up to a full message; the gadget's endpoint waits while its queue is full, so nothing is lost on the way
void CTcpTransport::SendComputerMIDI (void)
{
	CSidecarGadget *pSidecar = CSidecarGadget::Get ();
	if (pSidecar == 0 || !pSidecar->OsUpdate () || (m_nGranted & HelloMidi) == 0) return;
	const unsigned nMax = sizeof m_Msg - 3;
	while (PollRoom ())
	{
		unsigned n = 0;
		u32 w;
		while (n + 3 <= nMax && pSidecar->ForRytm (&w))
		{
			u8 p[4];
			memcpy (p, &w, 4);
			int k = usbmidi_packet_len (p);
			for (int i = 0; i < k; i++) m_Msg[3 + n++] = p[1 + i];
		}
		if (n == 0) return;
		emu_midi_head (m_Msg, (u16) n);
		if (!SendAll (m_Msg, 3 + n, "computer MIDI")) return;
		m_nComputerIn += n;
	}
}

// Answer the emulator's requests from the output stage: what the FIFO holds now, or, once it has been empty for
// 2 ms, silence (counted) for every frame still requested - the emulator plays zeros either way, its N requests
// are answered at their own count (its clock), and the stream keeps moving. Real frames end the silence.
void CTcpTransport::SendFrames (void)
{
	while (m_nWanted > 0 && m_pSocket != 0 && PollRoom ())	// the rest at the next Poll
	{
		unsigned nLevel = g_Output.Level ();
		unsigned k = m_nWanted < MaxFramesPerMessage ? m_nWanted : MaxFramesPerMessage;
		if (nLevel > 0)
		{
			if (nLevel < k) k = nLevel;
			m_nWaitSince = 0;
		}
		else
		{
			unsigned now = CTimer::GetClockTicks ();
			if (m_nWaitSince == 0) m_nWaitSince = now | 1;
			if (now - m_nWaitSince < 2000) return;
			m_nSilence += k;			// still empty: m_nWaitSince stays, the rest follows as silence
		}
		static s32 F[MaxFramesPerMessage][Output::kChannels];
		g_Output.Pull (F, k);				// silence when it runs out
		asm volatile ("dsb sy; sev");			// the control core refills
		boolean bVoices = (m_nGranted & HelloVoices) && g_Output.GetMode () == Output::kTracks;
		unsigned nLen = emu_frames_head (m_Tx, bVoices ? 'a' : 'A', k);
		for (unsigned f = 0; f < k; f++)
		{
			if (bVoices)
			{
				for (int v = 0; v < Output::kVoices; v++)
				{
					s16 x = (s16) (F[f][Output::kVoiceToChannel[v]] >> 16);
					m_Tx[nLen++] = (u8) x;
					m_Tx[nLen++] = (u8) (x >> 8);
				}
			}
			else
			{
				for (int c = 0; c < Output::kChannels; c++)
				{
					u32 x = (u32) F[f][c];
					for (int b = 0; b < 4; b++) m_Tx[nLen++] = (u8) (x >> (8 * b));
				}
			}
		}
		if (!SendAll (m_Tx, nLen, "frames")) return;
		m_nWanted -= k;
		m_nFrames += k;
		m_nRequests++;
	}
}

// The engine's records (g_ToHost: whole, however core 3's bytes were split), each sent as one 'M' - or dropped
// (counted) when the emulator did not grant the MIDI pair.
void CTcpTransport::SendMIDI (void)
{
	const int nMax = (int) sizeof m_Msg - 3;
	int nLen;
	const u8 *pRec;
	while (PollRoom () && (pRec = g_ToHost.Next (&nLen)) != 0)	// the rest at the next Poll
	{
		if ((m_nGranted & HelloMidi) == 0 || nLen > nMax)
		{
			m_nDropped += nLen;
			continue;
		}
		emu_midi_head (m_Msg, (u16) nLen);
		memcpy (m_Msg + 3, pRec, nLen);
		if (!SendAll (m_Msg, 3 + nLen, "MIDI")) return;
		m_nMidiOut += nLen;
	}
}

void CTcpTransport::Stop (void)
{
	m_nConnecting = -1;				// an attempt in flight is abandoned: Reap closes it when it ends
	if (m_pSocket == 0) return;
	delete m_pSocket;				// closes: the emulator plays zeros from here
	m_pSocket = 0;
	m_bHello = FALSE;
	m_nWanted = 0;
	CLogger::Get ()->Write (From, LogNotice, "link closed");
}

void CTcpTransport::LogStats (CLogger *pLogger)
{
	pLogger->Write (From, LogNotice,
		"%s %u.%u.%u.%u:%u, hello %s (granted %u), frames %u/s in %u requests, wanted %u, silence %u, "
		"midi in %u B (lost %u) out %u B dropped %u, connects %u, skipped %u B, early N %u, clamped %u, "
		"wanted max %u, poll gap max %u us, computer midi %u B to the emulator, %u B back",
		m_pSocket ? "connected to" : m_nConnecting >= 0 ? "connecting to" : "trying", m_Peer[0], m_Peer[1], m_Peer[2], m_Peer[3], m_nPort,
		m_bHello ? "yes" : "no", m_nGranted, m_nFrames, m_nRequests, m_nWanted, m_nSilence, m_nMidiIn,
		m_nMidiInLost, m_nMidiOut, m_nDropped, m_nConnects, (unsigned) m_Parser.Skipped (), m_nEarlyN, m_nClamped,
		m_nWantedMax, m_nPollGapMax, m_nComputerIn, m_nComputerOut);
	m_nFrames = m_nRequests = m_nWantedMax = m_nPollGapMax = 0;
}

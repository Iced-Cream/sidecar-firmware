// pi/engine/link1.cpp - see link1.h.
#include "link1.h"

#include <circle/usb/usbmidi.h>

#include "bridge.h"
#include "device/device.h"
#include "usbmidi_pack.h"

namespace link1 {

Spsc<uint8_t, kInBytes> g_In;
Spsc<uint8_t, kOutBytes> g_Out;

static RecordReader<Spsc<uint8_t, kOutBytes>> s_Reader (&g_Out);
static CUSBMIDIDevice *volatile s_pDevice;	// core 0 writes, the control core reads (Poll: its sender while there)
static TStats s_Stats;
static SysExAsm<4096> s_Asm;			// the control core's

const TStats &Stats (void) { return s_Stats; }
CUSBMIDIDevice *Device (void) { return s_pDevice; }

void Packet (unsigned nCable, uint8_t *pPacket, unsigned nLength)
{
	for (unsigned i = 0; i < nLength; i++)
		if (!g_In.push (pPacket[i])) s_Stats.nInLost++;
	asm volatile ("dsb sy; sev");
}

void SetDevice (CUSBMIDIDevice *pDevice)
{
	s_pDevice = pDevice;
	asm volatile ("dsb sy; sev");
}

// One record at a time, one transfer in flight (circle-midi-async.patch): the box never holds core 0's loop.
void Send (void)
{
	static uint8_t s_Packets[4 * ((4096 + 2) / 3)];
	CUSBMIDIDevice *p = s_pDevice;
	if (p == 0)
	{
		s_Reader.Drain ();			// no box: the engine's records for it go nowhere (whole)
		return;
	}
	unsigned nState = p->GetSendState ();
	if (nState == 1) return;			// the last transfer still in flight
	if (nState == 2) s_Stats.nSendFail++;		// it failed: that message is lost (counted), the next goes
	int nLen;
	const uint8_t *pRec = s_Reader.Next (&nLen);
	if (pRec == 0) return;
	int n = usbmidi_pack_sysex (pRec, nLen, s_Packets);
	if (n == 0) n = usbmidi_pack_piece (pRec, nLen, s_Packets);
	if (n == 0) return;				// (the engine sends whole SysEx, or pieces of one)
	if (p->SendEventPacketsAsync (s_Packets, (unsigned) n)) s_Stats.nSent++;
	else s_Stats.nSendFail++;
}

void ToBox (const uint8_t *pData, int nLength)
{
	if (nLength <= 0) return;
	if (nLength > 4096 || g_Out.size () + 2 + nLength > kOutBytes)
	{
		s_Stats.nOutFull += (unsigned) nLength;
		return;
	}
	g_Out.push ((uint8_t) nLength);
	g_Out.push ((uint8_t) (nLength >> 8));
	for (int i = 0; i < nLength; i++) g_Out.push (pData[i]);
}

void Poll (void)
{
	static boolean s_bUp;
	boolean bUp = s_pDevice != 0;
	if (bUp != s_bUp)
	{
		s_bUp = bUp;
		core::device::set_out (1, bUp ? ToBox : nullptr);
	}
	uint8_t b;
	while (g_In.pop (&b))
	{
		unsigned n = s_Asm.feed (b);
		if (n == 0) continue;
		s_Stats.nSysEx++;
		bridge::sysex_link (1, s_Asm.data (), (int) n);
	}
	s_Stats.nBadSysEx = s_Asm.bad ();
}

}  // namespace link1

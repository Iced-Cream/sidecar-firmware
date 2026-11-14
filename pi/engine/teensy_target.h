// pi/engine/teensy_target.h - usb=mirror through a Teensy 4.1 (docs/teensy-mirror.md): the Pi's end of the link
// (relay_proto.h). The Teensy enumerates on a Pi USB-A port as 1209:0002; this driver claims it and becomes the mirror
// target (mirror_target.h) of the box the USB-C gadget does not mirror (mirror=rytm|a4 picks the gadget's), so both boxes
// are shown to computers at once, each on its own cable.
//
// What runs where (all on core 0):
//   xHCI IRQ    the link's completions: events, the computer's audio OUT blocks (mirror_stream.h), packets the
//               computer sent to the box's pipes; the box's IN blocks and pipe packets going out to the Teensy
//   core 0 loop MirrorPoll: the forwarded control requests (CtlData / the box / CtlAnswer), credits, the box's late
//               attach, its pipes' halt recovery, the hold after the box went, a stats line every 10 s
//   task level  Configure, and the descriptor upload when the box is read (MirrorSetDescriptors .. MirrorBoxUp)
#ifndef _teensy_target_h
#define _teensy_target_h

#include <circle/usb/usbfunction.h>
#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbrequest.h>
#include <circle/logger.h>
#include <circle/types.h>
#include "mirror_target.h"
#include "mirror_stream.h"
#include "relay_proto.h"
#include "midi_filter.h"

class CObUsb;

class CTeensyTarget : public CUSBFunction, public IMirrorTarget
{
public:
	CTeensyTarget (CUSBFunction *pFunction);
	~CTeensyTarget (void);
	boolean Configure (void) override;

	static CTeensyTarget *Get (void)	{ return s_pThis; }
	static void LogStats (CLogger *pLogger);

	// IMirrorTarget
	void SetBox (CObUsb *pBox) override;
	boolean Mirror (void) const override;
	boolean MirrorShown (void) const override;
	boolean MirrorSetDescriptors (const u8 *pDev, const u8 *pCfg, unsigned nCfg) override;
	void MirrorSetString (u8 uchIndex, const u8 *pDesc, unsigned nLength) override;
	void MirrorBoxUp (boolean bUp) override;
	void MirrorControlDone (unsigned nSeq, const u8 *pData, int nLength) override;
	boolean MirrorPipeIn (u8 uchAddr, const u8 *pData, unsigned nLength) override;
	void MirrorPipeOutDone (u8 uchAddr) override;
	boolean MirrorOutExtra (u8 *pBlock, u16 nCounter) override;
	void FromBoxBlock (const u8 *pBlock) override;
	void ToBox (s32 (*pFrames)[12], const s16 (*pVoices)[8], unsigned nFrames) override;
	void MirrorPoll (void) override;

private:
	static void EvStub (CUSBRequest *pURB, void *pParam, void *pContext);
	static void AudioInStub (CUSBRequest *pURB, void *pParam, void *pContext);	// the computer's OUT blocks (link 0x82)
	static void AudioOutStub (CUSBRequest *pURB, void *pParam, void *pContext);	// an IN block sent to the Teensy (0x02)
	static void ChanInStub (CUSBRequest *pURB, void *pParam, void *pContext);	// a packet the computer sent to the box
	static void ChanOutStub (CUSBRequest *pURB, void *pParam, void *pContext);	// a box packet sent to the Teensy

	void OnEvent (const relay::Event &e);
	int Vendor (boolean bIn, u8 uchRequest, u16 wValue, u16 wIndex, const void *pData, unsigned nLength);	// task / loop
	boolean Upload (void);			// the box's descriptors and strings to the Teensy, then kReqConnect
	void Disconnect (void);
	void ControlPoll (void);
	boolean IsNameRequest (const u8 *pSetup, u8 uchType) const;
	unsigned ChanOfOut (u8 uchAddr) const;	// the link channel a mirrored OUT endpoint rides (kChannels: none)
	unsigned ChanOfIn (u8 uchAddr) const;
	void Resubmit (CUSBRequest *pURB);
	static void MidiTap (const u8 *pEventPacket);	// xHCI IRQ (link1::Tap): the box's USB MIDI, for the computer
	void FlushMidiIn (void);			// xHCI IRQ: queued box MIDI out to the Teensy when its channel is free
	unsigned MidiInChan (void) const;		// the link channel of the box's MIDI IN endpoint (kChannels: none)

	static CTeensyTarget *s_pThis;

	CUSBEndpoint *m_pEv, *m_pAudioIn, *m_pAudioOut;
	CUSBEndpoint *m_pChanIn[relay::kChannels], *m_pChanOut[relay::kChannels];
	static const unsigned EvURBs = 2, AudioInURBs = 4, AudioOutBufs = 16, ChanInURBs = 2;
	u8 *m_pEvBuf[EvURBs];
	CUSBRequest *m_pEvURB[EvURBs], *m_pAudioInURB[AudioInURBs], *m_pChanInURB[relay::kChannels][ChanInURBs];
	CUSBRequest *m_pAudioOutURB[AudioOutBufs];	// kept while its length (the computer's channels) stays
	unsigned m_nAudioOutLen[AudioOutBufs];
	volatile unsigned m_nAtBox[relay::kChannels];	// computer packets at the box, not yet taken (a box that goes: credited back)
	u8 *m_pAudioInBuf[AudioInURBs];
	u8 *m_pAudioOutBuf[AudioOutBufs];
	volatile boolean m_bAudioOutBusy[AudioOutBufs];
	u8 *m_pChanInBuf[relay::kChannels][ChanInURBs];
	u8 *m_pChanOutBuf[relay::kChannels];
	volatile boolean m_bChanOutBusy[relay::kChannels];
	u8 *m_pCtlBuf;				// the link's EP0 (DMA)
	u16 m_nVersion;

	CObUsb *volatile m_pBox;
	// the box's descriptors as read (MirrorSetDescriptors / MirrorSetString)
	u8 m_Dev[18];
	u8 m_Cfg[mirror::kMaxConfig];
	unsigned m_nCfg;
	struct TStr { u8 uchIndex, nLength; u8 Data[254]; };
	TStr m_Str[mirror::kMaxStrings];
	unsigned m_nStr;
	relay::Map m_Map;
	boolean m_bHave, m_bKeep;		// read; the same box again (its strings kept)
	volatile boolean m_bBoxUp, m_bConnected, m_bPcConfigured, m_bUploadDue;
	u64 m_nBoxGoneAt;
	unsigned m_nHoldSecs;
	char m_Name[24];			// mirrorname= suffix, as the gadget's

	mirror::Stream m_Stream;

	// control: one request at a time, as on the Teensy (relay::CtlSlot there)
	enum TCtlState { CtlIdle, CtlNew, CtlAtBox, CtlAnswered };
	volatile TCtlState m_CtlState;
	u8 m_CtlSeq;
	u8 m_CtlSetup[8];
	u8 m_CtlData[512];
	int m_nCtlAnswer;			// the box's answer length (< 0: STALL)

	volatile unsigned m_nCreditDue[relay::kChannels];
	volatile unsigned m_nMidiHeld[relay::kChannels];	// MIDI credits held back while link1's queue to the box is full
	MidiFilter m_ToBoxFilter;			// the computer's MIDI to the box: usbmidi=os: as the gadget's (no OS update by default)
	static const unsigned MidiInRing = 512;
	u32 m_MidiIn[MidiInRing];			// the box's event packets for the computer
	volatile unsigned m_nMidiInHead, m_nMidiInTail;
	volatile unsigned m_nTakenDue;

	struct TStats {
		unsigned nEvents, nSetups, nAborts, nAnswers, nStalls, nLate, nAudioOutDrop, nChanInDrop, nChanInMidi;
		unsigned nPipeOut, nPipeIn, nPipeInDrop, nCredits, nVendorFail, nConnects;
		unsigned nMidiToBox, nMidiToBoxFull, nMidiToPc, nMidiToPcDrop;
	} m_Stats;
	u64 m_nLastLog;
};

#endif

//
// rytmobdevice.h - Circle USB function driver for the Analog Rytm MKII in OVERBRIDGE mode
// (VID 0x1935, PID 0x0b48): alt 3, interrupt IN 0x83 / OUT 0x03, 368-byte blocks.
//
// Run-time options (cmdline.txt on the SD card, one line, space separated; all optional):
//   oburbs=N       URBs in flight per endpoint, 1..16 (default 8; stock Circle allowed 2)
//   obblocks=N     368-byte blocks per URB (one TRB, IOC at its end), 1..8 (default 1)
//   obout=MODE     paced (default): one OUT block per IN block received, as Overwitch,
//                  rytm-emu's host and the Teensy's RytmHost do; free: every OUT URB is
//                  resubmitted on completion (the first hardware run's behaviour); off: no OUT
//   oboutdelay=N   paced: IN blocks to receive before the first OUT block (default 0)
//   obctr=MODE     zero (default): the OUT frame counter starts at 0; in: at the first IN
//                  block's counter
//
#ifndef _rytmobdevice_h
#define _rytmobdevice_h

#include <circle/usb/usbfunction.h>
#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbrequest.h>
#include <circle/logger.h>
#include <circle/types.h>

class CRytmObDevice : public CUSBFunction
{
public:
	static const unsigned MaxURBs = 16;		// <= XHCI_EP_MAX_URBS (circle-xhci-urbqueue.patch)
	static const unsigned MaxBlocks = 8;

	CRytmObDevice (CUSBFunction *pFunction);
	~CRytmObDevice (void);

	boolean Configure (void);

	// called once a second from the main loop (counters are updated in IRQ context)
	static void LogAllStats (CLogger *pLogger);

	// 10 s capture of IN MAIN L/R (channels 0, 1) as the raw int32 samples (the on-wire scale is not yet measured on the Rytm side: MAIN peaks of ~3.9e6 with a pattern playing would be -55 dBFS as int32 full scale, so nothing is cut off),
	// interleaved little-endian, i.e. WAV PCM data. Task level: CaptureAlloc once (1.92 MB on the
	// heap), CaptureStart arms it; the IN completion fills it (no allocation, no logging) and
	// sets CaptureFull. Gaps/lost: IN frame-counter gaps (and blocks lost) while capturing.
	static const unsigned CaptureRate = 48000, CaptureSecs = 10, CaptureChannels = 2;
	static const unsigned CaptureFrames = CaptureRate * CaptureSecs;
	static const unsigned CaptureBytes = CaptureFrames * CaptureChannels * 4;
	static boolean CaptureAlloc (void);
	static boolean CaptureStart (void);		// FALSE: no buffer, no stream, or busy
	static void CaptureStop (void);			// abandon a capture (stream gone)
	static boolean CaptureActive (void)	{ return s_bCapOn; }
	static boolean CaptureFull (void)	{ return s_bCapFull; }
	static void CaptureClearFull (void)	{ s_bCapFull = FALSE; }
	static unsigned CaptureFramesDone (void) { return s_nCapPos; }
	static unsigned CaptureGaps (void)	{ return s_nCapGaps; }
	static unsigned CaptureLost (void)	{ return s_nCapLost; }
	static const u8 *CaptureData (void)	{ return (const u8 *) s_pCapBuf; }

	// every xHCI transfer event, from the patched CXHCIEndpoint::TransferEvent (IRQ)
	static void XHCIEvent (unsigned nEndpointID, unsigned nCode, unsigned nResidue, unsigned nPending);

	enum TOutMode { OutPaced, OutFree, OutOff };

	struct TEvStats				// xHCI transfer events of one endpoint
	{
		unsigned nEvents;
		unsigned nSuccess;		// completion code 1
		unsigned nShort;		// 13
		unsigned nRing;			// 14 ring underrun, 15 overrun
		unsigned nMissed;		// 23 missed service
		unsigned nOther;		// anything else (the last one in nLastOther)
		unsigned nLastOther;
		unsigned nOrphan;		// no URB pending when the event came
	};

private:
	boolean ScanDescriptors (void);			// log everything, locate alt 3 endpoints
	boolean SubmitIn (unsigned i);
	boolean SubmitOut (unsigned i);
	void InDone (unsigned i, CUSBRequest *pURB);
	void OutDone (unsigned i, CUSBRequest *pURB);
	void PumpOut (void);
	void FillOut (u8 *pBuf, unsigned nBlocks);
	void Log (CLogger *pLogger);
	static void InStub (CUSBRequest *pURB, void *pParam, void *pContext);
	static void OutStub (CUSBRequest *pURB, void *pParam, void *pContext);

private:
	// options
	unsigned m_nURBs, m_nBlocks, m_nOutDelay;
	TOutMode m_OutMode;
	boolean m_bCounterFromIn;

	// endpoint descriptors found at interface 1 / 2 alt 3 (point into the device's config descriptor)
	const TUSBEndpointDescriptor *m_pInDesc;
	const TUSBEndpointDescriptor *m_pOutDesc;
	CUSBEndpoint *m_pIn;
	CUSBEndpoint *m_pOut;

	// fixed URB pool (no allocation while streaming), one buffer each
	CUSBRequest *m_pInURB[MaxURBs], *m_pOutURB[MaxURBs];
	volatile boolean m_bInBusy[MaxURBs], m_bOutBusy[MaxURBs];
	volatile unsigned m_nInPending, m_nOutPending;
	volatile boolean m_bStreaming;

	u16 m_nOutCounter;
	boolean m_bOutCounterSet;
	u16 m_nExpectIn;
	boolean m_bHaveIn;
	u32 m_nPhase;
	volatile unsigned m_nCredit;			// OUT blocks owed (paced)

	// statistics, written from the xHCI interrupt, read by LogAllStats
	volatile unsigned m_nInBlocks, m_nInShort, m_nInBadMagic, m_nInGaps, m_nInLost, m_nInMaxGap,
			  m_nInOddGap, m_nInErrors, m_nInSubmitFail;
	volatile unsigned m_nOutBlocks, m_nOutErrors, m_nOutSubmitFail, m_nCreditDrops;
	volatile int m_nInPeak;				// largest |sample| seen on MAIN L (channel 0), per second
	volatile unsigned m_nInMaxDt;			// longest time between IN completions (us), per second

	// times (CTimer::GetClockTicks, us)
	unsigned m_nStartUs, m_nLastInUs, m_nLastOutUs, m_nFirstInUs, m_nFirstOutUs, m_nOut16Us, m_nLastLogUs;
	boolean m_bSaidStart;
	unsigned m_nPrevIn, m_nPrevOut, m_nSeconds;

	static CRytmObDevice *s_pThis;

	static s32 *s_pCapBuf;
	static volatile unsigned s_nCapPos;		// frames written
	static volatile boolean s_bCapOn, s_bCapFull;
	static volatile unsigned s_nCapGaps, s_nCapLost;

public:
	static TEvStats s_EvIn, s_EvOut;
};

#endif

// pi/engine/rec_pi.h - the recorder (recorder.h) and the takes' playback (rec_play.h) in the Pi kernel: its memory reserved once at boot (cmdline recbuf=MB,
// default 64; 0: recording off), the card through FatFs, the boot scan of /Recordings, the console's rec commands,
// /status's "rec" member. Everything here is core 0's: the IN completion (xHCI interrupt) and the OUT fill feed
// g_Rec, the loop drains it (Poll), the web task and the console control it.
#ifndef _rec_pi_h
#define _rec_pi_h

#include <circle/string.h>
#include "recorder.h"
#include "rec_play.h"

extern rec::Recorder g_Rec;
extern recplay::Player g_Play;		// the takes' playback (rec_play.h): mixed onto MAIN in the OUT fill (ob_usb.cpp)

namespace rec_pi {
void Init (void);			// CKernel::Initialize, after the card is mounted (bridge::begin)
void Poll (void);			// core 0's loop
bool Command (const char *pCmd);	// rec, rec=start|stop|arm|ch:LIST|bits:16|24|name:PREFIX; play, play=start|stop|
					// scan|take:N|ch:N|level:N|start:TAKE/NN (true: handled)
void Status (CString *pOut);		// ,"rec":{...},"play":{...}
void Transport (u8 nStatus, u16 nSPP);	// MIDI realtime (core 3): PLAY/STOP for an armed recorder; then the send FX's clock
int UiState (unsigned *pSecs);		// the display: 0 off, 1 armed, 2 recording (and its seconds)
}

#endif

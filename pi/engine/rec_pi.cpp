// pi/engine/rec_pi.cpp - see rec_pi.h.
#include "rec_pi.h"

#include <circle/alloc.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <fatfs/ff.h>
#include "sendfx_pi.h"
#include "bridge.h"

static const char From[] = "rec";
rec::Recorder g_Rec;
recplay::Player g_Play;

namespace {

FIL s_F[rec::kSources];
bool s_bUsed[rec::kSources];

void SdPath (char *pOut, unsigned n, const char *p) { strcpy (pOut, "SD:"); strncpy (pOut + 3, p, n - 4); pOut[n - 1] = 0; }

bool BMkdir (const char *p)
{
	char sd[200];
	SdPath (sd, sizeof sd, p);
	FRESULT r = f_mkdir (sd);
	return r == FR_OK || r == FR_EXIST;
}
int BOpen (const char *p)
{
	char sd[200];
	SdPath (sd, sizeof sd, p);
	for (int i = 0; i < rec::kSources; i++)
	{
		if (s_bUsed[i]) continue;
		if (f_open (&s_F[i], sd, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -1;
		s_bUsed[i] = true;
		return i;
	}
	return -1;
}
bool BWrite (int h, const void *p, uint32_t n)
{
	UINT w = 0;
	return f_write (&s_F[h], p, n, &w) == FR_OK && w == n;
}
bool BPwrite (int h, uint64_t nOff, const void *p, uint32_t n)
{
	FIL *f = &s_F[h];
	FSIZE_t nEnd = f_size (f);
	UINT w = 0;
	return f_lseek (f, nOff) == FR_OK && f_write (f, p, n, &w) == FR_OK && w == n && f_lseek (f, nEnd) == FR_OK;
}
bool BSync (int h) { return f_sync (&s_F[h]) == FR_OK; }
bool BClose (int h)
{
	bool ok = f_close (&s_F[h]) == FR_OK;
	s_bUsed[h] = false;
	return ok;
}
bool BSpace (uint64_t *pFree, uint64_t *pTotal)
{
	DWORD nFree;
	FATFS *pFs;
	if (f_getfree ("SD:", &nFree, &pFs) != FR_OK) return false;
	*pFree = (uint64_t) nFree * pFs->csize * 512;
	*pTotal = (uint64_t) (pFs->n_fatent - 2) * pFs->csize * 512;
	return true;
}
uint32_t BNow (void) { return CTimer::GetClockTicks (); }
int BList (const char *p, char (*pNames)[64], uint64_t *pSizes, bool *pDirs, int nMax)
{
	char sd[200];
	SdPath (sd, sizeof sd, p);
	DIR d;
	FILINFO fi;
	if (f_opendir (&d, sd) != FR_OK) return -1;
	int n = 0;
	while (n < nMax && f_readdir (&d, &fi) == FR_OK && fi.fname[0])
	{
		strncpy (pNames[n], fi.fname, 63);
		pNames[n][63] = 0;
		pSizes[n] = fi.fsize;
		pDirs[n] = (fi.fattrib & AM_DIR) != 0;
		n++;
	}
	f_closedir (&d);
	return n;
}
bool BReadAt (const char *p, uint64_t nOff, void *pBuf, uint32_t n)
{
	char sd[300];
	SdPath (sd, sizeof sd, p);
	FIL f;
	if (f_open (&f, sd, FA_READ) != FR_OK) return false;
	UINT got = 0;
	bool ok = f_lseek (&f, nOff) == FR_OK && f_read (&f, pBuf, n, &got) == FR_OK && got == n;
	f_close (&f);
	return ok;
}
bool BPatch (const char *p, uint64_t nOff, const void *pBuf, uint32_t n)
{
	char sd[300];
	SdPath (sd, sizeof sd, p);
	FIL f;
	if (f_open (&f, sd, FA_WRITE | FA_OPEN_EXISTING) != FR_OK) return false;
	UINT w = 0;
	bool ok = f_lseek (&f, nOff) == FR_OK && f_write (&f, pBuf, n, &w) == FR_OK && w == n;
	return f_close (&f) == FR_OK && ok;
}
void BLog (const char *pLine) { CLogger::Get ()->Write (From, LogNotice, "%s", pLine + (strncmp (pLine, "rec: ", 5) ? 0 : 5)); }

const rec::Backend s_Card = {BMkdir, BOpen, BWrite, BPwrite, BSync, BClose, BSpace, BNow, BList, BReadAt, BPatch, BLog};

// playback's reads (rec_play.h): its own files, a stamp per entry (FatFs' date and time: larger is newer)
FIL s_PF[2];
bool s_bPUsed[2];
int PList (const char *p, char (*pNames)[64], uint64_t *pSizes, bool *pDirs, uint32_t *pStamps, int nMax)
{
	char sd[200];
	SdPath (sd, sizeof sd, p);
	DIR d;
	FILINFO fi;
	if (f_opendir (&d, sd) != FR_OK) return -1;
	int n = 0;
	while (n < nMax && f_readdir (&d, &fi) == FR_OK && fi.fname[0])
	{
		strncpy (pNames[n], fi.fname, 63);
		pNames[n][63] = 0;
		pSizes[n] = fi.fsize;
		pDirs[n] = (fi.fattrib & AM_DIR) != 0;
		pStamps[n] = (uint32_t) fi.fdate << 16 | fi.ftime;
		n++;
	}
	f_closedir (&d);
	return n;
}
int POpen (const char *p)
{
	char sd[300];
	SdPath (sd, sizeof sd, p);
	for (int i = 0; i < 2; i++)
	{
		if (s_bPUsed[i]) continue;
		if (f_open (&s_PF[i], sd, FA_READ) != FR_OK) return -1;
		s_bPUsed[i] = true;
		return i;
	}
	return -1;
}
int PRead (int h, void *p, uint32_t n)
{
	UINT got = 0;
	return f_read (&s_PF[h], p, n, &got) == FR_OK ? (int) got : -1;
}
bool PSeek (int h, uint64_t nOff) { return f_lseek (&s_PF[h], nOff) == FR_OK; }
void PClose (int h)
{
	f_close (&s_PF[h]);
	s_bPUsed[h] = false;
}
void PLog (const char *pLine) { CLogger::Get ()->Write ("play", LogNotice, "%s", pLine + (strncmp (pLine, "play: ", 6) ? 0 : 6)); }

const recplay::Backend s_PlayCard = {PList, POpen, PRead, PSeek, PClose, PLog};
int s_nRecWas;		// the recorder's state at the last poll: a take that ends selects it for playback

unsigned s_nBufMB;
unsigned s_nLastLogUs;

}  // namespace

namespace rec_pi {

void Init (void)
{
	CLogger *L = CLogger::Get ();
	s_nBufMB = CKernelOptions::Get ()->GetAppOptionDecimal ("recbuf", 64);
	if (s_nBufMB > 512) s_nBufMB = 512;
	size_t nBytes = s_nBufMB ? (size_t) s_nBufMB << 20 : 0;
	void *p = 0;
	if (nBytes)
	{
		size_t nStage = (size_t) rec::kSources * rec::kChunk;
		if (nBytes < nStage + (2u << 20)) nBytes = nStage + (2u << 20);
		p = memalign (64, nBytes);
		if (p == 0) L->Write (From, LogError, "no memory for the recorder (%u MB): recording off", s_nBufMB);
	}
	g_Rec.init (p, p ? nBytes : 0, &s_Card);
	g_Play.init (&s_PlayCard, BNow);
	if (p == 0)
	{
		L->Write (From, LogNotice, "recording off (recbuf=0)");
		return;
	}
	unsigned t0 = CTimer::GetClockTicks ();
	int nFixed = g_Rec.fix_all ("/Recordings");
	rec::Status s;
	g_Rec.status (&s, true);
	int nTakes = g_Play.scan ();
	L->Write (From, LogNotice, "%u MB reserved at %p (ring %u ms at the default selection), boot scan of /Recordings: %d fixed in %u ms, %d takes to play",
		  (unsigned) (nBytes >> 20), p, s.ring_ms, nFixed, (CTimer::GetClockTicks () - t0) / 1000, nTakes);
}

// Playback's requests (the REC page, the console, the web page) on core 0: never while a take records (or closes).
static bool RecBusy (void)
{
	rec::Status s;
	g_Rec.status (&s, false);
	return s.state == rec::Recorder::kRecording || s.state == rec::Recorder::kEnded;
}
static bool PlayStart (char *pErr, unsigned nErr)
{
	if (RecBusy ())
	{
		strncpy (pErr, "a take is recording", nErr - 1);
		return false;
	}
	recplay::Status ps;
	g_Play.status (&ps);
	if (ps.takes == 0) g_Play.scan ();
	return g_Play.play (pErr, nErr);
}

static void Say (void);

// The remote UI's REC page (core/ui/page_rec.cpp, drawn on core 3): its request (YES, encoder A) run here on core 0,
// the recorder's state handed over every 200 ms and after each request.
static void RuiPoll (void)
{
	static unsigned s_nLast, s_nErrAt, s_nPlayErrAt;
	static char s_Err[32], s_PlayErr[32];
	unsigned now = CTimer::GetClockTicks ();
	int nReq = bridge::rui_rec_take ();
	unsigned nKeys = bridge::rui_rec_toggles ();
	if (nKeys)		// 1026: T1..T8 the tracks' IN channels (2..9), T9 MASTER, T10 / T11 IN L / R (the FX return)
	{
		uint32_t m = g_Rec.mask ();
		for (int k = 0; k < 11; k++)
			if (nKeys >> k & 1) m ^= 1u << (k < 8 ? rec::kIn + 2 + k : k == 8 ? rec::kMaster : rec::kIn + k + 1);
		char err[64] = "";
		if (!g_Rec.configure (m, g_Rec.bits (), err, sizeof err))
		{
			CLogger::Get ()->Write (From, LogWarning, "remote page: channels: %s", err);
			strncpy (s_Err, err, sizeof s_Err - 1);
			s_nErrAt = now;
		}
		Say ();
	}
	int nToggle, nCh, nTake, nLevel;
	bool bPlayIn = bridge::rui_play_take (&nToggle, &nCh, &nTake, &nLevel);
	if (bPlayIn)		// B pushed / turned, C, D (core/ui/page_rec.cpp)
	{
		char err[64] = "";
		bool ok = true;
		if (nLevel) g_Play.set_level (g_Play.level () + nLevel);
		if (nTake) g_Play.step_take (nTake);		// C right: older takes (the list is newest first)
		if (nCh) g_Play.step_channel (nCh);
		if (nToggle)
		{
			if (g_Play.active ()) g_Play.stop ();
			else ok = PlayStart (err, sizeof err);
		}
		if (!ok) CLogger::Get ()->Write ("play", LogWarning, "remote page: %s", err);
		strncpy (s_PlayErr, ok ? "" : err, sizeof s_PlayErr - 1);
		s_nPlayErrAt = now;
	}
	if (nReq)
	{
		char err[64] = "";
		bool ok = true;
		if ((nReq == 1 || nReq == 3) && g_Play.active ()) g_Play.stop ();	// never play while recording
		if (nReq == 1) ok = g_Rec.start (err, sizeof err);
		else if (nReq == 3) ok = g_Rec.arm (err, sizeof err);
		else if (nReq == 2 || nReq == 4) g_Rec.stop ();
		static const char *Name[] = {"", "start", "stop", "arm", "disarm"};
		if (!ok) CLogger::Get ()->Write (From, LogWarning, "remote page: %s: %s", Name[nReq >= 0 && nReq <= 4 ? nReq : 0], err);
		strncpy (s_Err, ok ? "" : err, sizeof s_Err - 1);
		s_nErrAt = now;
		Say ();
	}
	else if (!bPlayIn && !nKeys && now - s_nLast < 200000) return;
	s_nLast = now;
	if (s_Err[0] && now - s_nErrAt > 3000000) s_Err[0] = 0;	// an error shows 3 s
	if (s_PlayErr[0] && now - s_nPlayErrAt > 3000000) s_PlayErr[0] = 0;
	recplay::Status ps;
	g_Play.status (&ps);
	bridge::RuiPlay pl = {};
	pl.state = ps.state, pl.level = ps.level, pl.ch_i = ps.ch_i, pl.channels = ps.channels, pl.take_i = ps.take_i;
	pl.takes = ps.takes, pl.pos_ms = (unsigned) (ps.pos / 48), pl.len_ms = (unsigned) (ps.len / 48);
	strncpy (pl.take, ps.take, sizeof pl.take - 1);
	strncpy (pl.channel, ps.channel, sizeof pl.channel - 1);
	strncpy (pl.error, s_PlayErr[0] ? s_PlayErr : ps.error, sizeof pl.error - 1);
	bridge::rui_play (pl);
	rec::Status s;
	g_Rec.status (&s, false);
	bridge::RuiRec r = {};
	r.state = !s.ready ? 0 : s.state == rec::Recorder::kArmed ? 2 : s.state == rec::Recorder::kRecording ? 3
		: s.state == rec::Recorder::kEnded ? 4 : 1;
	r.secs = (unsigned) (s.frames / 48000);
	r.free_mb = (unsigned) (s.free_bytes >> 20), r.total_mb = (unsigned) (s.total_bytes >> 20), r.need_kbps = s.need_kbps;
	r.in_mask = (s.mask >> rec::kIn & 0xFFF) | (s.mask & (1u << rec::kMaster) ? 3 : 0);	// MASTER: IN 0 / 1
	r.ch_select = 1;
	strncpy (r.take, s.take, sizeof r.take - 1);
	strncpy (r.error, s_Err[0] ? s_Err : s.error, sizeof r.error - 1);
	bridge::rui_rec (r);
}

void Poll (void)
{
	g_Rec.poll ();
	g_Play.poll ();
	rec::Status s;
	g_Rec.status (&s, false);
	if ((s.state == rec::Recorder::kRecording || s.state == rec::Recorder::kEnded) && g_Play.active ())
		g_Play.stop ();				// a take started (the Rytm's PLAY on an armed recorder): playback fades out
	if (s.state == rec::Recorder::kIdle && s_nRecWas != rec::Recorder::kIdle && s_nRecWas != rec::Recorder::kArmed && s.take[0])
		g_Play.scan (s.take);			// a take ended: it is the one to play
	s_nRecWas = s.state;
	RuiPoll ();
	unsigned now = CTimer::GetClockTicks ();
	if (now - s_nLastLogUs < 10000000) return;
	s_nLastLogUs = now;
	g_Rec.status (&s, false);
	if (s.state != rec::Recorder::kRecording) return;
	CLogger::Get ()->Write (From, LogNotice, "%s %u s, %u KB written, card %u KB/s (need %u), worst write %u ms, ring %u/%u max %u, dropped %u, OUT short %u",
		s.take, (unsigned) (s.frames / 48000), (unsigned) (s.bytes >> 10), s.write_kbps, s.need_kbps,
		s.worst_write_us / 1000, s.ring_used, s.ring_slots, s.ring_max_used, s.drops, s.side_under);
}

static void Say (void)
{
	rec::Status s;
	g_Rec.status (&s, false);
	char ids[80];
	rec::list_ids (s.mask, ids, sizeof ids);
	static const char *State[] = {"idle", "armed", "recording", "finishing"};
	CLogger::Get ()->Write (From, LogNotice, "%s%s, %u-bit, prefix %s, channels %s (%u files, %u KB/s, ring %u ms), "
		"take %s %u s, dropped %u%s%s", State[s.state & 3], s.arm_mode ? " (PLAY/STOP)" : "", s.bits, s.prefix,
		ids[0] ? ids : "none", rec::files (s.mask), s.need_kbps, s.ring_ms, s.take[0] ? s.take : "-",
		(unsigned) (s.frames / 48000), s.drops, s.error[0] ? ", error: " : "", s.error);
}

static unsigned Num (const char *q)
{
	unsigned n = 0;
	for (; *q >= '0' && *q <= '9'; q++) n = n * 10 + (*q - '0');
	return n;
}

static void SayPlay (void)
{
	recplay::Status ps;
	g_Play.status (&ps);
	static const char *State[] = {"no take", "stopped", "playing", "stopping"};
	CLogger::Get ()->Write ("play", LogNotice, "%s: take %s (%d/%d), channel %s (%d/%d, %u-bit %s), %u.%01u / %u.%01u s, level %d, "
		"underruns %u%s%s", State[ps.state & 3], ps.take[0] ? ps.take : "-", ps.take_i + 1, ps.takes, ps.channel[0] ? ps.channel : "-",
		ps.ch_i + 1, ps.channels, ps.bits, ps.width == 2 ? "stereo" : "mono", (unsigned) (ps.pos / 48000),
		(unsigned) (ps.pos % 48000 / 4800), (unsigned) (ps.len / 48000), (unsigned) (ps.len % 48000 / 4800), ps.level,
		ps.underruns, ps.error[0] ? ", error: " : "", ps.error);
}

static bool PlayCommand (const char *p)
{
	char err[64] = "";
	bool ok = true;
	if (p[4] == '=')
	{
		const char *v = p + 5;
		if (strcmp (v, "start") == 0) ok = PlayStart (err, sizeof err);
		else if (strcmp (v, "stop") == 0) g_Play.stop ();
		else if (strcmp (v, "scan") == 0) g_Play.scan ();
		else if (strncmp (v, "take:", 5) == 0) g_Play.select_take ((int) Num (v + 5));
		else if (strncmp (v, "ch:", 3) == 0) g_Play.select_channel ((int) Num (v + 3));
		else if (strncmp (v, "level:", 6) == 0) g_Play.set_level ((int) Num (v + 6));
		else if (strncmp (v, "start:", 6) == 0)	// start:TAKE/NN - the web page's pick
		{
			char take[40];
			const char *sl = strchr (v + 6, '/');
			unsigned n = sl ? (unsigned) (sl - (v + 6)) : 0;
			if (!sl || n == 0 || n >= sizeof take) ok = false, strcpy (err, "play=start:TAKE/NN");
			else
			{
				memcpy (take, v + 6, n);
				take[n] = 0;
				g_Play.stop ();		// what plays fades out in the OUT fill (10 ms), then closes here
				for (unsigned t0 = CTimer::GetClockTicks (); g_Play.active () && CTimer::GetClockTicks () - t0 < 50000; )
					g_Play.poll ();
				g_Play.poll ();
				g_Play.scan ();
				if (g_Play.active ()) ok = false, strcpy (err, "busy: try again");
				else if (!g_Play.select_by_name (take, (int) Num (sl + 1))) ok = false, strcpy (err, "no such take");
				else ok = PlayStart (err, sizeof err);
			}
		}
		else
		{
			ok = false;
			strcpy (err, "play=start|stop|scan|take:N|ch:N|level:0..127|start:TAKE/NN");
		}
	}
	if (!ok) CLogger::Get ()->Write ("play", LogWarning, "%s: %s", p, err);
	SayPlay ();
	return true;
}

bool Command (const char *p)
{
	if (strncmp (p, "play", 4) == 0 && (p[4] == 0 || p[4] == '=')) return PlayCommand (p);
	if (strncmp (p, "rec", 3) != 0 || (p[3] != 0 && p[3] != '=')) return false;
	CLogger *L = CLogger::Get ();
	char err[64] = "";
	bool ok = true;
	if (p[3] == '=')
	{
		const char *v = p + 4;
		if ((strcmp (v, "start") == 0 || strcmp (v, "arm") == 0) && g_Play.active ()) g_Play.stop ();	// never both
		if (strcmp (v, "start") == 0) ok = g_Rec.start (err, sizeof err);
		else if (strcmp (v, "arm") == 0) ok = g_Rec.arm (err, sizeof err);
		else if (strcmp (v, "stop") == 0) g_Rec.stop ();
		else if (strncmp (v, "ch:", 3) == 0)
		{
			uint32_t m = g_Rec.mask ();
			ok = rec::parse_list (v + 3, &m, err, sizeof err) && g_Rec.configure (m, g_Rec.bits (), err, sizeof err);
		}
		else if (strncmp (v, "bits:", 5) == 0)
		{
			unsigned b = 0;
			for (const char *q = v + 5; *q >= '0' && *q <= '9'; q++) b = b * 10 + (*q - '0');
			ok = g_Rec.configure (g_Rec.mask (), b, err, sizeof err);
		}
		else if (strncmp (v, "name:", 5) == 0)
		{
			ok = g_Rec.set_prefix (v + 5);
			if (!ok) strcpy (err, "name: 1..24 of A-Z a-z 0-9 _ -");
		}
		else
		{
			ok = false;
			strcpy (err, "rec=start|stop|arm|ch:LIST|bits:16|24|name:PREFIX");
		}
	}
	if (!ok) L->Write (From, LogWarning, "%s: %s", p, err);
	Say ();
	return true;
}

void Status (CString *pOut)
{
	rec::Status s;
	g_Rec.status (&s, true);
	char ids[80];
	rec::list_ids (s.mask, ids, sizeof ids);
	for (char *c = s.error; *c; c++) if (*c == '"' || *c == '\\') *c = '\'';
	CString t;
	t.Format (",\"rec\":{\"state\":%d,\"arm\":%u,\"ready\":%u,\"bits\":%u,\"prefix\":\"%s\",\"take\":\"%s\",\"ch\":[%s],"
		  "\"frames\":%u,\"bytes\":%u,\"drops\":%u,\"silence\":%u,\"outShort\":%u,\"cardKBps\":%u,\"needKBps\":%u,"
		  "\"worstWriteMs\":%u,\"writes\":%u,\"ringMs\":%u,\"ringUsed\":%u,\"ringMax\":%u,\"ringSlots\":%u,"
		  "\"freeMB\":%u,\"totalMB\":%u,\"error\":\"%s\",\"peak\":[",
		  s.state, s.arm_mode, s.ready, s.bits, s.prefix, s.take, ids, (unsigned) s.frames, (unsigned) (s.bytes >> 10) /* KB */,
		  s.drops, s.dropped_frames, s.side_under, s.write_kbps, s.need_kbps, s.worst_write_us / 1000, s.writes,
		  s.ring_ms, s.ring_used, s.ring_max_used, s.ring_slots, (unsigned) (s.free_bytes >> 20),
		  (unsigned) (s.total_bytes >> 20), s.error);
	pOut->Append (t);
	for (int i = 0; i < rec::kSources; i++)
	{
		t.Format ("%s%u", i ? "," : "", s.peak[i] * 1000 / 8388607);	// per mille of full scale
		pOut->Append (t);
	}
	pOut->Append ("]}");
	recplay::Status ps;
	g_Play.status (&ps);
	for (char *c = ps.error; *c; c++) if (*c == '"' || *c == '\\') *c = '\'';
	t.Format (",\"play\":{\"state\":%d,\"take\":\"%s\",\"ch\":\"%s\",\"takeI\":%d,\"takes\":%d,\"chI\":%d,\"chs\":%d,"
		  "\"posMs\":%u,\"lenMs\":%u,\"level\":%d,\"underruns\":%u,\"error\":\"%s\"}",
		  ps.state, ps.take, ps.channel, ps.take_i, ps.takes, ps.ch_i, ps.channels, (unsigned) (ps.pos / 48),
		  (unsigned) (ps.len / 48), ps.level, ps.underruns, ps.error);
	pOut->Append (t);
}

void Transport (u8 nStatus, u16 nSPP)
{
	if (nStatus == 0xFA || nStatus == 0xFB || nStatus == 0xFC) g_Rec.transport (nStatus);
	sendfx_pi::Clock (nStatus, nSPP);
}

int UiState (unsigned *pSecs)
{
	rec::Status s;
	g_Rec.status (&s, false);
	*pSecs = (unsigned) (s.frames / 48000);
	return s.state == rec::Recorder::kArmed ? 1 : s.state == rec::Recorder::kIdle ? 0 : 2;
}

}  // namespace rec_pi

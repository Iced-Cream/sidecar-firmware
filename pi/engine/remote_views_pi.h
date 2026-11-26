// pi/engine/remote_views_pi.h - the remote UI's seven views, built from the engine's state whether or not a display exists
// (docs/remote-ui.md "No touch screen"). This is the kernel's view section, moved out of kernel.cpp so that it runs on the
// Mac: no Circle, no LVGL. Three small interfaces keep the three sides apart:
//   Remote   the remote service's four calls (remote_pi.h on the Pi; the real remote_srv session in tests/test_remote_views.cpp)
//   Engine   what the views are built from: the engine's state only, never a page or an LVGL object
//   Display  the local touch screen's setters and "page is shown" questions; a null pointer is a Pi with no screen
// The rule: a view is built while someone holds its gate (the local page shown, or a client subscribed) and handed to the client with
// Remote::Offer; the Display is touched only when it exists, and only for a page that shows. With no Display nothing is "shown".
#ifndef _pi_engine_remote_views_pi_h
#define _pi_engine_remote_views_pi_h

#include "../ui/remote/rmt_seqview.h"
#include "../ui/remote/rmt_views.h"
#include "../ui/sidecar_insfx.h"
#include "../ui/sidecar_mfx.h"
#include "../ui/sidecar_status.h"
#include "../ui/sidecar_voice.h"

namespace remote_views {

struct Remote
{
	virtual ~Remote () {}
	virtual bool Gate (int view, bool localShown) = 0;	// remote_pi::Gate
	virtual bool Offer (int view, const void *v) = 0;	// remote_pi::Offer
	virtual bool Due (int view) = 0;			// remote_pi::Due
};

// where the Pi's pattern view stands (rytmpat_pi::Get, as text for the PAT page's header)
struct PatWhere
{
	char text[48];
	int stale;						// the text is a warning
};

struct Engine
{
	virtual ~Engine () {}
	virtual unsigned NowUs () = 0;				// a wrapping microsecond clock: only differences are used
	virtual const char *Project () = 0;			// the project's name (m_Ui.project)
#if SIDECAR_SEQED
	virtual bool SeqView (sidecar_seq_view_t *v, unsigned *pSeen) = 0;	// core 3's newest sequencer view, when there is a new one
	virtual int Playhead () = 0;				// ms into the pattern, -1 stopped
	virtual void RytmPatWant (int nPage) = 0;		// 0 none, 1 PAT, 2 VOICE: keeps the Rytm pattern fetch alive
	virtual bool RytmPattern (rytm_pattern_t *p, int *pIndex, unsigned *pSeen) = 0;
	virtual void RytmPatWhere (PatWhere *w) = 0;
#endif
	virtual void VoiceFill (sidecar_voice_view_t *v, const rytm_pattern_t *pat, int nPatIndex) = 0;
	virtual sidecar_status_view_t *StatusView () = 0;	// the view the stats line fills in part; StatusFill does the rest
	virtual void StatusFill (sidecar_status_view_t *v) = 0;	// all but ui_max_us and gen
	virtual void InsFill (sidecar_insfx_view_t *v) = 0;
	virtual void MfxFill (sidecar_mfx_view_t *v) = 0;
};

struct Display
{
	virtual ~Display () {}
	virtual bool SeqShown () = 0;
	virtual bool PatShown () = 0;
	virtual bool VoiceShown () = 0;
	virtual bool StatusShown () = 0;
	virtual bool InsShown () = 0;
	virtual bool MfxShown () = 0;
#if SIDECAR_SEQED
	virtual void SeqView (const sidecar_seq_view_t *v) = 0;
	virtual void ShowSeq () = 0;				// the Rytm's TRK + TRIG asks for the SEQ page
	virtual void Playhead (int nPlay) = 0;
	virtual void RytmPat (const rytm_pattern_t *p, int nIndex, const char *project) = 0;
	virtual void RytmPatWhere (const char *text, int stale) = 0;
#endif
	virtual void VoiceSet (const sidecar_voice_view_t *v) = 0;
	virtual unsigned UiMaxUs () = 0;
	virtual void StatusSet (const sidecar_status_view_t *v) = 0;
	virtual void InsSet (const sidecar_insfx_view_t *v) = 0;
	virtual void MfxSet (const sidecar_mfx_view_t *v) = 0;
};

// One per kernel (its views are kept between calls). Run it every loop, with pDisplay 0 when the Pi has no screen.
class Feed
{
public:
	Feed ();
	void Run (Remote &r, Engine &e, Display *pDisplay);
private:
#if SIDECAR_SEQED
	sidecar_seq_view_t m_Seq;
	unsigned m_nSeqSeen, m_nShowReq, m_nPatSeen, m_nPatAt;
	bool m_bSeqNew, m_bPatNew;
	rytm_pattern_t m_RytmPat;				// the newest decoded Rytm pattern: the PAT page's, VOICE's p-locks
	int m_nRytmPatIndex;
	rmt_pat_view_t m_Pat;
#endif
	sidecar_voice_view_t m_Voice;
	sidecar_insfx_view_t m_Ins;
	sidecar_mfx_view_t m_Mfx;
	unsigned m_nVoiceAt, m_nStatusAt, m_nInsAt, m_nMfxAt;
};

}	// namespace remote_views

#endif

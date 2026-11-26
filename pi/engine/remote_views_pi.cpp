// pi/engine/remote_views_pi.cpp - see remote_views_pi.h. The kernel's view section for the remote UI: each view is built from the
// engine's state while its gate holds (the local page shown, or the client subscribed) and offered to the client; the local
// display, when there is one, is fed from the same build and only for a page that shows.
#include "remote_views_pi.h"

#include <string.h>

namespace remote_views {

Feed::Feed ()
{
	memset (this, 0, sizeof *this);
#if SIDECAR_SEQED
	m_nRytmPatIndex = -1;
#endif
}

void Feed::Run (Remote &r, Engine &e, Display *d)
{
#if SIDECAR_SEQED
	{					// the SEQ page: core 3's published view (when new) and the play head
		if (e.SeqView (&m_Seq, &m_nSeqSeen))
		{
			strncpy (m_Seq.project, e.Project (), sizeof m_Seq.project - 1);
			if (d != 0)
			{
				d->SeqView (&m_Seq);
				if (m_Seq.show_req != m_nShowReq)	// seq-ux: TRK + TRIG on the Rytm (rui1037; rui1036 FUNC + LFO): the SEQ page
				{
					m_nShowReq = m_Seq.show_req;
					d->ShowSeq ();
				}
			}
			m_bSeqNew = true;
		}
		const int nPlay = e.Playhead ();
		if (d != 0) d->Playhead (nPlay);
		// the remote client (docs/remote-ui.md): the view when it changed or the client has no snapshot (core 3 publishes on a change, so
		// this builds nothing of its own), and the play head quantised to 1/32 step, offered every loop (the session holds it to its rate)
		if (r.Gate (RMT_V_SEQ, d != 0 && d->SeqShown ()) && (m_bSeqNew || r.Due (RMT_V_SEQ)) && r.Offer (RMT_V_SEQ, &m_Seq))
			m_bSeqNew = false;
		if (r.Gate (RMT_V_SEQPOS, false))
		{
			rmt_seqpos_view_t sp;
			memset (&sp, 0, sizeof sp);
			sp.pos32 = nPlay < 0 ? -1 : (nPlay * 32 + 500) / 1000;
			r.Offer (RMT_V_SEQPOS, &sp);
		}
	}
	{					// the Rytm pattern view: fetched while open, its age / fetching shown
		const bool bVoiceShown = d != 0 && d->VoiceShown ();
		const bool bPatGate = r.Gate (RMT_V_PAT, d != 0 && d->PatShown ());	// this display's PAT page, or the remote client's
		const bool bVoiceGate = r.Gate (RMT_V_VOICE, bVoiceShown);
		int nPage = bVoiceShown ? 2 : bPatGate ? 1 : bVoiceGate ? 2 : 0;	// (the local VOICE page keeps its mode whatever the remote client shows)	// (VOICE: the track's p-locks; the remote client's VOICE too)
		bool bOpen = nPage != 0;
		e.RytmPatWant (nPage);
		int nIndex;
		if (e.RytmPattern (&m_RytmPat, &nIndex, &m_nPatSeen))
		{
			m_nRytmPatIndex = nIndex;
			if (d != 0) d->RytmPat (&m_RytmPat, nIndex, e.Project ());
			memcpy (&m_Pat.pat, &m_RytmPat, sizeof m_Pat.pat);
			m_Pat.index = nIndex, m_Pat.stale = 0;
			strncpy (m_Pat.project, e.Project (), sizeof m_Pat.project - 1);
			m_Pat.kit[0] = 0;
			strcpy (m_Pat.where, "from the Rytm");
			m_bPatNew = true;
		}
		unsigned nNow = e.NowUs ();
		if (bOpen && nNow - m_nPatAt >= 250000)
		{
			m_nPatAt = nNow;
			PatWhere W;
			memset (&W, 0, sizeof W);
			e.RytmPatWhere (&W);
			if (d != 0) d->RytmPatWhere (W.text, W.stale);
			if (strncmp (m_Pat.where, W.text, sizeof m_Pat.where - 1) != 0 || m_Pat.stale != W.stale)
			{
				strncpy (m_Pat.where, W.text, sizeof m_Pat.where - 1);
				m_Pat.stale = W.stale;
				m_bPatNew = true;
			}
		}
		if (bPatGate && (m_bPatNew || r.Due (RMT_V_PAT)) && r.Offer (RMT_V_PAT, &m_Pat)) m_bPatNew = false;
	}
#endif
	const bool bVoiceLocal = d != 0 && d->VoiceShown ();
	if (r.Gate (RMT_V_VOICE, bVoiceLocal))	// the VOICE page: ~10 frames a second, redrawn only on a change
	{
		unsigned nNow = e.NowUs ();
		if (nNow - m_nVoiceAt >= 100000)
		{
			m_nVoiceAt = nNow;
#if SIDECAR_SEQED
			e.VoiceFill (&m_Voice, m_RytmPat.valid ? &m_RytmPat : 0, m_nRytmPatIndex);
#else
			e.VoiceFill (&m_Voice, 0, -1);
#endif
			if (bVoiceLocal) d->VoiceSet (&m_Voice);
			r.Offer (RMT_V_VOICE, &m_Voice);
		}
	}
	const bool bStatusLocal = d != 0 && d->StatusShown ();
	if (r.Gate (RMT_V_STATUS, bStatusLocal))	// touch2: SIDECAR STATUS, twice a second (labels redrawn only on a change)
	{
		unsigned nNow = e.NowUs ();
		if (nNow - m_nStatusAt >= 500000)
		{
			m_nStatusAt = nNow;
			sidecar_status_view_t &sv = *e.StatusView ();
			e.StatusFill (&sv);
			sv.ui_max_us = d != 0 ? d->UiMaxUs () : 0;
			uint32_t h = 2166136261u;
			const uint8_t *b = (const uint8_t *) &sv;
			for (unsigned i = 0; i < __builtin_offsetof (sidecar_status_view_t, gen); i++) h = (h ^ b[i]) * 16777619u;
			sv.gen = h;
			if (bStatusLocal) d->StatusSet (&sv);
			r.Offer (RMT_V_STATUS, &sv);
		}
	}
	const bool bInsLocal = d != 0 && d->InsShown ();
	if (r.Gate (RMT_V_INSFX, bInsLocal))	// fx-ins: the INSERT FX page, ~10 frames a second, redrawn only on a change
	{
		unsigned nNow = e.NowUs ();
		if (nNow - m_nInsAt >= 100000)
		{
			m_nInsAt = nNow;
			e.InsFill (&m_Ins);
			if (bInsLocal) d->InsSet (&m_Ins);
			r.Offer (RMT_V_INSFX, &m_Ins);
		}
	}
	const bool bMfxLocal = d != 0 && d->MfxShown ();
	if (r.Gate (RMT_V_MFX, bMfxLocal))	// the MASTER FX page, the same way
	{
		unsigned nNow = e.NowUs ();
		if (nNow - m_nMfxAt >= 100000)
		{
			m_nMfxAt = nNow;
			e.MfxFill (&m_Mfx);
			if (bMfxLocal) d->MfxSet (&m_Mfx);
			r.Offer (RMT_V_MFX, &m_Mfx);
		}
	}
}

}	// namespace remote_views

// pi/ui/remote/rmt_seqview.h - the remote UI's two sequencer view structs that have no page of their own: the play head and the Rytm
// pattern with its header texts (docs/remote-ui.md). SEQ is sidecar_seq_view_t (../sidecar_seqed.h) as it is. Plain C, no LVGL.
#ifndef RMT_SEQVIEW_H
#define RMT_SEQVIEW_H

#include "../rytm_pattern.h"
#include "../sidecar_seqed.h"

#ifdef __cplusplus
extern "C" {
#endif

// The SEQ page's play head: the position in 1/32 steps (steps x 32), -1 stopped. Quantised on the Pi, so a smooth position moves the
// value only 32 times a step and a stopped sequencer sends nothing; sidecar_seqed_playhead takes steps x 1000 (pos32 * 1000 / 32).
typedef struct
{
	int		pos32;
	unsigned	gen;
} rmt_seqpos_view_t;

// The PAT page (sidecar_rytmpat_set / _where): the decoded pattern, its number and the texts the Pi's page shows
typedef struct
{
	rytm_pattern_t	pat;			// pat.valid 0: nothing fetched yet
	int		index;			// 0..127, -1
	int		stale;			// the shown pattern may be old (the text in the warning colour)
	char		project[24], kit[24], where[48];
	unsigned	gen;
} rmt_pat_view_t;

#ifdef __cplusplus
}
#endif

#endif

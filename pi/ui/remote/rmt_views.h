// pi/ui/remote/rmt_views.h - the remote UI's views on the wire (docs/remote-ui.md): STATUS, VOICE, INSERT FX, MASTER FX, SEQ, the SEQ
// play head and PAT, as sections. One table (the X-macros in rmt_views.c) generates the Pi's encoder, the app's decoder and the schema
// hash, so the two sides cannot drift apart. The structs are never copied as memory: pointers, alignment and layout
// stay each side's own; a section is its fields in the table's order, little-endian.
//
// Skew rules: a section's data is length-prefixed by its frame. A decoder takes the fields it finds and leaves the
// rest of the struct alone (an older sender: shorter data), and ignores bytes past the last field it knows (a newer
// sender). Sections and views are append-only; an unknown view or section is ignored. Arrays keep their sizes for good
// (a change is a new field); schema (per view) is bumped on a change of meaning, the high byte on an incompatible one.
#ifndef RMT_VIEWS_H
#define RMT_VIEWS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { RMT_V_STATUS = 1, RMT_V_VOICE = 2, RMT_V_INSFX = 3, RMT_V_MFX = 4, RMT_V_SEQ = 5, RMT_V_SEQPOS = 6, RMT_V_PAT = 7, RMT_V_COUNT = 8 };
#define RMT_MAX_SI	24		// section instances in a view at most (the delta state's size)
#define RMT_MAX_SEC	8
#define RMT_SEC_BUF	1024		// a section's encoding is never larger

typedef struct
{
	uint8_t id, rate_hz, nsec;	// rate_hz: the default feed rate
	uint16_t schema;
	size_t size;			// the view struct's size (the caller's buffer for it)
	const char *name;
} rmt_view_info_t;

typedef struct
{
	uint8_t ninst;			// instances (1 for a plain section)
	uint8_t hot;			// changes every frame (meters, traces): quantised already, at the subscription's rate
	const char *name;
} rmt_sec_info_t;

const rmt_view_info_t *rmt_view_info (int view);			// 0: not a view this build knows
const rmt_sec_info_t *rmt_sec_info (int view, int sec);			// 0: not a section
int rmt_view_nsi (int view);						// the section instances in all (<= RMT_MAX_SI)

// A section instance's bytes; the length, or -1 (unknown, or cap too small)
int rmt_sec_encode (int view, int sec, int inst, const void *v, uint8_t *out, int cap);
// Applies data to the view struct v (v->gen is bumped for any applied section: the UI redraws on a new gen); 0, or -1
// for an unknown view / section / instance. Short data fills the leading fields only; extra bytes are ignored.
int rmt_sec_decode (int view, int sec, int inst, void *v, const uint8_t *data, int len);

// a hash over every view's fields (names, kinds, sizes): equal on both sides when the schemas are identical
uint32_t rmt_schema_hash (void);

// The delta state for one view and one client: the hash of each section instance as last sent
typedef struct { uint32_t hash[RMT_MAX_SI]; uint8_t sent[RMT_MAX_SI]; } rmt_vtx_t;
typedef void (*rmt_emit_fn) (void *ud, int view, int sec, int inst, const uint8_t *data, int len);
void rmt_vtx_reset (rmt_vtx_t *t);					// forget: the next emit sends every section (RESYNC)
// Encodes every section of the view struct v and calls fn for each one that differs from what was last sent; the
// count. hot_only: only the sections marked hot (a cheaper pass between full ones)
int rmt_vtx_emit (rmt_vtx_t *t, int view, const void *v, rmt_emit_fn fn, void *ud, int hot_only);

#ifdef __cplusplus
}
#endif

#endif

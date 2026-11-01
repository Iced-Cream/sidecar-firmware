// pi/ui/sidecar_preset.h - the display's JUNO / 6-OP FM preset pages (0.4.0): the POOL page's fourth and fifth tabs. The
// same data as the Rytm's knob A on those machines and the web page's Pool tab (core/presets/preset_index.h): the project's
// list as rows ("000  name", an empty slot "---", the built-ins 128 / 96 rows when nothing is listed), and the RAM index of
// /JUNO and /DX7 as a browser (folders, FACTORY / ROM on top, banks as folders). Like the sample pools' pages: a tap on a
// filled row opens its menu (REPLACE / CANCEL), a tap on an empty row opens the index aimed at it, ADD opens the index to
// add into the list's first empty slot; in the index a folder opens, a preset goes into the list ("preset=juno|fm:add|set:..").
// The index is never read from the card here: a window of it (PRE_ROWS rows) is copied by the provider (preset_pi.cpp),
// the page keeps its own folder and place, so the Rytm's own list is not moved by it. LVGL only (builds on the Mac too).
#ifndef SIDECAR_PRESET_H
#define SIDECAR_PRESET_H

#ifdef __cplusplus
extern "C" {
#endif

#define PRE_ROWS	40		// the rows of a window (more than a page shows at 1280x720)
#define PRE_NAME	40		// a row's name shown

// What the page asks for (sidecar_preset_kind (), _folder (), _first ()) and what the provider fills. Rows are valid for
// the request they echo (kind, folder, first), else the page draws them empty and asks again.
typedef struct
{
	int		kind;			// 0 JUNO, 1 6-OP FM
	int		folder;			// -1 the project's list, else the index's folder (its roots are 0 and 1)
	int		first;			// the first row of the window
	int		ready;			// 1: rows are valid; 0: the index is being read, or changed under the copy
	unsigned	igen;			// the index's walk (the folder numbers belong to one)
	int		total, up, n;		// the rows of the folder / list, its parent folder (-1 a root), rows filled
	int		built_in, full;		// the list holds the built-ins; the index is full for this kind (the rest is left out)
	int		count[2];		// each list's rows (the tabs)
	char		title[48];		// the folder's path
	char		name[PRE_ROWS][PRE_NAME];
	unsigned char	flags[PRE_ROWS];	// 1 folder, 2 used (a list's slot with a preset)
	short		kid[PRE_ROWS];		// the folder a folder row opens
	unsigned	msg_seq;		// the last add's end (any page's): changes with each
	char		msg[40];
	unsigned	gen;			// changes with anything above
} sidecar_preset_view_t;

void sidecar_preset_create (void *parent, int y0);	// hidden until shown
void sidecar_preset_show (int on);
int sidecar_preset_shown (void);
void sidecar_preset_open (int kind);			// the page on that list (0 JUNO, 1 FM): its rows
void sidecar_preset_set (const sidecar_preset_view_t *v);	// copied
int sidecar_preset_asking (void);			// a slot's menu is open (tests)
int sidecar_preset_index_shown (void);			// the index (not the list) is shown
int sidecar_preset_kind (void);
int sidecar_preset_folder (void);
int sidecar_preset_first (void);
int sidecar_preset_count (int kind);			// the list's rows as last seen (the POOL page's tabs), -1 unknown
// Commands through fn (sidecar_ui's); the POOL page's tabs through pool_fn (list 0..2: that POOL list, the POOL page
// again; -1: DONE), the POOL page's JUNO / FM tabs through sidecar_wgview_set_preset
void sidecar_preset_set_command (void (*fn) (const char *cmd));
void sidecar_preset_set_pool (void (*fn) (int list));

#ifdef __cplusplus
}
#endif

#endif

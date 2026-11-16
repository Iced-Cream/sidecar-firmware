// pi/ui/sidecar_xy.h - the Pi display's XY page (0.5.0, core/mod/xypad.h): a "Kaoss pad". The pad takes most of the page
// (a dot at the finger with a short trail; X right, Y up), the HOLD toggle and SETUP beside it. SETUP shows X's and Y's
// four slots (a target key and a depth slider each); a target key opens the picker (the kind, then its index and
// parameter: the LFOs' target list, plus MORPH, the scenes' fader; a param by its machine's / TYPE's name, "P3 HRM"). Every edit goes back as a console line through the
// command callback, as the MORPH page's do: xy=pos:X:Y while a finger drags (at most every 30 ms, and always the last
// position), xy=up at the release or a lost press, xy=hold:0|1, xy=set:x|y:SLOT:TARGET:DEPTH, xy=clear:x|y:SLOT. The
// UI never calls the engine's modules. LVGL only; both themes, every size from TH->w / TH->h.
#ifndef SIDECAR_XY_H
#define SIDECAR_XY_H

#ifdef __cplusplus
extern "C" {
#endif

#define SXY_SLOTS	4
#define SXY_MAX		16383		// a position's top (xy=pos)

typedef struct
{
	char		name[16];		// "" none, "MORPH", "RYTM:PERF12", "V8:P16" (the id: xy=set takes it)
	char		label[16];		// as the screens name it: "V8:SEED" (the voice's machine's label), "MORPH"
	signed char	depth;			// -100..100 %
} sidecar_xy_slot_t;

typedef struct
{
	int			valid;
	unsigned char		hold;		// HOLD on
	unsigned char		touching;	// the engine has a finger on the pad
	unsigned char		active;		// touching, or HOLD's held values
	unsigned char		locked;		// the setup file is newer than this build: not saved
	unsigned short		x, y;		// the engine's position, 0..SXY_MAX
	sidecar_xy_slot_t	s[2][SXY_SLOTS];	// [0] X, [1] Y
	// the picker's names of the params that have no fixed name, as the engine labels them now (core/mod/target.h
	// param_label; "" or "-": the machine / TYPE leaves it unused): voice v's P1..P16 by its machine, send FX 1 / 2's
	// P2..P5 by its TYPE, INSERT chain c's (I1..I8 B1..B3 M1 M2) A1..A4 B1..B4 E1..E4 ("ACUT", "QHPF")
	char			vl[8][16][5];
	char			fl[2][4][5];
	char			il[13][12][6];
	// 0.5.0: RT1..RT12's P1..P8 as the track's Rytm machine labels them ("TUN"; "P1" the machine not known)
	char			rl[12][8][6];
	// 0.5.0: each Rytm track's machine for the picker's TRACK list: 0 not known, 1 a Sidecar machine, 2 a stock one
	unsigned char		tm[12];
	unsigned		gen;		// changes with anything above (a hash)
} sidecar_xy_view_t;

typedef void (*sidecar_xy_command_t) (const char *cmd);
void sidecar_xy_create (void *parent, int y0);		// hidden until shown
void sidecar_xy_set_command (sidecar_xy_command_t fn);
void sidecar_xy_show (int on);
int sidecar_xy_shown (void);
void sidecar_xy_set (const sidecar_xy_view_t *v);	// copied; cheap when gen is unchanged
// 0.5.1 HOME > Perform: the same pad, its HOLD and SETUP keys and the slots' list on the page's own parent (rectangles x, y, w, h in
// the parent's coordinates); SETUP calls setup (the page opens FX > XY on its setup). The view is wanted while the XY page, Perform
// or HOME > Overview (its XY tile: HOLD and the first targets) shows: sidecar_xy_wanted () is what the engine's feed asks.
typedef struct
{
	int	pad[4], hold[4], setup_key[4], list[4];	// the list's rectangle is in list_parent's coordinates
	void	*list_parent;
	void	(*setup) (void);
} sidecar_xy_perform_t;
void sidecar_xy_perform_create (void *parent, const sidecar_xy_perform_t *g);	// after sidecar_xy_create
void sidecar_xy_perform_show (int on);
void sidecar_xy_overview (int on);
int sidecar_xy_wanted (void);
int sidecar_xy_perform_hit (int what, int *x, int *y, int *w, int *h);	// Perform's pad (0), HOLD (1), SETUP (2) in UI coordinates (tests)
void sidecar_xy_axes (char *x, char *y, int cap);	// the first target of X and of Y as the screens name it, "" none
// the page's own state (the host's screenshots, tests)
void sidecar_xy_setup (int on);				// SETUP (1) or the pad (0)
int sidecar_xy_setup_shown (void);
void sidecar_xy_pick (int axis, int slot);		// the picker for X (0) / Y (1) slot 0..3; -1: closed
int sidecar_xy_picking (void);				// the picker's axis * 4 + slot, -1 closed
void sidecar_xy_pad_area (int *x, int *y, int *w, int *h);	// the pad in UI coordinates
void sidecar_xy_hold_key (void);			// a tap on HOLD
int sidecar_xy_hold_shown (void);			// the HOLD key's state as shown
const char *sidecar_xy_text (int which);		// what a label shows: 0 the picker's title, 1 its parameter, 2 + n summary line n

#ifdef __cplusplus
}
#endif

#endif

// pi/ui/sidecar_mfx.h - the Pi display's MASTER FX page (docs/MASTER-FX.md): the master chain's four slots, each a TYPE key
// (a tap: the next TYPE the slot can take, the caller runs "mfx=N:type:T"), P1..P4 as bars with the TYPE's labels and values,
// MIX for a send TYPE and L/R for an AMP; ON / BYPASS, SRC RET / MIX; LEV, the chain's in and out meters, the POST EQ's line;
// and a line saying what the chain processes. LVGL only; both themes, every size: the layout comes from TH->w / TH->h, not
// from fixed places, so 800x480, 1024x600 and 1280x720 are one code path.
#ifndef SIDECAR_MFX_H
#define SIDECAR_MFX_H

#ifdef __cplusplus
extern "C" {
#endif

#define SMF_SLOTS	4

typedef struct
{
	int		type;			// 0 OFF ..
	char		name[8];		// "COMP"
	char		next[8];		// the TYPE a tap gives ("OFF" when nothing else fits)
	char		label[4][5];		// "THR" ..
	char		val[4][8];		// "-20DB" ..
	unsigned char	v[4];			// 0..127 as it plays
	unsigned char	send;			// 1: a send TYPE (MIX means something)
	unsigned char	mix;			// 0..127
	unsigned char	amp;			// 1: AMP
	unsigned char	pair;			// 1: an L/R pair
} sidecar_mfx_slot_t;

typedef struct
{
	int			valid;
	unsigned char		on;		// a slot or the POST EQ is set
	unsigned char		byp;
	unsigned char		mix;		// SRC MIX
	unsigned char		rep;		// the Rytm reports pans and routing
	unsigned char		onmain;		// stems still on ROUTE TO MAIN (with rep)
	unsigned char		lev;		// 0..127, 100 unity
	unsigned char		in, out;	// peaks, 0..127
	sidecar_mfx_slot_t	slot[SMF_SLOTS];
	char			peq[56];	// "" flat, else "HPF 420  LPF 6.2K ..."
	unsigned		gen;		// changes with anything above
} sidecar_mfx_view_t;

typedef void (*sidecar_mfx_command_t) (const char *cmd);
void sidecar_mfx_create (void *parent, int y0);	// hidden until shown
void sidecar_mfx_set_command (sidecar_mfx_command_t fn);
void sidecar_mfx_show (int on);
int sidecar_mfx_shown (void);
void sidecar_mfx_set (const sidecar_mfx_view_t *v);	// copied; cheap when gen is unchanged

#ifdef __cplusplus
}
#endif

#endif

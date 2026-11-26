// pi/ui/sidecar_mfx.c - see sidecar_mfx.h. Four columns, one a slot; each row a label with its value over a bar. No fixed
// places: columns from the page's width, rows from its height, so every screen size takes the same code.
#include "sidecar_mfx.h"
#include "sidecar_theme.h"
#include "lvgl.h"
#include <string.h>

#define ROWS	5	// P1..P4, then MIX or L/R

static lv_obj_t *s_Page, *s_TitleBar, *s_Title, *s_Byp, *s_Src, *s_LevL, *s_Lev, *s_In, *s_Out, *s_Type[SMF_SLOTS], *s_Pair[SMF_SLOTS],
	*s_Lab[SMF_SLOTS][ROWS], *s_Bar[SMF_SLOTS][ROWS], *s_Eq, *s_Info;
static sidecar_mfx_view_t s_V;
static unsigned s_nGen = ~0u;
static sidecar_mfx_command_t s_Cmd;

static int oled (void) { return TH->id == THEME_OLED; }
static void cmd (const char *c) { if (s_Cmd) s_Cmd (c); }

static void byp_cb (lv_obj_t *o, int i, void *ud) { (void) o; (void) i; (void) ud; cmd (s_V.byp ? "mfx=byp:0" : "mfx=byp:1"); }
static void src_cb (lv_obj_t *o, int i, void *ud) { (void) o; (void) i; (void) ud; cmd (s_V.mix ? "mfx=src:ret" : "mfx=src:mix"); }
static void type_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int s = (int) (intptr_t) ud;
	char c[40];
	lv_snprintf (c, sizeof c, "mfx=%d:type:%s", s + 1, s_V.slot[s].next);
	cmd (c);
}
static void pair_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int s = (int) (intptr_t) ud;
	char c[24];
	lv_snprintf (c, sizeof c, "mfx=%d:pair:%d", s + 1, s_V.slot[s].pair ? 0 : 1);
	cmd (c);
}

// the touch edits (th_meter_edit): the console's lines an encoder turn sends. ud: slot * 8 + row (row 0..3 P1..P4, 4 MIX), -1 LEV.
// MIX and LEV's texts follow the finger at once (the P rows' texts are the engine's: they follow it)
static void mix_text (int s, int v)
{
	char t[16];
	lv_snprintf (t, sizeof t, "MIX %d", v);
	th_label_set (s_Lab[s][4], t);
}
static void lev_text (int v)
{
	char t[16];
	lv_snprintf (t, sizeof t, "LEV %d", v);
	th_label_set (s_LevL, t);
}
static void bar_cb (lv_obj_t *o, int v, void *ud)
{
	(void) o;
	int u = (int) (intptr_t) ud, s = u >> 3, r = u & 7;
	char c[32];
	v &= 0xFFFF;
	if (r == 4) lv_snprintf (c, sizeof c, "mfx=%d:mix:%d", s + 1, v), mix_text (s, v);
	else lv_snprintf (c, sizeof c, "mfx=%d:p%d:%d", s + 1, r + 1, v);
	cmd (c);
}
static void lev_cb (lv_obj_t *o, int v, void *ud)
{
	(void) o; (void) ud;
	char c[24];
	v &= 0xFFFF;
	lv_snprintf (c, sizeof c, "mfx=lev:%d", v);
	lev_text (v);
	cmd (c);
}

void sidecar_mfx_create (void *parent, int y0)
{
	const int o = oled ();
	s_nGen = ~0u;
	memset (&s_V, 0, sizeof s_V);
	const int pw = o ? TH->w - 48 : th_page_w (), ph = TH->h - y0 - (o ? LU (25) : 0);
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, pw, ph + (o ? LU (25) : 0));
	s_Page = pg;
	const lv_font_t *f = o ? TH->f_micro : TH->f_small;
	const int kind = o ? TH_KEY | TH_KEY_SMALL : TH_KEY, ink = o ? TH->ink : TH->win_ink, dim = o ? TH->dim : TH->win_dim;
	const int x0 = o ? 0 : LU (24), w = o ? pw : pw - LU (48), top = o ? LU (16) : TH->dy ? LU (44) : LU (14), keyh = o ? LU (22) + (TH->pdy > 0 ? TH->pdy / 12 : 0) : LU (44) + (TH->pdy > 0 ? TH->pdy / 8 : 0);	// (0.5.1: touch size; at 800x480 the title makes room: the sub-page row names the page)
	const int foot = o ? LU (22) : LU (54);	// the POST EQ's line and the hint
	if (o)
	{
		s_TitleBar = th_titlebar (pg, 0, 0, pw, LU (13));
		th_titlebar_set (s_TitleBar, "MASTER FX", "", 0);
		s_Title = 0;
	}
	else
	{
		s_TitleBar = 0;
		th_panel (pg, LU (8), LU (6), pw - LU (16), ph - LU (14), "", TH_PANEL_WINDOW);
		s_Title = th_label (pg, LU (24), LU (14), TH->f_mid, TH->win_ink, "MASTER FX");
		if (!TH->dy) lv_obj_add_flag (s_Title, LV_OBJ_FLAG_HIDDEN);
	}
	// the top row: ON / BYPASS, SRC, LEV, the meters
	const int kw = w / 4 - 4;
	s_Byp = th_key (pg, x0, top, kw, keyh, "ON", kind, byp_cb, 0);
	s_Src = th_key (pg, x0 + w / 4, top, kw, keyh, "SRC RET", kind, src_cb, 0);
	s_LevL = th_label (pg, x0 + w / 2, top, f, ink, "LEV");
	lv_obj_set_width (s_LevL, kw);
	s_Lev = th_meter (pg, x0 + w / 2, top, kw, keyh, 127, TH_METER_H, ink);		// (the label's place too: the touch target)
	th_meter_inset (s_Lev, 0, keyh - keyh / 3, 0, 1);
	th_meter_edit (s_Lev, lev_cb, 0);
	s_In = th_meter (pg, x0 + w * 3 / 4, top + 1, w / 4, keyh / 2 - 2, 127, TH_METER_H, dim);
	s_Out = th_meter (pg, x0 + w * 3 / 4, top + keyh / 2 + 1, w / 4, keyh / 2 - 2, 127, TH_METER_H, ink);
	// four columns
	const int y1 = top + keyh + LU (o ? 3 : 8), cw = w / SMF_SLOTS, y2 = y1 + keyh + LU (o ? 3 : 6);
	const int rh = (ph - (o ? 0 : LU (14)) - foot - y2) / ROWS, bh = LU (o ? 5 : 8) + TH->pdy / 30;
	for (int s = 0; s < SMF_SLOTS; s++)
	{
		const int x = x0 + s * cw;
		s_Type[s] = th_key (pg, x, y1, cw - 4, keyh, "1 OFF", kind, type_cb, (void *) (intptr_t) s);
		for (int r = 0; r < ROWS; r++)
		{
			s_Lab[s][r] = th_label (pg, x, y2 + r * rh, f, dim, "");
			lv_obj_set_width (s_Lab[s][r], cw - 4);
			s_Bar[s][r] = th_meter (pg, x, y2 + r * rh, cw - 6, rh, 127, TH_METER_H, ink);	// (the label's row: the touch target)
			th_meter_inset (s_Bar[s][r], 0, rh - bh - 2, 0, 2);
			th_meter_edit (s_Bar[s][r], bar_cb, (void *) (intptr_t) (s * 8 + r));
		}
		const int py = y2 + (ROWS - 1) * rh, pk = rh - 2 < keyh ? keyh : rh - 2, room = ph - foot - py - 1;	// (0.5.1: the touch size, into the gap above the EQ line)
		s_Pair[s] = th_key (pg, x, py, cw - 4, pk < room ? pk : room, "MONO", kind, pair_cb, (void *) (intptr_t) s);
		lv_obj_add_flag (s_Pair[s], LV_OBJ_FLAG_HIDDEN);
	}
	s_Eq = th_label (pg, x0, ph - foot, f, dim, "");
	s_Info = th_label (pg, x0, ph - foot / 2 - (o ? 0 : LU (8)), f, o ? TH->faint : dim, "");
	lv_obj_set_width (s_Eq, w);
	lv_obj_set_width (s_Info, w);
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
}

void sidecar_mfx_set_command (sidecar_mfx_command_t fn) { s_Cmd = fn; }

void sidecar_mfx_show (int on)
{
	if (!s_Page) return;
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
}

void *sidecar_mfx_obj (int what)
{
	return what == 100 ? s_Lev : what >= 0 && what < SMF_SLOTS * 8 && (what & 7) < ROWS ? s_Bar[what >> 3][what & 7] : 0;
}

int sidecar_mfx_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }

static void refresh (void)
{
	const sidecar_mfx_view_t *v = &s_V;
	char t[96];
	const char *r = !v->on ? "NO CHAIN" : v->byp ? "BYPASS" : "ON";
	if (s_TitleBar) th_titlebar_set (s_TitleBar, "MASTER FX", r, 0);
	else { lv_snprintf (t, sizeof t, "MASTER FX  %s", r); th_label_set (s_Title, t); }
	th_key_text (s_Byp, v->byp ? "BYPASS" : "ON");
	th_key_check (s_Byp, !v->byp && v->on);
	th_key_text (s_Src, v->mix ? "SRC MIX" : "SRC RET");
	th_key_check (s_Src, v->mix);
	th_meter_set (s_Lev, v->lev);
	th_meter_edit_set (s_Lev, v->lev, v->lev_def, v->valid);
	lev_text (th_meter_editing (s_Lev) ? th_meter_get (s_Lev) : v->lev);
	th_meter_set (s_In, v->in);
	th_meter_set (s_Out, v->out);
	for (int s = 0; s < SMF_SLOTS; s++)
	{
		const sidecar_mfx_slot_t *x = &v->slot[s];
		lv_snprintf (t, sizeof t, "%d %s", s + 1, x->type ? x->name : "OFF");
		th_key_text (s_Type[s], t);
		th_key_check (s_Type[s], x->type != 0);
		for (int k = 0; k < 4; k++)
		{
			if (x->type) lv_snprintf (t, sizeof t, "%s %s", x->label[k], x->val[k]);
			else t[0] = 0;
			th_label_set (s_Lab[s][k], t);
			th_meter_set (s_Bar[s][k], x->type ? x->v[k] : -1);
			th_meter_edit_set (s_Bar[s][k], x->base[k], x->def[k], v->valid && x->type);
		}
		th_meter_set (s_Bar[s][4], x->send ? x->mix : -1);
		th_meter_edit_set (s_Bar[s][4], x->mix, x->mixdef, v->valid && x->send);
		if (x->send) mix_text (s, th_meter_editing (s_Bar[s][4]) ? th_meter_get (s_Bar[s][4]) : x->mix);
		else th_label_set (s_Lab[s][4], "");
		if (x->amp)
		{
			lv_obj_remove_flag (s_Pair[s], LV_OBJ_FLAG_HIDDEN);
			th_key_text (s_Pair[s], x->pair ? "L/R PAIR" : "L+R ONE");
			th_key_check (s_Pair[s], x->pair);
		}
		else lv_obj_add_flag (s_Pair[s], LV_OBJ_FLAG_HIDDEN);
	}
	if (v->peq[0]) lv_snprintf (t, sizeof t, "POST EQ: %s", v->peq);
	else t[0] = 0;
	th_label_set (s_Eq, t);
	if (v->mix && v->rep && v->onmain) lv_snprintf (t, sizeof t, "MIX: %d VOICES ON MAIN STAY ANALOG", v->onmain);
	th_label_set (s_Info, !v->valid ? "WAITING FOR THE ENGINE"
		: v->mix && v->rep ? (v->onmain ? t : "MIX: 8 STEMS, PANNED, THROUGH THE CHAIN")
		: v->mix ? "MIX: STEMS, MONO. TRACKS OFF ROUTE TO MAIN"
			 : "RET: THE SIDECAR'S RETURNS, NOT THE RYTM'S MIX");
}

void sidecar_mfx_set (const sidecar_mfx_view_t *v)
{
	if (!s_Page || v->gen == s_nGen) return;
	for (int s = 0; s < SMF_SLOTS; s++)	// a slot's TYPE changed: its bars are other parameters now, a drag on them stops
		if (v->slot[s].type != s_V.slot[s].type)
			for (int r = 0; r < ROWS; r++) th_meter_edit_cancel (s_Bar[s][r]);
	s_V = *v;
	s_nGen = v->gen;
	refresh ();
}

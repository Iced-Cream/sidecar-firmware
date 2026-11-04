// pi/ui/sidecar_ui_generic.c - sidecar_ui.h for the generic engine (pi/engine GENERIC=1, the engine strip-down
// 2026-11-04): after the boot screen (sidecar_splash.h) one page, the Sidecar's status - the Rytm and the link, the
// project, the network and the web page, the computer's audio, the SoC, the build - and the warning banner (the supply,
// the temperature) on its last line. No controls: the pages of the full engine (sidecar_ui.c and the page files) stay in
// the tree, unwired. The display's settings (style, screensaver, brightness, touch calibration) still come from the
// console and /.rytm/display.txt, as before; this page only follows them. LVGL only (the Mac's host build too).
#include "sidecar_ui.h"
#include "sidecar_theme.h"
#include "lvgl.h"

#include <string.h>

enum { ROW_RYTM, ROW_LINK, ROW_PROJECT, ROW_NET, ROW_WEB, ROW_AUDIO, ROW_SOC, ROW_BUILD, ROW_N };
static const char *const kRowName[ROW_N] = {"RYTM", "LINK", "PROJECT", "NETWORK", "WEB PAGE", "COMPUTER", "SOC", "BUILD"};

static lv_obj_t *s_Title, *s_Row[ROW_N], *s_Note, *s_Dim;
static int s_nStyle = THEME_OLED, s_nBuilt;
static int s_PhysW, s_PhysH;
static int s_nDim = 100;
static sidecar_ui_status_t s_St;
static int s_bHaveSt;
static sidecar_ui_command_t s_Cmd;
static void *s_CmdParam;
static char s_Banner[96];

static int oled (void) { return s_nStyle == THEME_OLED; }

static void dim_layer (void)
{
	if (oled ())						// the dot expansion dims (sidecar_ui_bright): a redraw
	{
		lv_obj_invalidate (lv_screen_active ());
		return;
	}
	if (!s_Dim)
	{
		s_Dim = lv_obj_create (lv_layer_top ());
		lv_obj_remove_style_all (s_Dim);
		lv_obj_set_size (s_Dim, LV_PCT (100), LV_PCT (100));
		lv_obj_set_style_bg_color (s_Dim, lv_color_hex (0x000000), 0);
		lv_obj_remove_flag (s_Dim, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_flag (s_Dim, LV_OBJ_FLAG_IGNORE_LAYOUT);
	}
	lv_obj_set_style_bg_opa (s_Dim, (lv_opa_t) ((100 - s_nDim) * 255 * 70 / (95 * 100)), 0);
	if (s_nDim >= 100) lv_obj_add_flag (s_Dim, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_remove_flag (s_Dim, LV_OBJ_FLAG_HIDDEN);
}

static void build (void)
{
	lv_obj_t *scr = lv_screen_active ();
	lv_obj_set_style_bg_color (scr, lv_color_hex (TH->bg), 0);
	lv_obj_set_style_bg_opa (scr, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color (scr, lv_color_hex (TH->ink), 0);
	lv_obj_set_style_text_font (scr, TH->f_body, 0);
	lv_obj_remove_flag (scr, LV_OBJ_FLAG_SCROLLABLE);
	const int tbh = oled () ? LU (13) : LU (52);
	s_Title = th_titlebar (scr, 0, 0, TH->w, tbh);
	th_titlebar_set (s_Title, oled () ? "SIDECAR" : "Sidecar", oled () ? "STATUS" : "Status", 0);
	const int x0 = oled () ? LU (6) : LU (24), x1 = oled () ? LU (90) : LU (200);
	const int top = tbh + (oled () ? LU (8) : LU (20));
	const int step = oled () ? LU (24) : LU (44);
	const lv_font_t *f = oled () ? TH->f_body : TH->f_mid ? TH->f_mid : TH->f_body;
	for (int i = 0; i < ROW_N; i++)
	{
		th_label (scr, x0, top + i * step, f, TH->dim, kRowName[i]);
		s_Row[i] = th_label (scr, x1, top + i * step, f, TH->ink, "-");
		lv_obj_set_width (s_Row[i], TH->w - x1 - x0);
		lv_label_set_long_mode (s_Row[i], LV_LABEL_LONG_MODE_CLIP);
	}
	s_Note = th_label (scr, x0, TH->h - (oled () ? LU (14) : LU (34)), TH->f_small, TH->dim, "");
	lv_obj_set_width (s_Note, TH->w - 2 * x0);
	lv_label_set_long_mode (s_Note, LV_LABEL_LONG_MODE_CLIP);
	s_Dim = 0;
	dim_layer ();
	s_nBuilt = 1;
	if (s_bHaveSt)
	{
		sidecar_ui_status_t st = s_St;
		sidecar_ui_status (&st);
	}
}

void sidecar_ui_create (void)
{
	lv_display_t *d = lv_display_get_default ();
	if (!s_PhysW)
	{
		s_PhysW = (int) lv_display_get_horizontal_resolution (d);
		s_PhysH = (int) lv_display_get_vertical_resolution (d);
	}
	sidecar_theme_screen (s_PhysW, s_PhysH);
	sidecar_theme_set (s_nStyle);
	int uw = s_PhysW / TH->dot, uh = s_PhysH / TH->dot;
	lv_display_set_resolution (d, uw < TH->w ? uw : TH->w, uh < TH->h ? uh : TH->h);
	build ();
}

void sidecar_ui_style (int style)
{
	style = style == THEME_CLASSIC ? THEME_CLASSIC : THEME_OLED;
	if (style == s_nStyle && s_nBuilt) return;
	s_nStyle = style;
	if (!s_nBuilt) return;
	lv_obj_clean (lv_layer_top ());
	lv_obj_clean (lv_screen_active ());
	sidecar_ui_create ();
	lv_obj_invalidate (lv_screen_active ());
}

int sidecar_ui_get_style (void) { return s_nStyle; }
int sidecar_ui_dot (void) { return TH->dot; }

void sidecar_ui_origin (int *x, int *y)
{
	int x0 = (s_PhysW - TH->w * TH->dot) / 2, y0 = (s_PhysH - TH->h * TH->dot) / 2;
	*x = x0 > 0 ? x0 : 0;
	*y = y0 > 0 ? y0 : 0;
}

void sidecar_ui_to_ui (int *x, int *y)
{
	int ox, oy, d = TH->dot;
	sidecar_ui_origin (&ox, &oy);
	lv_display_t *dd = lv_display_get_default ();
	int w = (int) lv_display_get_horizontal_resolution (dd), h = (int) lv_display_get_vertical_resolution (dd);
	int ux = (*x - ox) / d, uy = (*y - oy) / d;
	*x = ux < 0 ? 0 : ux >= w ? w - 1 : ux;
	*y = uy < 0 ? 0 : uy >= h ? h - 1 : uy;
}

int sidecar_ui_bright (void)
{
	if (!oled ()) return 256;
	return 256 - (100 - s_nDim) * 256 * 70 / (95 * 100);
}

void sidecar_ui_dim (int percent)
{
	if (percent < 5) percent = 5;
	if (percent > 100) percent = 100;
	if (percent == s_nDim) return;
	s_nDim = percent;
	if (s_nBuilt) dim_layer ();
}

static void row (int i, const char *t)
{
	if (s_Row[i]) th_label_set (s_Row[i], t && *t ? t : "-");
}

void sidecar_ui_status (const sidecar_ui_status_t *s)
{
	s_St = *s;
	s_bHaveSt = 1;
	if (s->ui_style >= 0 && s->ui_style != s_nStyle)
	{
		sidecar_ui_style (s->ui_style);
		return;						// (the rebuild applied s_St)
	}
	if (s->brightness > 0) sidecar_ui_dim (s->brightness);
	if (s->ui_look >= 0 && s->ui_look != sidecar_dot_get_look ()) sidecar_dot_look (s->ui_look);
	if (!s_nBuilt) return;
	char b[96];
	row (ROW_RYTM, s->rytm ? (oled () ? "CONNECTED" : "connected") : (oled () ? "NOT CONNECTED" : "not connected"));
	row (ROW_LINK, s->link);
	row (ROW_PROJECT, s->project);
	lv_snprintf (b, sizeof b, "%s%s%s", s->ip, s->host[0] ? "  " : "", s->host);
	row (ROW_NET, b);
	row (ROW_WEB, s->web ? (oled () ? "ON" : "on") : (oled () ? "OFF" : "off"));
	row (ROW_AUDIO, s->mac < 0 ? (oled () ? "NO USB-C GADGET" : "no USB-C gadget") : s->mac ? (oled () ? "AUDIO ON" : "audio on")
										     : (oled () ? "AUDIO OFF" : "audio off"));
	if (s->clock_low_mhz > 0) lv_snprintf (b, sizeof b, "%d C, %d MHz (lowered)", s->temp_c, s->clock_low_mhz);
	else if (s->temp_c > 0) lv_snprintf (b, sizeof b, "%d C, %d MHz setting", s->temp_c, s->cpu_mhz);
	else b[0] = 0;
	row (ROW_SOC, b);
	lv_snprintf (b, sizeof b, "%s  %s", s->build, s->circle);
	row (ROW_BUILD, b);
	// the warning banner's text (the supply's, the temperature's) on the last line, else the link note
	s_Banner[0] = 0;
	if (s->temp_warn) lv_snprintf (s_Banner, sizeof s_Banner, "SoC %d C: the temperature warning is on", s->temp_c);
	const char *note = s_Banner[0] ? s_Banner : s->note ? s->note : "";
	th_label_set (s_Note, note);
	th_label_color (s_Note, s_Banner[0] ? TH->bad : TH->dim);
}

void sidecar_ui_set_command (sidecar_ui_command_t fn, void *param)
{
	s_Cmd = fn;
	s_CmdParam = param;
}

const char *sidecar_ui_banner (void) { return s_Banner[0] ? s_Banner : 0; }
void sidecar_ui_show (int page) { (void) page; }
int sidecar_ui_page (void) { return 0; }
int sidecar_ui_nav_lit (void) { return -1; }
const char *sidecar_ui_nav_page (int i) { (void) i; return 0; }
const char *sidecar_ui_nav_key (int i) { (void) i; return 0; }
void sidecar_ui_combo_map (int on) { (void) on; }
int sidecar_ui_combo_map_shown (void) { return 0; }
unsigned sidecar_ui_taps (void) { return 0; }
int sidecar_ui_touch_test_shown (void) { return 0; }
void sidecar_ui_touch_cal_start (void) {}
int sidecar_ui_touch_cal_step (void) { return -1; }
void sidecar_ui_touch (int pressed, int raw_x, int raw_y, int x, int y) { (void) pressed, (void) raw_x, (void) raw_y, (void) x, (void) y; }

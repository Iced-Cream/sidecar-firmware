// pi/ui/sidecar_theme.c - see sidecar_theme.h. The four themes' tables, the dot fonts as LVGL fonts, the widgets and their
// painters. Every widget is a plain lv_obj drawn in its DRAW_MAIN by the current theme's painter: no LVGL theme, no
// styles per state, so a theme switch is a table pointer and a rebuild (sidecar_ui_style).
#include "sidecar_theme.h"
#include "sidecar_logo.h"	// the "Docked" logo (th_paint_logo)
#include "src/misc/lv_area_private.h"	// lv_area_intersect
#include "src/misc/lv_text_private.h"	// lv_text_get_width
#include <string.h>

// ---- the dot fonts as LVGL fonts (A8 glyphs of 0 / 255: no anti-aliasing, so the dot grid stays crisp)

typedef struct { const dotfont_t *f; uint8_t s, bold, round; } dotlv_t;	// round (SIDECAR): each dot a round dot, not a block

// a round dot's alpha in its s x s cell: 2 the OLED grid's own cell [c c/2; c/2 c/4] (sidecar_dot.h), 3 and more a disc
// of 0.42 s radius, its edge anti-aliased (by the squared distance: no sqrt)
static uint8_t dot_alpha (int s, int i, int j)
{
	if (s == 2) return (uint8_t) (255 >> (i + j));
	int dx = 2 * i + 1 - s, dy = 2 * j + 1 - s, d2 = dx * dx + dy * dy;	// in half pixels
	int r = s * 84, lo = (r - 100) * (r - 100) / 10000, hi = (r + 100) * (r + 100) / 10000;	// the radius x 100, +- half a pixel
	if (d2 <= lo) return 255;
	if (d2 >= hi) return 0;
	return (uint8_t) (255 * (hi - d2) / (hi - lo));
}

static bool dot_dsc (const lv_font_t *font, lv_font_glyph_dsc_t *g, uint32_t cp, uint32_t next)
{
	(void) next;
	const dotlv_t *d = (const dotlv_t *) font->dsc;
	if (cp < 0x20) return false;
	const dotglyph_t *gl = dotfont_find (d->f, cp);
	if (!gl) gl = dotfont_find (d->f, '?');
	int w = gl->w + d->bold;
	g->adv_w = (uint16_t) ((w + 1) * d->s);
	g->box_w = (uint16_t) (w * d->s);
	g->box_h = (uint16_t) (d->f->h * d->s);
	g->ofs_x = 0;
	g->ofs_y = 0;
	g->stride = 0;
	g->format = LV_FONT_GLYPH_FORMAT_A8;			// dot_bitmap writes A8 (0 / 255)
	g->is_placeholder = 0;
	g->gid.src = gl;
	g->entry = NULL;
	if (cp == ' ') g->box_w = 0;				// nothing to draw
	return true;
}

static const void *dot_bitmap (lv_font_glyph_dsc_t *g, lv_draw_buf_t *buf)
{
	const dotlv_t *d = (const dotlv_t *) g->resolved_font->dsc;
	const dotglyph_t *gl = (const dotglyph_t *) g->gid.src;
	if (!gl || !buf) return NULL;
	uint8_t *out = buf->data;
	uint32_t stride = buf->header.stride;
	int s = d->s, w = gl->w + d->bold;
	for (int r = 0; r < d->f->h; r++)
	{
		unsigned bits = gl->row[r];
		if (d->bold) bits |= bits << 1;			// each lit column twice: one column wider
		for (int j = 0; j < s; j++)
		{
			uint8_t *o = out + (uint32_t) (r * s + j) * stride;
			for (int k = 0; k < w; k++)
			{
				uint8_t v = (bits >> (w - 1 - k) & 1) ? 0xFF : 0x00;
				if (d->round && v) for (int i = 0; i < s; i++) o[k * s + i] = dot_alpha (s, i, j);
				else for (int i = 0; i < s; i++) o[k * s + i] = v;
			}
		}
	}
	return buf;						// LVGL 9.4 takes the draw buffer itself
}

// line height: the face's rows and one dot above and below; the base line one dot up. On the 2x2-pixel grid the faces
// keep the 3x3 grid's physical sizes: micro 5x7 (14 px, was 3x5 at 15 px), body and title 5x9 (18 px, was 4x6 at 18 px
// and 5x7 at 21 px), big 5x9 bold at 2x (36 px)
static const dotlv_t kMicroDsc = {&dotfont_5x7, 1, 0}, kTextDsc = {&dotfont_5x9, 1, 0}, kBigDsc = {&dotfont_5x9, 2, 1};
static const lv_font_t kDotMicro = {.get_glyph_dsc = dot_dsc, .get_glyph_bitmap = dot_bitmap, .line_height = 9, .base_line = 1, .dsc = &kMicroDsc};
static const lv_font_t kDotText = {.get_glyph_dsc = dot_dsc, .get_glyph_bitmap = dot_bitmap, .line_height = 11, .base_line = 1, .dsc = &kTextDsc};
static const lv_font_t kDotBig = {.get_glyph_dsc = dot_dsc, .get_glyph_bitmap = dot_bitmap, .line_height = 20, .base_line = 2, .dsc = &kBigDsc};
// 1280x720: micro the 5x9 face (kDotText), body and title 6x11, big 6x11 bold at 2x (docs/UI-THEMES.md "Sizes")
static const dotlv_t kText11Dsc = {&dotfont_6x11, 1, 0}, kBig11Dsc = {&dotfont_6x11, 2, 1};
static const lv_font_t kDotText11 = {.get_glyph_dsc = dot_dsc, .get_glyph_bitmap = dot_bitmap, .line_height = 13, .base_line = 1, .dsc = &kText11Dsc};
static const lv_font_t kDotBig11 = {.get_glyph_dsc = dot_dsc, .get_glyph_bitmap = dot_bitmap, .line_height = 24, .base_line = 2, .dsc = &kBig11Dsc};

// ---- the themes

#define OLED_TEAL	0x5ABCE6	// the FUNC key #2B5A6E at full brightness (x2.1, the hue kept)
#define OLED_TEAL_HI	0x9CD7F0	// its light tint: selection, attention

static const sidecar_theme_t kOled = {
	.id = THEME_OLED, .name = "OLED", .dot = SIDECAR_DOT, .w = 400, .h = 240, .bw = 400, .bh = 240,
	// the rows outside the scale an even dark grey, not DOT_SHADE's checkerboard (it read as noise behind the notes)
	// the greys two steps lighter than the first cut (the 4x6 body face is thinner, and the panel loses the dark greys
	// at a slight angle: the grid and faint text must still read off-axis)
	.bg = 0x000000, .ink = 0xF0F0F0, .dim = 0xBEBEBE, .faint = 0x7C7C7C, .sub = 0xBEBEBE,
	.win = 0x000000, .win_edge = DOT_DOTTED, .win_ink = 0xF0F0F0, .win_dim = 0xBEBEBE,
	.title_bg = DOT_DITHER, .title_ink = 0x000000, .sel_bg = 0xE0E0E0, .sel_ink = 0x000000,
	// the Rytm MKII's panel (rytm-emu src/gui/hw.rs: the FUNC key and sub-label print), lit as dots: the FUNC key's
	// teal #2B5A6E scaled to full brightness is the one accent, a light tint of it the selection; the panel's blue print
	// #2C5C9E brightened is the secondary (single notes, FX 1). No red, no amber: states differ by brightness, dither, outline
	.accent = OLED_TEAL, .accent_hi = OLED_TEAL_HI,
	.ok = OLED_TEAL, .bad = OLED_TEAL_HI, .warn = OLED_TEAL_HI,
	.play = 0xFFFFFF, .lock = 0xD8D8D8,
	.row_in = 0x000000, .row_out = 0x262626, .row_root = 0x1C3A48, .line_step = 0x383838, .line_beat = 0x808080,
	.line_page = 0xC4C4C4, .key_white = 0xA0A0A0, .key_black = 0x505050, .muted = 0x909090,
	// 5x9 the body and titles, 5x7 the micro face (the sizes of the 3x3 grid's 4x6 and 3x5)
	.f_micro = &kDotMicro, .f_small = &kDotText, .f_body = &kDotText, .f_big = &kDotBig, .f_title = &kDotText,
	.f_mid = &kDotText, .f_mid2 = &kDotText,
	.radius = 0, .gap = 3, .touch = 21, .edge_w = 1,
};

// SIDECAR's faces: Space Grotesk Medium (pi/ui/fonts/gen.sh, SIL OFL 1.1) for text, the dot font in round dots for the
// numbers (a knob's value at 2x, the header's page name and HOME's pattern at 3x)
extern const lv_font_t sidecar_sg12, sidecar_sg14, sidecar_sg16, sidecar_sg18, sidecar_sg20, sidecar_sg22, sidecar_sg24, sidecar_sg28;
static const dotlv_t kNumDsc = {&dotfont_5x7, 2, 0, 1}, kNum11Dsc = {&dotfont_5x9, 2, 1, 1};
static const dotlv_t kHeroDsc = {&dotfont_5x9, 3, 0, 1}, kHero11Dsc = {&dotfont_6x11, 3, 0, 1};
static const lv_font_t kSdNum = {.get_glyph_dsc = dot_dsc, .get_glyph_bitmap = dot_bitmap, .line_height = 16, .base_line = 1, .dsc = &kNumDsc};
static const lv_font_t kSdNum11 = {.get_glyph_dsc = dot_dsc, .get_glyph_bitmap = dot_bitmap, .line_height = 20, .base_line = 1, .dsc = &kNum11Dsc};
static const lv_font_t kSdHero = {.get_glyph_dsc = dot_dsc, .get_glyph_bitmap = dot_bitmap, .line_height = 30, .base_line = 2, .dsc = &kHeroDsc};
static const lv_font_t kSdHero11 = {.get_glyph_dsc = dot_dsc, .get_glyph_bitmap = dot_bitmap, .line_height = 36, .base_line = 2, .dsc = &kHero11Dsc};

// the "UV night" palette (the web app's, docs/UI-THEMES.md "SIDECAR"): text >= 4.5:1, controls >= 3:1 on bg and cards
#define UVN_BG		0x08071A	// the screen
#define UVN_CARD	0x110F28	// a card (panel, window)
#define UVN_BORD	0x2C2950	// a card's border, a track, an empty meter
#define UVN_CTRL	0x6A64A0	// a control's border (3.5:1 on a card)
#define UVN_TXT		0xEEEDFF
#define UVN_LBL		0xBDB9E8	// labels, secondary text
#define UVN_FAINT	0x8A85C0	// disabled (5.5:1 all the same)
#define UVN_ACC		0x9A97FF	// the accent: selection, values, the A side
#define UVN_COR		0xFF7A5C	// coral: the B side, FM carriers, recording, the selected note, warnings
#define UVN_MAG		0xD46CFF	// magenta: the crossfader's middle, a hot meter
#define UVN_RAIL	0x0D0A2A	// the T1..T8 rail

// The painters read the palette through UV_*: the standard one above, or the high-contrast one (sidecar_theme_set picks;
// docs/UI-THEMES.md "High contrast"). trk: an empty track / meter; cor2, acc2: the light tints of coral and the accent.
static struct { uint32_t bg, card, bord, ctrl, txt, lbl, faint, acc, cor, mag, rail, trk, cor2, acc2; int hc; } UV =
	{UVN_BG, UVN_CARD, UVN_BORD, UVN_CTRL, UVN_TXT, UVN_LBL, UVN_FAINT, UVN_ACC, UVN_COR, UVN_MAG, UVN_RAIL, UVN_BORD, 0xFFB3A1, 0xC9C7FF, 0};
#define UV_BG	UV.bg
#define UV_CARD	UV.card
#define UV_BORD	UV.bord
#define UV_CTRL	UV.ctrl
#define UV_TXT	UV.txt
#define UV_LBL	UV.lbl
#define UV_FAINT UV.faint
#define UV_ACC	UV.acc
#define UV_COR	UV.cor
#define UV_MAG	UV.mag
#define UV_RAIL	UV.rail
#define UV_TRK	UV.trk
#define UV_COR2	UV.cor2
#define UV_ACC2	UV.acc2
#define HC	(UV.hc)

// the high-contrast accents
#define HC_BLUE		0x8FD3FF
#define HC_GREEN	0x80D553
#define HC_YELLOW	0xFFD166

static const sidecar_theme_t kSidecar = {
	.id = THEME_SIDECAR, .name = "SIDECAR", .dot = 1, .w = 800, .h = 480, .bw = 800, .bh = 480,
	.bg = UVN_BG, .ink = UVN_TXT, .dim = UVN_LBL, .faint = UVN_FAINT, .sub = UVN_LBL,
	.win = UVN_CARD, .win_edge = UVN_BORD, .win_ink = UVN_TXT, .win_dim = UVN_LBL,
	.title_bg = 0x14103A, .title_ink = UVN_TXT, .sel_bg = UVN_ACC, .sel_ink = UVN_BG,
	.accent = UVN_ACC, .accent_hi = 0xC9C7FF,
	.ok = UVN_ACC, .bad = UVN_COR, .warn = 0xFFB3A1,
	.play = UVN_TXT, .lock = UVN_MAG,
	// the piano roll: white-key rows lighter than black-key rows, the lines in three steps of the control lavender
	.row_in = 0x120F2C, .row_out = 0x0B0920, .row_root = 0x1E1A48, .line_step = 0x1D1A3D, .line_beat = 0x3A3566,
	.line_page = UVN_CTRL, .key_white = 0x7A779A, .key_black = UVN_BORD, .muted = 0x5A548C,
	.f_micro = &sidecar_sg12, .f_small = &sidecar_sg14, .f_body = &sidecar_sg16,
	.f_big = &sidecar_sg24, .f_title = &sidecar_sg20,
	.f_mid = &sidecar_sg18, .f_mid2 = &sidecar_sg22,
	.f_num = &kSdNum, .f_hero = &kSdHero,
	.radius = 12, .gap = 6, .touch = 46, .edge_w = 1,
};

// SQUARE and GUIDE (docs/UI-THEMES.md): the style guide's palette. The guide names eight colours; the warm roles it has no
// token for (warning, error, recording, p-lock) take two muted ones, and the disabled grey is a mix of two guide colours.
#define SQ_CHARCOAL	0x171916	// the screen and every panel
#define SQ_GREEN	0x31BD3E	// signal green: the active and the selected, status
#define SQ_SAGE		0x8DC494	// notes, the logo, a track's machine
#define SQ_CHALK	0xECEFE8	// primary text, the play head
#define SQ_MUTED	0xB0BEAE	// secondary text, labels
#define SQ_OLIVE	0x4B5449	// panel and control borders
#define SQ_DEEP		0x2B342B	// the piano roll's grid, an empty track or meter
#define SQ_INK		0x0F120E	// text on green
#define SQ_AMBER	0xE8C25A	// extra: warning, held, p-lock, recording, a hot level (the guide's sketch shows it on REC and MISSING)
#define SQ_BRICK	0xD9674F	// extra: an error, delete, kill
#define SQ_FAINT	0x7E8A7C	// derived: disabled text (olive and muted sage, half and half)

// SQUARE and GUIDE keep the text in capitals by a font of their own over the Arimo tables (sq_font): the pages' strings stay
// as they are written. Sizes: 11 supporting, 13 controls, 18 primary, 28 SQUARE's hero numbers.
extern const lv_font_t sidecar_ar11, sidecar_ar13, sidecar_ar18, sidecar_ar28;
static lv_font_t s_Up[4];
#define UP11 (&s_Up[0])
#define UP13 (&s_Up[1])
#define UP18 (&s_Up[2])
#define UP28 (&s_Up[3])

static const sidecar_theme_t kSquare = {
	.id = THEME_SQUARE, .name = "SQUARE", .dot = 1, .w = 800, .h = 480, .bw = 800, .bh = 480,
	.bg = SQ_CHARCOAL, .ink = SQ_CHALK, .dim = SQ_MUTED, .faint = SQ_FAINT, .sub = SQ_MUTED,
	.win = SQ_CHARCOAL, .win_edge = SQ_OLIVE, .win_ink = SQ_CHALK, .win_dim = SQ_MUTED,
	.title_bg = SQ_CHARCOAL, .title_ink = SQ_CHALK, .sel_bg = SQ_GREEN, .sel_ink = SQ_INK,
	.accent = SQ_GREEN, .accent_hi = SQ_SAGE,
	.ok = SQ_GREEN, .bad = SQ_BRICK, .warn = SQ_AMBER,
	.play = SQ_CHALK, .lock = SQ_AMBER,
	// the piano roll: white-key rows a step above the screen, black-key rows a step below, the lines in three steps of olive
	.row_in = 0x1C201B, .row_out = 0x121411, .row_root = 0x232C24, .line_step = SQ_DEEP, .line_beat = 0x3A4439,
	.line_page = SQ_OLIVE, .key_white = SQ_DEEP, .key_black = 0x1F251F, .muted = 0x6B7868,
	.f_micro = UP11, .f_small = UP13, .f_body = UP13, .f_big = UP18, .f_title = UP18,
	.f_mid = UP18, .f_mid2 = UP18,
	.f_num = UP18, .f_hero = UP28,
	.radius = 0, .gap = 8, .touch = 44, .edge_w = 1,
};

// GUIDE: the same palette, only 11 / 13 / 18 px (the hero numbers 18 too)
static const sidecar_theme_t kGuide = {
	.id = THEME_GUIDE, .name = "GUIDE", .dot = 1, .w = 800, .h = 480, .bw = 800, .bh = 480,
	.bg = SQ_CHARCOAL, .ink = SQ_CHALK, .dim = SQ_MUTED, .faint = SQ_FAINT, .sub = SQ_MUTED,
	.win = SQ_CHARCOAL, .win_edge = SQ_OLIVE, .win_ink = SQ_CHALK, .win_dim = SQ_MUTED,
	.title_bg = SQ_CHARCOAL, .title_ink = SQ_CHALK, .sel_bg = SQ_GREEN, .sel_ink = SQ_INK,
	.accent = SQ_GREEN, .accent_hi = SQ_SAGE,
	.ok = SQ_GREEN, .bad = SQ_BRICK, .warn = SQ_AMBER,
	.play = SQ_CHALK, .lock = SQ_AMBER,
	.row_in = 0x1C201B, .row_out = 0x121411, .row_root = 0x232C24, .line_step = SQ_DEEP, .line_beat = 0x3A4439,
	.line_page = SQ_OLIVE, .key_white = SQ_DEEP, .key_black = 0x1F251F, .muted = 0x6B7868,
	.f_micro = UP11, .f_small = UP13, .f_body = UP13, .f_big = UP18, .f_title = UP18,
	.f_mid = UP18, .f_mid2 = UP18,
	.f_num = UP18, .f_hero = UP18,
	.radius = 0, .gap = 8, .touch = 44, .edge_w = 1,
};

const sidecar_theme_t *g_Theme = &kOled;
th_fit_fn th_fit_report;
static sidecar_theme_t s_Cur;				// the table now: the theme's, sized for the screen
static int s_bWide, s_bBig, s_nContrast;

const sidecar_theme_t *sidecar_theme_get (int id)
{
	return id == THEME_SIDECAR ? &kSidecar : id == THEME_SQUARE ? &kSquare : id == THEME_GUIDE ? &kGuide : &kOled;
}
void sidecar_theme_screen (int pw, int ph) { s_bWide = pw >= 1024 && ph >= 600; s_bBig = pw >= 1280 && ph >= 720; }
int sidecar_theme_wide (void) { return s_bWide; }
void sidecar_theme_contrast (int hc) { s_nContrast = hc == 1 || hc == 2 ? hc : 0; }
int sidecar_theme_get_contrast (void) { return s_nContrast; }

// the high-contrast SIDECAR (the web app's data-theme="contrast"): black, white 2 px edges, one accent, yellow for the
// warm roles; the same ids, fonts, sizes and layouts as SIDECAR, so the pages need no branch of their own
static void contrast_apply (sidecar_theme_t *t, int hc)
{
	const uint32_t acc = hc == 2 ? HC_GREEN : HC_BLUE;
	UV.bg = UV.card = UV.rail = 0x000000;
	UV.bord = UV.ctrl = UV.txt = 0xFFFFFF;
	UV.lbl = 0xE6E6E6;
	UV.faint = 0xA6A6A6;
	UV.acc = UV.acc2 = acc;
	UV.cor = UV.mag = UV.cor2 = HC_YELLOW;
	UV.trk = 0x767676;
	UV.hc = hc;
	t->hc = hc;
	t->edge_w = 2;
	t->bg = 0x000000; t->ink = 0xFFFFFF; t->dim = 0xE6E6E6; t->faint = 0xA6A6A6; t->sub = 0xE6E6E6;
	t->win = 0x000000; t->win_edge = 0xFFFFFF; t->win_ink = 0xFFFFFF; t->win_dim = 0xE6E6E6;
	t->title_bg = 0x000000; t->title_ink = 0xFFFFFF; t->sel_bg = acc; t->sel_ink = 0x000000;
	t->accent = t->accent_hi = t->ok = acc;
	t->bad = t->warn = t->lock = HC_YELLOW;
	t->play = 0xFFFFFF;
	t->row_in = 0x000000; t->row_out = 0x161616; t->row_root = th_mix (0x000000, acc, 41);
	t->line_step = 0x333333; t->line_beat = 0x777777; t->line_page = 0xFFFFFF;
	t->key_white = 0xE6E6E6; t->key_black = 0x000000; t->muted = 0xA6A6A6;
}

// the capitals font: the Arimo table's glyphs, a-z asked for as A-Z (the table is the base, kept in dsc)
static bool up_dsc (const lv_font_t *font, lv_font_glyph_dsc_t *g, uint32_t cp, uint32_t next)
{
	const lv_font_t *b = (const lv_font_t *) font->dsc;
	if (cp >= 'a' && cp <= 'z') cp -= 32;
	if (next >= 'a' && next <= 'z') next -= 32;
	return b->get_glyph_dsc (b, g, cp, next);
}

static const void *up_bitmap (lv_font_glyph_dsc_t *g, lv_draw_buf_t *buf)
{
	const lv_font_t *w = g->resolved_font, *b = (const lv_font_t *) w->dsc;
	g->resolved_font = b;					// the table's own bitmap reader finds its glyphs through it
	const void *r = b->get_glyph_bitmap (g, buf);
	g->resolved_font = w;
	return r;
}

static void up_init (void)
{
	static const lv_font_t *const kBase[4] = {&sidecar_ar11, &sidecar_ar13, &sidecar_ar18, &sidecar_ar28};
	for (int i = 0; i < 4; i++)
	{
		s_Up[i] = *kBase[i];
		s_Up[i].get_glyph_dsc = up_dsc;
		s_Up[i].get_glyph_bitmap = up_bitmap;
		s_Up[i].static_bitmap = 0;
		s_Up[i].dsc = kBase[i];
	}
}

// SQUARE and GUIDE: the painters' palette (UV_*) in the guide's colours. cor / mag / cor2 (the warm hues of SIDECAR's
// painters: a level, a hot meter, a held note) are the one amber; an error and a delete key use TH->bad, the brick
static void square_apply (void)
{
	UV.bg = UV.card = UV.rail = SQ_CHARCOAL;
	UV.bord = UV.ctrl = SQ_OLIVE;
	UV.txt = SQ_CHALK;
	UV.lbl = SQ_MUTED;
	UV.faint = SQ_FAINT;
	UV.acc = SQ_GREEN;
	UV.acc2 = SQ_SAGE;
	UV.cor = UV.mag = UV.cor2 = SQ_AMBER;
	UV.trk = SQ_DEEP;
}

void sidecar_theme_set (int id)
{
	if (!s_Up[0].get_glyph_dsc) up_init ();
	s_Cur = *sidecar_theme_get (id);
	UV.bg = UVN_BG; UV.card = UVN_CARD; UV.bord = UVN_BORD; UV.ctrl = UVN_CTRL; UV.txt = UVN_TXT; UV.lbl = UVN_LBL;
	UV.faint = UVN_FAINT; UV.acc = UVN_ACC; UV.cor = UVN_COR; UV.mag = UVN_MAG; UV.rail = UVN_RAIL; UV.trk = UVN_BORD;
	UV.cor2 = 0xFFB3A1; UV.acc2 = 0xC9C7FF; UV.hc = 0;
	if (id == THEME_SIDECAR && s_nContrast) contrast_apply (&s_Cur, s_nContrast);
	if (THEME_IS_SQ (id)) square_apply ();
	if (s_bWide)						// 1024x600: the dots / pixels it has
	{
		s_Cur.w = (s_bBig ? 1280 : 1024) / s_Cur.dot;
		s_Cur.h = (s_bBig ? 720 : 600) / s_Cur.dot;
	}
	if (s_bBig)						// 1280x720: the base layout in LU, the faces and keys with it
	{
		s_Cur.big = 1;
		s_Cur.bw = s_Cur.bw * 5 / 4;
		s_Cur.bh = s_Cur.bh * 5 / 4;
		s_Cur.touch = s_Cur.touch * 5 / 4;
		if (!THEME_IS_SQ (id)) s_Cur.gap = s_Cur.gap + 1;
		if (THEME_IS_SQ (id)) {}			// the sizes are physical: 11 / 13 / 18 (28) px at 1280x720 as well
		else if (id == THEME_OLED)
		{
			s_Cur.f_micro = &kDotText;
			s_Cur.f_small = s_Cur.f_body = s_Cur.f_title = s_Cur.f_mid = s_Cur.f_mid2 = &kDotText11;
			s_Cur.f_big = &kDotBig11;
		}
		else
		{
			s_Cur.f_micro = &sidecar_sg16;
			s_Cur.f_small = &sidecar_sg18;
			s_Cur.f_body = &sidecar_sg20;
			s_Cur.f_mid = &sidecar_sg22;
			s_Cur.f_title = s_Cur.f_mid2 = &sidecar_sg24;
			s_Cur.f_big = &sidecar_sg28;
			s_Cur.f_num = &kSdNum11;
			s_Cur.f_hero = &kSdHero11;
			s_Cur.radius = 15;
		}
	}
	// the navigation: OLED's column is part of its base layout; SIDECAR's rail is added right of the 800x480 one when the
	// screen has room (1024x600: 96 px, 1280x720: 120), at 800x480 the keys are a row in the header
	const int lu = s_Cur.big ? 5 : 4;			// LU () of the table being set (TH is still the old one)
	if (id == THEME_OLED) s_Cur.nav = 48 * lu / 4;
	else s_Cur.nav = s_bWide ? 96 * lu / 4 : 0;
	if (id == THEME_OLED) s_Cur.f_num = s_Cur.f_hero = s_Cur.f_big;
	s_Cur.dx = s_Cur.w - s_Cur.bw - (id == THEME_OLED ? 0 : s_Cur.nav);
	s_Cur.dy = s_Cur.h - s_Cur.bh;
	g_Theme = &s_Cur;
}

lv_color_t th_c (uint32_t rgb) { return lv_color_hex (rgb); }
// an LED colour: OLED has no LED hues - white (the palette's white ramp: playing, REC's blink), the panel teal (green:
// connected, OK), its light tint (anything else). SIDECAR: the palette's ramps in its own hues, as dim or bright as the
// palette's step (red coral, green and blue the accent, amber the warm tint, cyan the light accent, magenta, white)
uint32_t th_led (int c)
{
	if (TH->id != THEME_OLED)
	{
		const uint32_t kHue[7] = {THEME_IS_SQ (TH->id) ? SQ_BRICK : UV_COR, UV_ACC, UV_ACC, UV_COR2, UV_ACC2, UV_MAG, UV_TXT};
		if (c < 0 || c >= 112 || c % 16 == 0) return UV_TRK;
		int i = c % 16;
		return th_mix (UV_TRK, kHue[c / 16], 70 + 186 * i * i / 225);
	}
	if (c >= LED_WHITE) return 0xFFFFFF;
	if (c >= LED_GREEN && c < LED_BLUE) return TH->accent;
	return TH->accent_hi;
}
uint32_t th_oled_secondary (int vel)	// the panel's blue print, brighter the louder (1..127)
{
	return th_mix (0x162E50, 0x4691FA, 60 + (vel < 1 ? 1 : vel > 127 ? 127 : vel) * 196 / 127);
}
uint32_t th_oled_accent (int vel)	// the FUNC teal, brighter the louder
{
	return th_mix (0x163040, OLED_TEAL, 60 + (vel < 1 ? 1 : vel > 127 ? 127 : vel) * 196 / 127);
}

uint32_t th_mix (uint32_t a, uint32_t b, int t)
{
	uint32_t o = 0;
	for (int s = 16; s >= 0; s -= 8)
	{
		int x = (int) (a >> s & 255), y = (int) (b >> s & 255);
		o |= (uint32_t) (x + (y - x) * t / 256) << s;
	}
	return o;
}

static int oled (void) { return TH->id == THEME_OLED; }
static int sq (void) { return THEME_IS_SQ (TH->id); }	// SQUARE or GUIDE: sharp corners, the guide's palette
static int guide (void) { return TH->id == THEME_GUIDE; }	// GUIDE: panels and selector keys share their edges

// ---- painters

void th_fill (lv_layer_t *l, const lv_area_t *c, int x1, int y1, int x2, int y2, uint32_t rgb, lv_opa_t opa, int r)
{
	lv_area_t a = {c->x1 + x1, c->y1 + y1, c->x1 + x2, c->y1 + y2}, k;
	if (x2 < x1 || y2 < y1 || !lv_area_intersect (&k, &a, &l->_clip_area)) return;
	if (sq () && r != LV_RADIUS_CIRCLE) r = 0;			// SQUARE, GUIDE: no rounded corner (a circle stays a circle)
	lv_draw_fill_dsc_t d;
	lv_draw_fill_dsc_init (&d);
	d.color = lv_color_hex (rgb);
	d.opa = opa;
	d.radius = r;
	lv_draw_fill (l, &d, &a);
}

// a horizontal gradient a .. b over x1..x2 (SIDECAR's header band; two stops: LV_GRADIENT_MAX_STOPS)
static void grad_h (lv_layer_t *l, const lv_area_t *c, int x1, int y1, int x2, int y2, uint32_t a, uint32_t b)
{
	lv_area_t r = {c->x1 + x1, c->y1 + y1, c->x1 + x2, c->y1 + y2}, k;
	if (x2 < x1 || y2 < y1 || !lv_area_intersect (&k, &r, &l->_clip_area)) return;
	lv_draw_fill_dsc_t d;
	lv_draw_fill_dsc_init (&d);
	d.opa = LV_OPA_COVER;
	d.grad.dir = LV_GRAD_DIR_HOR;
	d.grad.stops_count = 2;
	d.grad.stops[0].color = lv_color_hex (a);
	d.grad.stops[0].opa = LV_OPA_COVER;
	d.grad.stops[0].frac = 0;
	d.grad.stops[1].color = lv_color_hex (b);
	d.grad.stops[1].opa = LV_OPA_COVER;
	d.grad.stops[1].frac = 255;
	lv_draw_fill (l, &d, &r);
}

void th_frame (lv_layer_t *l, const lv_area_t *c, int x1, int y1, int x2, int y2, uint32_t rgb, int w, int r)
{
	if (oled () || r == 0 || (sq () && r != LV_RADIUS_CIRCLE))	// square: four fills (crisp, and DOT_* colours stay exact)
	{
		th_fill (l, c, x1, y1, x2, y1 + w - 1, rgb, LV_OPA_COVER, 0);
		th_fill (l, c, x1, y2 - w + 1, x2, y2, rgb, LV_OPA_COVER, 0);
		th_fill (l, c, x1, y1 + w, x1 + w - 1, y2 - w, rgb, LV_OPA_COVER, 0);
		th_fill (l, c, x2 - w + 1, y1 + w, x2, y2 - w, rgb, LV_OPA_COVER, 0);
		return;
	}
	lv_area_t a = {c->x1 + x1, c->y1 + y1, c->x1 + x2, c->y1 + y2}, k;
	if (!lv_area_intersect (&k, &a, &l->_clip_area)) return;
	lv_draw_border_dsc_t d;
	lv_draw_border_dsc_init (&d);
	d.color = lv_color_hex (rgb);
	d.width = w;
	d.radius = r;
	d.opa = LV_OPA_COVER;
	lv_draw_border (l, &d, &a);
}

static int text_w_ls (const lv_font_t *f, const char *t, int ls)
{
	lv_text_attributes_t a;
	lv_text_attributes_init (&a);
	a.letter_space = ls;
	return lv_text_get_width (t, (uint32_t) strlen (t), f, &a);
}

// th_text with a letter spacing (SQUARE, GUIDE: 1 px on section labels, 2 px on the wordmark)
static void text_ls (lv_layer_t *l, const lv_area_t *c, int x, int y, int w, const lv_font_t *f, uint32_t rgb, lv_text_align_t al,
		     const char *t, int ls)
{
	lv_area_t a = {c->x1 + x, c->y1 + y, c->x1 + x + w - 1, c->y1 + y + f->line_height - 1}, k;
	if (!t || !t[0] || !lv_area_intersect (&k, &a, &l->_clip_area)) return;
	if (th_fit_report)
	{
		int need = text_w_ls (f, t, ls), ow = lv_area_get_width (c);
		int x0 = al == LV_TEXT_ALIGN_CENTER ? x + (w - need) / 2 : al == LV_TEXT_ALIGN_RIGHT ? x + w - need : x;
		if (need > w) th_fit_report (t, need, w);
		else if (x0 < 0 || x0 + need > ow) th_fit_report (t, need, x0 < 0 ? need + x0 : ow - x0);	// past its object
	}
	lv_draw_label_dsc_t d;
	lv_draw_label_dsc_init (&d);
	d.text = t;
	d.font = f;
	d.color = lv_color_hex (rgb);
	d.align = al;
	d.letter_space = ls;
	d.text_local = 1;
	d.flag = LV_TEXT_FLAG_EXPAND;
	lv_draw_label (l, &d, &a);
}

void th_text (lv_layer_t *l, const lv_area_t *c, int x, int y, int w, const lv_font_t *f, uint32_t rgb, lv_text_align_t al,
	      const char *t)
{
	text_ls (l, c, x, y, w, f, rgb, al, t, 0);
}

int th_text_width (const lv_font_t *f, const char *t)
{
	lv_text_attributes_t a;
	lv_text_attributes_init (&a);
	return lv_text_get_width (t, (uint32_t) strlen (t), f, &a);
}

// a disc of dots (no anti-aliasing): rows of fills
static void disc (lv_layer_t *l, const lv_area_t *c, int cx, int cy, int r, uint32_t rgb)
{
	for (int dy = -r; dy <= r; dy++)
	{
		int dx = 0;
		while ((dx + 1) * (dx + 1) + dy * dy <= r * r + r) dx++;
		th_fill (l, c, cx - dx, cy + dy, cx + dx, cy + dy, rgb, LV_OPA_COVER, 0);
	}
}

static void circle (lv_layer_t *l, const lv_area_t *c, int cx, int cy, int r, uint32_t rgb, lv_opa_t opa)
{
	th_fill (l, c, cx - r, cy - r, cx + r, cy + r, rgb, opa, LV_RADIUS_CIRCLE);
}

// an arc of width w (rounded ends) at radius r round cx, cy: LVGL's angles, 0 at 3 o'clock, clockwise (SIDECAR's knobs)
static void arc (lv_layer_t *l, const lv_area_t *c, int cx, int cy, int r, int w, int a0, int a1, uint32_t rgb)
{
	lv_draw_arc_dsc_t d;
	lv_draw_arc_dsc_init (&d);
	d.color = lv_color_hex (rgb);
	d.width = w;
	d.center.x = c->x1 + cx;
	d.center.y = c->y1 + cy;
	d.radius = (uint16_t) (r + w / 2);
	d.start_angle = a0 % 360;
	d.end_angle = a1 % 360;
	d.rounded = !sq ();					// (SQUARE, GUIDE: square ends)
	d.opa = LV_OPA_COVER;
	lv_draw_arc (l, &d);
}

// sin / cos x 1024 of 0..359 degrees, from a quarter table
static int isin (int deg)
{
	static const short q[91] = {0, 18, 36, 54, 71, 89, 107, 125, 143, 160, 178, 195, 213, 230, 248, 265, 282, 299, 316, 333,
		350, 367, 384, 400, 416, 433, 449, 465, 481, 496, 512, 527, 543, 558, 573, 587, 602, 616, 630, 644, 658, 672, 685,
		698, 711, 724, 737, 749, 761, 773, 784, 796, 807, 818, 828, 839, 849, 859, 868, 878, 887, 896, 904, 912, 920, 928,
		935, 943, 949, 956, 962, 968, 974, 979, 984, 989, 994, 998, 1002, 1005, 1008, 1011, 1014, 1016, 1018, 1020, 1022,
		1023, 1023, 1024, 1024};
	deg = ((deg % 360) + 360) % 360;
	if (deg <= 90) return q[deg];
	if (deg <= 180) return q[180 - deg];
	if (deg <= 270) return -q[deg - 180];
	return -q[360 - deg];
}
static int icos (int deg) { return isin (deg + 90); }

void th_paint_knob (lv_layer_t *l, const lv_area_t *a, int v, int max, uint32_t rgb)
{
	lv_area_t c = *a;
	int w = lv_area_get_width (a), h = lv_area_get_height (a);
	int r = (w < h ? w : h) / 2 - 1, cx = w / 2, cy = h / 2;
	if (max <= 0) max = 1;
	int known = v >= 0;
	if (v < 0) v = 0;
	if (v > max) v = max;
	int ang = 225 - 270 * v / max;				// 225 (min, lower left) .. -45 (max, lower right), screen y down
	if (oled ())
	{
		const int n = TH->big && r >= 20 ? 25 : r >= 14 ? 19 : 13;			// the finer grid: more dots on a big ring
		int lit = known ? (v * (n - 1) + max / 2) / max : -1;
		for (int i = 0; i < n; i++)		// the scale: a ring of dots, lit up to the value
		{
			int d = 225 - 270 * i / (n - 1);
			int x = cx + (icos (d) * r + (icos (d) > 0 ? 512 : -512)) / 1024, y = cy - (isin (d) * r + (isin (d) > 0 ? 512 : -512)) / 1024;
			th_fill (l, &c, x, y, x, y, i <= lit ? rgb : TH->faint, LV_OPA_COVER, 0);
		}
		// UX review T7: a hollow cap (an outline of dots, not the page's brightest mass) and the pointer as a line of
		// ink dots from the centre to the cap's edge; a value not known yet: the outline faint, no pointer
		int rb = r - (r >= 14 ? 4 : 3), nd = rb >= 10 ? 28 : 20;
		for (int i = 0; i < nd; i++)
		{
			int d = 360 * i / nd;
			int x = cx + (icos (d) * rb + (icos (d) > 0 ? 512 : -512)) / 1024, y = cy - (isin (d) * rb + (isin (d) > 0 ? 512 : -512)) / 1024;
			th_fill (l, &c, x, y, x, y, known ? 0x8C8C8C : 0x5A5A5A, LV_OPA_COVER, 0);
		}
		if (!known) return;
		for (int k = 0; k <= rb - 1; k++)
		{
			int x = cx + (icos (ang) * k + (icos (ang) > 0 ? 512 : -512)) / 1024;
			int y = cy - (isin (ang) * k + (isin (ang) > 0 ? 512 : -512)) / 1024;
			th_fill (l, &c, x, y, x, y, TH->ink, LV_OPA_COVER, 0);
		}
		return;
	}
	// SQUARE, GUIDE: the same arc with square ends and a short chalk tick across it at the value (the guide's knobs)
	if (sq ())
	{
		const int aw = r >= LU (30) ? LU (6) : LU (4), rr = r - aw / 2 - 1;
		arc (l, &c, cx, cy, rr, aw, 135, 405, UV_TRK);
		if (!known) return;
		int e = 135 + 270 * v / max;
		if (e > 136) arc (l, &c, cx, cy, rr, aw, 135, e, rgb);
		lv_draw_line_dsc_t d;
		lv_draw_line_dsc_init (&d);
		d.color = lv_color_hex (UV_TXT);
		d.width = 2;
		d.opa = LV_OPA_COVER;
		const int r0 = rr - aw / 2 - 2, r1 = rr + aw / 2 + 1;
		d.p1.x = c.x1 + cx + icos (e) * r0 / 1024;
		d.p1.y = c.y1 + cy + isin (e) * r0 / 1024;
		d.p2.x = c.x1 + cx + icos (e) * r1 / 1024;
		d.p2.y = c.y1 + cy + isin (e) * r1 / 1024;
		lv_draw_line (l, &d);
		return;
	}
	// SIDECAR: a 270 degree arc, the track in the border's colour, the value in the knob's colour (the accent; coral for a
	// level) from the minimum, a light dot at the value; not known yet: the track alone (the value inside: meter_draw)
	const int aw = r >= LU (30) ? LU (6) : LU (4), rr = r - aw / 2 - 1;
	arc (l, &c, cx, cy, rr, aw, 135, 405, UV_TRK);
	if (!known) return;
	int e = 135 + 270 * v / max;
	if (e > 136) arc (l, &c, cx, cy, rr, aw, 135, e, rgb);
	int px = cx + icos (360 - e) * rr / 1024, py = cy - isin (360 - e) * rr / 1024;
	if (HC) circle (l, &c, px, py, aw * 3 / 4 + 3, 0x000000, LV_OPA_COVER);	// the dot: white with a black edge
	circle (l, &c, px, py, aw * 3 / 4 + 1, UV_TXT, LV_OPA_COVER);
}

void th_paint_logo (lv_layer_t *l, const lv_area_t *c, int x, int y, int size)
{
	const uint32_t kPal[4] = {UV_COR, UV_MAG, UV_ACC, UV_LBL}, kWhite[4] = {0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF};
	const uint32_t kSage[4] = {SQ_SAGE, SQ_SAGE, SQ_SAGE, SQ_SAGE};
	const uint32_t *pal = HC ? kWhite : sq () ? kSage : kPal;	// high contrast: plain white dots; SQUARE, GUIDE: sage dots, the green disc
	for (int i = 0; i < SIDECAR_LOGO_N; i++)
	{
		int cx = x + (kLogo[i].x * size + 230) / 460, cy = y + (kLogo[i].y * size + 230) / 460, r = (kLogo[i].r * size + 2300) / 4600;
		th_fill (l, c, cx - r, cy - r, cx + r, cy + r, pal[kLogo[i].c], LV_OPA_COVER, LV_RADIUS_CIRCLE);
	}
	int cx = x + SIDECAR_LOGO_DISC_X * size / 460, cy = y + SIDECAR_LOGO_DISC_Y * size / 460, r = SIDECAR_LOGO_DISC_R * size / 460;
	th_fill (l, c, cx - r, cy - r, cx + r, cy + r, HC ? 0xFFFFFF : UV_ACC, LV_OPA_COVER, LV_RADIUS_CIRCLE);
}

int th_page_w (void) { return TH->w - TH->nav; }

static int font_line (const lv_font_t *f) { return f->line_height; }

// the navigation column's icons (OLED), 15 x 11 dots, drawn in the key's ink; the key's text picks one and stays as a
// micro label under it
#define ICON_W	15
#define ICON_H	11
static const struct { const char *name; const char *row[ICON_H]; } kIcon[] = {
	{"HOME", {	".......#.......",		// a house, its door
			"......#.#......",
			".....#...#.....",
			"....#.....#....",
			"...#.......#...",
			"..###########..",
			"..#.........#..",
			"..#..###....#..",
			"..#..#.#....#..",
			"..#..#.#....#..",
			"..####.######.."}},
	{"SEQ", {	"##.#...........",		// a piano roll: keys, notes
			"#..............",
			"#....######....",
			"#..............",
			"##.#.......###.",
			"#..............",
			"#..####........",
			"#..............",
			"##.#....#####..",
			"#..............",
			"###############"}},
	{"PAT", {	"###.###.###.###",		// trig keys, some lit
			"###.#.#.###.#.#",
			"###.###.###.###",
			"...............",
			"###.###.###.###",
			"#.#.###.#.#.#.#",
			"###.###.###.###",
			"...............",
			"###.###.###.###",
			"#.#.#.#.###.###",
			"###.###.###.###"}},
	{"SET", {	"...###.........",		// three sliders
			"###############",
			"...###.........",
			"...............",
			".........###...",
			"###############",
			".........###...",
			"...............",
			".....###.......",
			"###############",
			".....###......."}},
	{"VOICE", {	".....#####.....",		// a knob, its pointer
			"...##.....##...",
			"..#.........#..",
			".#.....#.....#.",
			".#.....#.....#.",
			".#.....#.....#.",
			".#...........#.",
			"..#.........#..",
			"...##.....##...",
			".....#####.....",
			"..............."}},
	{"FX", {	"#######........",		// a filter's curve
			".......##......",
			".........#.....",
			"..........#....",
			"...........#...",
			"...........#...",
			"............#..",
			"............#..",
			".............#.",
			".............#.",
			"..............."}},
	{"SONG", {	"###############",		// a timeline: pattern blocks over a ruler
			"#.....#....#..#",
			"#.....#....#..#",
			"###############",
			"...............",
			"..#####........",
			"...............",
			"........######.",
			"...............",
			"#..#..#..#..#..",
			"###############"}},
	{"REC", {	"...............",		// the record dot
			"......###......",
			"....#######....",
			"...#########...",
			"..###########..",
			"..###########..",
			"..###########..",
			"...#########...",
			"....#######....",
			"......###......",
			"..............."}},
	{"BACK", {	".....#.........",		// an arrow round and back
			"....##.........",
			"...##########..",
			"..###########..",
			"...##.......##.",
			"....#........#.",
			".............#.",
			".............#.",
			".............#.",
			"....##########.",
			"..............."}},
};

static int nav_icon (lv_layer_t *l, const lv_area_t *c, const char *name, int cx, int y, uint32_t ink)
{
	for (unsigned i = 0; i < sizeof kIcon / sizeof kIcon[0]; i++)
	{
		if (strcmp (kIcon[i].name, name)) continue;
		const int s = TH->big || !oled () ? 2 : 1;	// 1280x720 and SIDECAR: each dot of the icon two
		for (int r = 0; r < ICON_H; r++)
			for (int k = 0; k < ICON_W; k++)
				if (kIcon[i].row[r][k] == '#') th_fill (l, c, cx + s * (k - ICON_W / 2), y + s * r, cx + s * (k - ICON_W / 2) + s - 1, y + s * r + s - 1, ink, LV_OPA_COVER, 0);
		return 1;
	}
	return 0;
}

// SQUARE and GUIDE: a key is a box (radius 0) with a 1 px olive edge and chalk text; the selected one is filled signal green
// with ink text; the main action has a green edge, DANGER a brick one; a track selector (TH_KEY_ROUND) is a circle. The
// navigation keys carry their T key small at the top right. No tints, no shadows.
static void sq_key (lv_layer_t *l, const lv_area_t *a, const char *text, const char *sub, const lv_font_t *f, int kind,
		    int checked, int pressed, int disabled, int led)
{
	lv_area_t c = *a;
	int w = lv_area_get_width (a), h = lv_area_get_height (a);
	const int k = kind & TH_KEY_KIND, left = kind & TH_KEY_LEFT;
	const int r = kind & TH_KEY_ROUND ? LV_RADIUS_CIRCLE : 0;
	if (r)							// a circle: the key's largest square, centred
	{
		const int d = w < h ? w : h;
		c.x1 += (w - d) / 2; c.y1 += (h - d) / 2;
		c.x2 = c.x1 + d - 1; c.y2 = c.y1 + d - 1;
		w = h = d;
	}
	if (!f) f = kind & TH_KEY_SMALL ? TH->f_micro : k == TH_KEY_NAV ? TH->f_body : TH->f_body;
	const int tabbed = k == TH_KEY_NAV || (kind & TH_KEY_TAB);
	const uint32_t lc = led > 0 && !disabled ? th_led (led) : 0;
	uint32_t edge = UV_CTRL, ink = UV_TXT, sb = k == TH_KEY_PAD ? UV_ACC2 : UV_LBL;
	int ew = 1, fill = 0;
	if (k == TH_KEY_FUNC) ink = UV_ACC2;
	else if (k == TH_KEY_DANGER) edge = ink = TH->bad;
	else if (k == TH_KEY_ACCENT) edge = UV_ACC;
	if (disabled) { edge = UV_TRK; ink = UV_FAINT; sb = UV_FAINT; }
	else if (checked && k != TH_KEY_PAD)
	{
		fill = k == TH_KEY_DANGER ? TH->bad : UV_ACC;
		th_fill (l, &c, 0, 0, w - 1, h - 1, fill, LV_OPA_COVER, r);
		edge = 0; ink = TH->sel_ink; sb = TH->sel_ink;
	}
	else if (pressed)
	{
		th_fill (l, &c, 0, 0, w - 1, h - 1, th_mix (UV_BG, k == TH_KEY_DANGER ? TH->bad : UV_ACC, 70), LV_OPA_COVER, r);
		edge = k == TH_KEY_DANGER ? TH->bad : UV_ACC;
	}
	if (k == TH_KEY_PAD && !disabled && (checked || lc)) { edge = lc && !checked ? lc : UV_ACC; ew = 2; }	// a pad: selected or lit, its outline
	if (k == TH_KEY_NAV && lc && !checked && !disabled) ink = lc;				// REC and the transport: the name in the LED's colour
	if (edge) th_frame (l, &c, 0, 0, w - 1, h - 1, edge, ew, r);
	if (lc && k != TH_KEY_PAD && k != TH_KEY_NAV && !checked)				// a corner mark (a trig key: a bar under its number)
	{
		if (k == TH_KEY_TRIG) th_fill (l, &c, w / 2 - LU (8), h - LU (7), w / 2 + LU (8), h - LU (5), lc, LV_OPA_COVER, 0);
		else th_fill (l, &c, w - LU (10), LU (4), w - LU (5), LU (9), lc, LV_OPA_COVER, 0);
	}
	if (tabbed)							// the name centred a little low, its T key small at the top right
	{
		th_text (l, &c, 0, (h - font_line (f)) / 2 + (sub && sub[0] ? LU (4) : 0), w, f, ink, LV_TEXT_ALIGN_CENTER, text);
		if (sub && sub[0]) th_text (l, &c, 0, LU (4), w - LU (5), TH->f_micro, checked ? TH->sel_ink : disabled ? UV_FAINT : UV_LBL, LV_TEXT_ALIGN_RIGHT, sub);
		return;
	}
	const int th = font_line (f) + (sub && sub[0] ? font_line (TH->f_micro) : 0), ty = (h - th) / 2;
	const int tx = left ? LU (12) : 0, tw = left ? w - LU (16) : w;
	const lv_text_align_t al = left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER;
	th_text (l, &c, tx, ty, tw, f, ink, al, text);
	if (sub && sub[0]) th_text (l, &c, tx, ty + font_line (f), tw, TH->f_micro, sb, al, sub);
}

void th_paint_key (lv_layer_t *l, const lv_area_t *a, const char *text, const char *sub, const lv_font_t *f, int kind,
		   int checked, int pressed, int disabled, int led)
{
	if (sq ()) { sq_key (l, a, text, sub, f, kind, checked, pressed, disabled, led); return; }
	lv_area_t c = *a;
	int w = lv_area_get_width (a), h = lv_area_get_height (a), k = kind & TH_KEY_KIND;
	int left = kind & TH_KEY_LEFT;
	if (!f) f = kind & TH_KEY_SMALL ? TH->f_micro : k == TH_KEY_NAV ? TH->f_title : TH->f_body;
	int th = font_line (f) + (sub && sub[0] ? font_line (TH->f_micro) : 0);
	int ty = (h - th) / 2;
	if (oled ())
	{
		uint32_t ink = TH->ink, edge = 0xC8C8C8;
		if (k == TH_KEY_DANGER) { ink = TH->bad; edge = TH->bad; }
		if (k == TH_KEY_NAV) edge = DOT_DOTTED;
		if (disabled) { ink = TH->faint; edge = DOT_DOTTED; }
		if (pressed && !disabled)
		{
			th_fill (l, &c, 0, 0, w - 1, h - 1, DOT_DITHER, LV_OPA_COVER, 0);
			ink = 0x000000;
		}
		else if ((checked || (kind & TH_KEY_FILL)) && !disabled)
		{
			th_fill (l, &c, 0, 0, w - 1, h - 1, k == TH_KEY_DANGER ? TH->bad : TH->sel_bg, LV_OPA_COVER, 0);
			ink = TH->sel_ink;
		}
		else th_fill (l, &c, 0, 0, w - 1, h - 1, 0x000000, LV_OPA_COVER, 0);
		if (k != TH_KEY_PAD || !checked) th_frame (l, &c, 0, 0, w - 1, h - 1, edge, 1, 0);
		if (k == TH_KEY_ACCENT && !checked && !pressed)	// the main action: a double frame
			th_frame (l, &c, LU (3), LU (3), w - 1 - LU (3), h - 1 - LU (3), DOT_DOTTED, 1, 0);
		if (led > 0)
		{
			uint32_t lc = th_led (led);
			if (k == TH_KEY_TRIG || k == TH_KEY_NAV) th_fill (l, &c, 4, h - 4, w - 5, h - 3, lc, LV_OPA_COVER, 0);	// under the text
			else th_fill (l, &c, w - 7, 2, w - 3, 4, lc, LV_OPA_COVER, 0);			// a corner LED
		}
		int tx = left ? LU (6) : 0, tw = left ? w - LU (8) : w;
		if (k == TH_KEY_NAV && text)			// an icon, its name small under it
		{
			const int ih = ICON_H * (TH->big ? 2 : 1);
			int iy = (h - ih - 3 - lv_font_get_line_height (TH->f_micro) + 2) / 2, dx = sub && sub[0] ? 4 : 0;
			if (nav_icon (l, &c, text, w / 2 + dx, iy, ink))	// (clear of the corner's trig)
			{
				th_text (l, &c, 2 * dx + 4, iy + ih + 2, w - 2 * dx - 4, TH->f_micro, ink, LV_TEXT_ALIGN_CENTER, text);	// under the icon, 2 px right for balance (user)
				if (sub && sub[0])			// UX review N1: its UP + T trig in the corner
					th_text (l, &c, 2, 1, w - 4, TH->f_micro, checked || pressed ? ink : TH->faint, LV_TEXT_ALIGN_LEFT, sub);
				return;
			}
		}
		th_text (l, &c, tx, ty, tw, f, ink, left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, text);
		if (sub && sub[0])
			th_text (l, &c, tx, ty + font_line (f), tw, TH->f_micro, checked || pressed ? ink : TH->dim,
				 left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, sub);
		return;
	}
	// SIDECAR: outlined pills (taller keys: rounded tiles), the selection filled with the accent; the main action and FUNC
	// tinted, DANGER coral; an LED a dot in the corner (a pad's: its outline lit); no shadows, no glow
	const int tile = k == TH_KEY_PAD || left || h > LU (60);
	const int r = tile ? (TH->big ? 14 : 11) : h / 2 < LU (24) ? h / 2 : LU (24);
	uint32_t lc = led > 0 && !disabled ? th_led (led) : 0;
	uint32_t edge = UV_CTRL, ink = UV_TXT, sb = UV_LBL;
	int ew = TH->edge_w, dashed = 0;
	if (k == TH_KEY_NAV)					// the rail: the page's key a light pill, an icon over its name, its T key in the corner
	{
		uint32_t nink = checked ? UV_BG : text && !strcmp (text, "REC") ? UV_COR : HC ? UV_ACC : UV_LBL;
		if (pressed && !checked) th_fill (l, &c, LU (5), 2, w - 1 - LU (5), h - 3, th_mix (UV_RAIL, UV_ACC, 50), LV_OPA_COVER, LU (12));
		if (checked && HC)					// high contrast: the accent, rounded toward the content (left), flush on the rail's line
		{
			th_fill (l, &c, 0, 2, w - 1, h - 3, UV_ACC, LV_OPA_COVER, LU (12));
			th_fill (l, &c, w / 2, 2, w - 1, h - 3, UV_ACC, LV_OPA_COVER, 0);
		}
		else if (checked) th_fill (l, &c, LU (5), 2, w - 1 - LU (5), h - 3, UV_TXT, LV_OPA_COVER, LU (12));
		const int ih = ICON_H * 2, fh = lv_font_get_line_height (TH->f_micro), iy = (h - ih - LU (4) - fh) / 2;
		if (lc && !checked) nink = lc;
		nav_icon (l, &c, text, w / 2, iy, nink);
		th_text (l, &c, LU (6), iy + ih + LU (4), w - LU (12), TH->f_micro, checked ? UV_BG : nink == UV_COR || HC ? nink : UV_LBL, LV_TEXT_ALIGN_CENTER, text);
		if (sub && sub[0]) th_text (l, &c, LU (11), LU (4), w - LU (16), TH->f_micro, checked ? (HC ? UV_BG : 0x3B3670) : HC ? UV_LBL : UV_FAINT, LV_TEXT_ALIGN_LEFT, sub);
		return;
	}
	if (HC && (kind & TH_KEY_TAB))				// the header's page keys, high contrast: a tab, the page's one filled with the accent, its top corners rounded,
	{							// its foot flush on the header's accent line (title_draw)
		uint32_t tink = UV_ACC, tsb = UV_LBL;
		if (checked || pressed)
		{
			uint32_t f = checked ? UV_ACC : th_mix (UV_BG, UV_ACC, 90);
			th_fill (l, &c, 0, 0, w - 1, h - 1, f, LV_OPA_COVER, TH->big ? 10 : 8);
			th_fill (l, &c, 0, h / 2, w - 1, h - 1, f, LV_OPA_COVER, 0);
			if (checked) tink = tsb = UV_BG;
		}
		th_text (l, &c, 0, ty, w, f, tink, LV_TEXT_ALIGN_CENTER, text);
		if (sub && sub[0]) th_text (l, &c, 0, ty + font_line (f), w, TH->f_micro, tsb, LV_TEXT_ALIGN_CENTER, sub);
		return;
	}
	if (disabled)							// (high contrast: a dashed outline too, so disabled is not only a colour)
	{
		edge = HC ? UV_FAINT : UV_BORD; ink = UV_FAINT; sb = UV_FAINT;
		dashed = HC;
	}
	else if (checked || (kind & TH_KEY_FILL))
	{
		uint32_t f = k == TH_KEY_DANGER ? UV_COR : UV_ACC;
		th_fill (l, &c, 0, 0, w - 1, h - 1, pressed ? th_mix (f, UV_TXT, 80) : f, LV_OPA_COVER, r);
		edge = 0; ink = UV_BG; sb = HC ? UV_BG : 0x2C2950;
	}
	else
	{
		if (k == TH_KEY_DANGER) { edge = UV_COR; ink = UV_COR2; }
		else if (k == TH_KEY_ACCENT || k == TH_KEY_FUNC) { edge = UV_ACC; ew = k == TH_KEY_ACCENT ? 2 : TH->edge_w; }
		if (pressed) th_fill (l, &c, 0, 0, w - 1, h - 1, th_mix (UV_CARD, k == TH_KEY_DANGER ? UV_COR : UV_ACC, 90), LV_OPA_COVER, r);
		else if (HC) {}						// high contrast: no tints, black inside
		else if (k == TH_KEY_ACCENT || k == TH_KEY_FUNC) th_fill (l, &c, 0, 0, w - 1, h - 1, th_mix (UV_CARD, UV_ACC, 40), LV_OPA_COVER, r);
		else if (k == TH_KEY_DANGER) th_fill (l, &c, 0, 0, w - 1, h - 1, th_mix (UV_CARD, UV_COR, 36), LV_OPA_COVER, r);
		else if (lc && k == TH_KEY_PAD) th_fill (l, &c, 0, 0, w - 1, h - 1, th_mix (UV_CARD, lc, 40), LV_OPA_COVER, r);
	}
	if (lc && k == TH_KEY_PAD && !checked) { edge = lc; ew = 2; }	// a pad lit: its outline in the LED's colour
	if (edge) th_frame (l, &c, 0, 0, w - 1, h - 1, edge, ew, r);
	if (dashed)							// gaps cut in the straight top and bottom edges (the rounded ends stay solid)
	{
		const int per = LU (10), gap = LU (4);
		for (int x = r + per / 2; x + gap < w - r; x += per)
		{
			th_fill (l, &c, x, 0, x + gap - 1, ew - 1, UV_BG, LV_OPA_COVER, 0);
			th_fill (l, &c, x, h - ew, x + gap - 1, h - 1, UV_BG, LV_OPA_COVER, 0);
		}
	}
	if (lc && k != TH_KEY_PAD)				// a corner dot (a trig key: a bar under its number)
	{
		if (k == TH_KEY_TRIG) th_fill (l, &c, w / 2 - LU (8), h - LU (7), w / 2 + LU (8), h - LU (5), lc, LV_OPA_COVER, LU (1));
		else circle (l, &c, w - 1 - r / 2 - LU (4), LU (4) + r / 3 + 1, LU (3), lc, LV_OPA_COVER);
	}
	const int tx = left ? LU (12) : 0, tw = left ? w - LU (16) : w;
	const lv_text_align_t al = left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER;
	th_text (l, &c, tx, ty, tw, f, ink, al, text);
	if (sub && sub[0]) th_text (l, &c, tx, ty + font_line (f), tw, TH->f_micro, sb, al, sub);
}

// ---- widgets

typedef struct
{
	char		text[24], sub[24];
	int16_t		kind, idx;
	int16_t		led, row;
	uint8_t		checkable;
	const lv_font_t	*font;
	th_cb_t		cb;
	void		*ud;
} key_t;

typedef struct
{
	th_cb_t		cb;
	void		*ud;
	int16_t		n, sel, kind, gap, x, y, w, h;
} seg_t;

static void free_ud (lv_event_t *e)
{
	void *p = lv_obj_get_user_data (lv_event_get_target_obj (e));
	if (p) lv_free (p);
}

static void copy (char *d, const char *s, int cap)
{
	int i = 0;
	for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
	d[i] = 0;
}

lv_obj_t *th_box (lv_obj_t *parent, int x, int y, int w, int h)
{
	lv_obj_t *o = lv_obj_create (parent);
	lv_obj_remove_style_all (o);
	lv_obj_set_pos (o, x, y);
	lv_obj_set_size (o, w, h);
	lv_obj_remove_flag (o, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag (o, LV_OBJ_FLAG_CLICKABLE);
	return o;
}

static void key_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	key_t *k = (key_t *) lv_obj_get_user_data (o);
	lv_area_t a;
	lv_obj_get_coords (o, &a);
	th_paint_key (lv_event_get_layer (e), &a, k->text, k->sub, k->font, k->kind, lv_obj_has_state (o, LV_STATE_CHECKED),
		      lv_obj_has_state (o, LV_STATE_PRESSED), lv_obj_has_state (o, LV_STATE_DISABLED), k->led);
}

static void key_ev (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	lv_event_code_t code = lv_event_get_code (e);
	if (code == LV_EVENT_REFR_EXT_DRAW_SIZE) { lv_event_set_ext_draw_size (e, 6); return; }
	if (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
	{
		lv_area_t a;
		lv_obj_get_coords (o, &a);
		lv_area_increase (&a, 6, 6);			// (a key's drawing may reach past it)
		lv_obj_invalidate_area (o, &a);
		return;
	}
	if (code != LV_EVENT_CLICKED || lv_obj_has_state (o, LV_STATE_DISABLED)) return;
	key_t *k = (key_t *) lv_obj_get_user_data (o);
	lv_obj_t *p = lv_obj_get_parent (o);
	if (k->idx >= 0)					// a seg's item
	{
		seg_t *s = (seg_t *) lv_obj_get_user_data (p);
		if (k->checkable) th_seg_set (p, k->idx);
		if (s->cb) s->cb (p, k->idx, s->ud);
		return;
	}
	if (k->cb) k->cb (o, 0, k->ud);
}

static void inval_key (lv_obj_t *o)
{
	lv_area_t a;
	lv_obj_get_coords (o, &a);
	lv_area_increase (&a, 6, 6);
	lv_obj_invalidate_area (o, &a);
}

static lv_obj_t *key_new (lv_obj_t *parent, int x, int y, int w, int h, const char *text, int kind, int idx)
{
	lv_obj_t *o = lv_obj_create (parent);
	lv_obj_remove_style_all (o);
	lv_obj_set_pos (o, x, y);
	lv_obj_set_size (o, w, h);
	lv_obj_remove_flag (o, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag (o, LV_OBJ_FLAG_CLICKABLE);
	key_t *k = (key_t *) lv_malloc_zeroed (sizeof (key_t));
	copy (k->text, text, sizeof k->text);
	k->kind = (int16_t) kind;
	k->idx = (int16_t) idx;
	k->checkable = 1;
	lv_obj_set_user_data (o, k);
	lv_obj_add_event_cb (o, key_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (o, key_ev, LV_EVENT_ALL, 0);
	lv_obj_add_event_cb (o, free_ud, LV_EVENT_DELETE, 0);
	return o;
}

lv_obj_t *th_key (lv_obj_t *parent, int x, int y, int w, int h, const char *text, int kind, th_cb_t cb, void *ud)
{
	lv_obj_t *o = key_new (parent, x, y, w, h, text, kind, -1);
	key_t *k = (key_t *) lv_obj_get_user_data (o);
	k->cb = cb;
	k->ud = ud;
	return o;
}

void th_key_text (lv_obj_t *o, const char *text)
{
	key_t *k = (key_t *) lv_obj_get_user_data (o);
	if (strncmp (k->text, text, sizeof k->text - 1) == 0) return;
	copy (k->text, text, sizeof k->text);
	inval_key (o);
}

void th_key_sub (lv_obj_t *o, const char *sub)
{
	key_t *k = (key_t *) lv_obj_get_user_data (o);
	if (strncmp (k->sub, sub, sizeof k->sub - 1) == 0) return;
	copy (k->sub, sub, sizeof k->sub);
	inval_key (o);
}

void th_key_led (lv_obj_t *o, int c)
{
	key_t *k = (key_t *) lv_obj_get_user_data (o);
	if (k->led == c) return;
	k->led = (int16_t) c;
	inval_key (o);
}

void th_key_kind (lv_obj_t *o, int kind)
{
	key_t *k = (key_t *) lv_obj_get_user_data (o);
	if (k->kind == kind) return;
	k->kind = (int16_t) kind;
	inval_key (o);
}

void th_key_font (lv_obj_t *o, const lv_font_t *f)
{
	((key_t *) lv_obj_get_user_data (o))->font = f;
	inval_key (o);
}

void th_seg_font (lv_obj_t *seg, const lv_font_t *f)
{
	for (int i = 0; i < th_seg_count (seg); i++) th_key_font (th_seg_item (seg, i), f);
}

void th_key_check (lv_obj_t *o, int on)
{
	if (!!lv_obj_has_state (o, LV_STATE_CHECKED) == !!on) return;
	if (on) lv_obj_add_state (o, LV_STATE_CHECKED); else lv_obj_remove_state (o, LV_STATE_CHECKED);
	inval_key (o);
}

void th_key_enable (lv_obj_t *o, int on)
{
	if (!lv_obj_has_state (o, LV_STATE_DISABLED) == !!on) return;
	if (on) lv_obj_remove_state (o, LV_STATE_DISABLED); else lv_obj_add_state (o, LV_STATE_DISABLED);
	inval_key (o);
}

// the items' places: rows split by "\n", each row's items share the width
static void seg_layout (lv_obj_t *seg, const char *const *map, int create)
{
	seg_t *s = (seg_t *) lv_obj_get_user_data (seg);
	int rows = 1, n = 0;
	for (int i = 0; map[i][0]; i++) if (strcmp (map[i], "\n") == 0) rows++;
	const int gap = guide () ? -1 : s->gap;			// GUIDE: the items overlap by a pixel, one line between two
	int rh = (s->h - (rows - 1) * gap) / rows, row = 0, i = 0;
	while (map[i][0])
	{
		int cnt = 0;
		while (map[i + cnt][0] && strcmp (map[i + cnt], "\n")) cnt++;
		int y = row * (rh + gap);
		for (int j = 0; j < cnt; j++)
		{
			int x1 = j * (s->w + gap) / cnt, x2 = (j + 1) * (s->w + gap) / cnt - gap;
			lv_obj_t *it = create ? 0 : th_seg_item (seg, n);
			if (!it)
			{
				it = key_new (seg, x1, y, x2 - x1, rh, map[i + j], s->kind | TH_KEY_HOLD, n);
				const lv_font_t *f = n ? ((key_t *) lv_obj_get_user_data (th_seg_item (seg, 0)))->font : 0;
				((key_t *) lv_obj_get_user_data (it))->font = f;	// a new item takes the others' font
			}
			else
			{
				lv_obj_set_pos (it, x1, y);
				lv_obj_set_size (it, x2 - x1, rh);
				th_key_text (it, map[i + j]);
				lv_obj_remove_flag (it, LV_OBJ_FLAG_HIDDEN);
			}
			((key_t *) lv_obj_get_user_data (it))->row = (int16_t) row;
			n++;
		}
		i += cnt;
		if (map[i][0]) { i++; row++; }
	}
	for (uint32_t k = (uint32_t) n; k < lv_obj_get_child_count (seg); k++) lv_obj_add_flag (lv_obj_get_child (seg, (int32_t) k), LV_OBJ_FLAG_HIDDEN);
	s->n = (int16_t) n;
	if (s->sel >= n) s->sel = -1;
}

lv_obj_t *th_seg (lv_obj_t *parent, int x, int y, int w, int h, const char *const *map, int kind, th_cb_t cb, void *ud)
{
	lv_obj_t *o = th_box (parent, x, y, w, h);
	seg_t *s = (seg_t *) lv_malloc_zeroed (sizeof (seg_t));
	s->cb = cb; s->ud = ud; s->sel = -1; s->kind = (int16_t) kind; s->gap = (int16_t) (sq () ? 4 : TH->gap);	// (SQUARE, GUIDE: the tight gap inside selector groups)
	s->x = (int16_t) x; s->y = (int16_t) y; s->w = (int16_t) w; s->h = (int16_t) h;
	lv_obj_set_user_data (o, s);
	lv_obj_add_event_cb (o, free_ud, LV_EVENT_DELETE, 0);
	seg_layout (o, map, 1);
	return o;
}

void th_seg_gap (lv_obj_t *seg, int gap)
{
	seg_t *s = (seg_t *) lv_obj_get_user_data (seg);
	s->gap = (int16_t) gap;
	// the items keep their texts: rebuilt from them
	static char t[64][24];
	static const char *m[64 * 2 + 1];
	int n = s->n, k = 0;
	for (int i = 0; i < n && i < 64; i++)
	{
		lv_obj_t *it = th_seg_item (seg, i);
		copy (t[i], ((key_t *) lv_obj_get_user_data (it))->text, 24);
		if (i && ((key_t *) lv_obj_get_user_data (it))->row != ((key_t *) lv_obj_get_user_data (th_seg_item (seg, i - 1)))->row) m[k++] = "\n";
		m[k++] = t[i];
	}
	m[k] = "";
	seg_layout (seg, m, 0);
}

void th_seg_map (lv_obj_t *seg, const char *const *map)
{
	seg_t *s = (seg_t *) lv_obj_get_user_data (seg);
	int old = s->n;
	seg_layout (seg, map, 0);
	for (int i = old; i < s->n; i++) th_key_check (th_seg_item (seg, i), 0);
	lv_obj_invalidate (seg);
}

lv_obj_t *th_seg_item (lv_obj_t *seg, int idx)
{
	if (idx < 0 || idx >= (int) lv_obj_get_child_count (seg)) return 0;
	return lv_obj_get_child (seg, idx);
}

int th_seg_count (lv_obj_t *seg) { return ((seg_t *) lv_obj_get_user_data (seg))->n; }
int th_seg_get (lv_obj_t *seg) { return ((seg_t *) lv_obj_get_user_data (seg))->sel; }

void th_seg_set (lv_obj_t *seg, int idx)
{
	seg_t *s = (seg_t *) lv_obj_get_user_data (seg);
	if (idx >= s->n) idx = -1;
	if (idx >= 0 && !((key_t *) lv_obj_get_user_data (th_seg_item (seg, idx)))->checkable) return;
	if (s->sel == idx) return;
	if (s->sel >= 0) th_key_check (th_seg_item (seg, s->sel), 0);
	if (idx >= 0) th_key_check (th_seg_item (seg, idx), 1);
	s->sel = (int16_t) idx;
}

void th_seg_enable (lv_obj_t *seg, int idx, int on)
{
	seg_t *s = (seg_t *) lv_obj_get_user_data (seg);
	for (int i = 0; i < s->n; i++)
		if (idx < 0 || i == idx) th_key_enable (th_seg_item (seg, i), on);
}

void th_seg_checkable (lv_obj_t *seg, int idx, int on)
{
	lv_obj_t *it = th_seg_item (seg, idx);
	if (it) ((key_t *) lv_obj_get_user_data (it))->checkable = (uint8_t) on;
}

void th_seg_led (lv_obj_t *seg, int idx, int c)
{
	lv_obj_t *it = th_seg_item (seg, idx);
	if (it) th_key_led (it, c);
}

// ---- panels, title bars, labels

static void ext6 (lv_event_t *e) { lv_event_set_ext_draw_size (e, 6); }

typedef struct { char title[32]; int16_t kind; } panel_t;

static void panel_draw (lv_event_t *e);

static int is_panel (lv_obj_t *o)
{
	for (uint32_t i = 0; i < lv_obj_get_event_count (o); i++)
		if (lv_event_dsc_get_cb (lv_obj_get_event_dsc (o, i)) == panel_draw) return 1;
	return 0;
}

// GUIDE: a panel's frame reaches half way to a panel beside or above / below it (up to 16 px away), where the neighbour's
// does too: one line between two panels, the page's grid. The numbers are relative to c; the neighbours' are the same
// pixel for both (the sum of the two edges halved).
static void snap_edges (lv_obj_t *o, const lv_area_t *c, int *x1, int *y1, int *x2, int *y2)
{
	lv_obj_t *p = lv_obj_get_parent (o);
	const int near = 16;
	for (uint32_t i = 0; p && i < lv_obj_get_child_count (p); i++)
	{
		lv_obj_t *s = lv_obj_get_child (p, (int32_t) i);
		lv_area_t a;
		if (s == o || lv_obj_has_flag (s, LV_OBJ_FLAG_HIDDEN) || !is_panel (s)) continue;
		lv_obj_get_coords (s, &a);
		const int vy = a.y1 <= c->y2 && a.y2 >= c->y1, hx = a.x1 <= c->x2 && a.x2 >= c->x1;
		if (vy && a.x1 > c->x2 && a.x1 - c->x2 <= near) *x2 = (c->x2 + a.x1) / 2 - c->x1;
		if (vy && a.x2 < c->x1 && c->x1 - a.x2 <= near) *x1 = (a.x2 + c->x1) / 2 - c->x1;
		if (hx && a.y1 > c->y2 && a.y1 - c->y2 <= near) *y2 = (c->y2 + a.y1) / 2 - c->y1;
		if (hx && a.y2 < c->y1 && c->y1 - a.y2 <= near) *y1 = (a.y2 + c->y1) / 2 - c->y1;
	}
}

static void panel_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	panel_t *p = (panel_t *) lv_obj_get_user_data (o);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c);
	if (p->kind == TH_PANEL_BARE) return;
	if (oled ())
	{
		// a dotted frame, the title in a notch of its top edge
		th_frame (l, &c, 0, LU (4), w - 1, h - 1, DOT_DOTTED, 1, 0);
		if (p->title[0])
		{
			int tw = th_text_width (TH->f_micro, p->title);
			th_fill (l, &c, 4, 0, 4 + tw + 5, LU (8), 0x000000, LV_OPA_COVER, 0);
			th_text (l, &c, 7, -1, tw + 2, TH->f_micro, TH->dim, LV_TEXT_ALIGN_LEFT, p->title);
		}
		return;
	}
	if (sq ())
	{
		// SQUARE, GUIDE: a 1 px olive frame (GUIDE: shared with its neighbours'), the label small, capitals, 1 px tracked, at
		// the top left inside; the controls below share its left edge
		int x1 = 0, y1 = 0, x2 = w - 1, y2 = h - 1;
		if (guide ()) snap_edges (o, &c, &x1, &y1, &x2, &y2);
		th_fill (l, &c, 0, 0, w - 1, h - 1, UV_CARD, LV_OPA_COVER, 0);		// (a dialog's window covers what is under it)
		th_frame (l, &c, x1, y1, x2, y2, UV_BORD, TH->edge_w, 0);
		if (p->title[0]) text_ls (l, &c, LU (12), LU (9), w - LU (24), TH->f_micro, UV_LBL, LV_TEXT_ALIGN_LEFT, p->title, 1);
		return;
	}
	// SIDECAR: a card (rounded, its border a step lighter than the card); a title is a small pill on the card's top edge,
	// the accent with dark text. A group and a window look the same: the page's controls all sit on cards
	const int r = TH->big ? 16 : 13;
	int top = 0;
	if (p->title[0])
	{
		const lv_font_t *f = TH->f_micro;
		int tw = th_text_width (f, p->title), ph = font_line (f) + LU (4);
		top = ph / 2;
		if (tw > w - LU (40)) tw = w - LU (40);
		th_fill (l, &c, 0, top, w - 1, h - 1, UV_CARD, LV_OPA_COVER, r);
		th_frame (l, &c, 0, top, w - 1, h - 1, UV_BORD, TH->edge_w, r);
		th_fill (l, &c, LU (12), 0, LU (12) + tw + LU (18) - 1, ph - 1, HC ? UV_BG : UV_ACC, LV_OPA_COVER, ph / 2);
		if (HC) th_frame (l, &c, LU (12), 0, LU (12) + tw + LU (18) - 1, ph - 1, UV_TXT, TH->edge_w, ph / 2);	// the pill: black, white edge
		th_text (l, &c, LU (21), (ph - font_line (f)) / 2, tw, f, HC ? UV_TXT : UV_BG, LV_TEXT_ALIGN_LEFT, p->title);
		return;
	}
	th_fill (l, &c, 0, 0, w - 1, h - 1, UV_CARD, LV_OPA_COVER, r);
	th_frame (l, &c, 0, 0, w - 1, h - 1, UV_BORD, TH->edge_w, r);
}

lv_obj_t *th_panel (lv_obj_t *parent, int x, int y, int w, int h, const char *title, int kind)
{
	lv_obj_t *o = th_box (parent, x, y, w, h);
	panel_t *p = (panel_t *) lv_malloc_zeroed (sizeof (panel_t));
	copy (p->title, title, sizeof p->title);
	if (!oled ()) for (char *q = p->title; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
	p->kind = (int16_t) kind;
	lv_obj_set_user_data (o, p);
	lv_obj_add_event_cb (o, panel_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (o, ext6, LV_EVENT_REFR_EXT_DRAW_SIZE, 0);
	lv_obj_add_event_cb (o, free_ud, LV_EVENT_DELETE, 0);
	return o;
}

int th_panel_top (void) { return LU (oled () ? 12 : 30); }

typedef struct { char left[48], right[48]; int16_t alert, led; } title_t;

static void title_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	title_t *t = (title_t *) lv_obj_get_user_data (o);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c);
	const lv_font_t *f = TH->f_title;
	int ty = (h - f->line_height) / 2;
	if (oled ())
	{
		th_fill (l, &c, 0, 0, w - 1, h - 1, t->alert ? DOT_ALERT : DOT_DITHER, LV_OPA_COVER, 0);
		th_text (l, &c, LU (7), ty, w - LU (10), f, 0x000000, LV_TEXT_ALIGN_LEFT, t->left);	// a margin: the text clear of the edge
		int rx = LU (16);					// the LED's notch kept: a blinking REC does not move the text
		if (th_fit_report && t->right[0])
		{
			int need = th_text_width (f, t->left) + 8 + th_text_width (TH->f_body, t->right);
			if (need > w - LU (7) - rx) th_fit_report (t->right, need, w - LU (7) - rx);
		}
		th_text (l, &c, LU (7), (h - TH->f_body->line_height) / 2, w - LU (7) - rx, TH->f_body, 0x000000, LV_TEXT_ALIGN_RIGHT, t->right);
		if (t->led > 0)					// a status LED at the right end: a dot block in a black notch
		{
			th_fill (l, &c, w - LU (12), 1, w - 2, h - 2, 0x000000, LV_OPA_COVER, 0);
			th_fill (l, &c, w - LU (12) + 1, 2, w - 3, h - 3, th_led (t->led), LV_OPA_COVER, 0);
		}
		return;
	}
	if (sq ())
	{
		// SQUARE, GUIDE: the screen's charcoal over a 1 px olive line; the page's name 18 px at the left, the state 13 px muted at
		// the right with its status dot; the power warning amber with ink text
		const uint32_t bg = t->alert ? SQ_AMBER : UV_BG, ink = t->alert ? TH->sel_ink : UV_TXT;
		th_fill (l, &c, 0, 0, w - 1, h - 1, bg, LV_OPA_COVER, 0);
		th_fill (l, &c, 0, h - 1, w - 1, h - 1, UV_BORD, LV_OPA_COVER, 0);
		th_text (l, &c, LU (16), ty, w - LU (28), f, ink, LV_TEXT_ALIGN_LEFT, t->left);
		const int rx = LU (t->led > 0 ? 34 : 16);
		if (th_fit_report && t->right[0])
		{
			int need = th_text_width (f, t->left) + LU (12) + th_text_width (TH->f_body, t->right);
			if (need > w - LU (16) - rx) th_fit_report (t->right, need, w - LU (16) - rx);
		}
		th_text (l, &c, LU (16), (h - TH->f_body->line_height) / 2, w - LU (16) - rx, TH->f_body, t->alert ? TH->sel_ink : UV_LBL, LV_TEXT_ALIGN_RIGHT, t->right);
		if (t->led > 0) circle (l, &c, w - LU (20), h / 2, LU (5), th_led (t->led), LV_OPA_COVER);
		return;
	}
	// SIDECAR: the header band, a plain horizontal gradient (no blur, no glow: three fills) over a border line; the power
	// warning coral with dark text
	if (t->alert) th_fill (l, &c, 0, 0, w - 1, h - 1, UV_COR, LV_OPA_COVER, 0);
	else if (HC)							// high contrast: black, no glow, a 3 px accent line under it
	{
		th_fill (l, &c, 0, 0, w - 1, h - 1, UV_BG, LV_OPA_COVER, 0);
		th_fill (l, &c, 0, h - 3, w - 1, h - 1, UV_ACC, LV_OPA_COVER, 0);	// the accent line the page tabs sit on
	}
	else
	{
		static const uint32_t kBand[4] = {0x1A1150, 0x14103A, 0x2A1033, 0x120E36};
		static const int kAt[4] = {0, 90, 158, 256};		// the stops at 0, 35, 62 and 100 % of the width
		for (int i = 0; i < 3; i++) grad_h (l, &c, w * kAt[i] / 256, 0, w * kAt[i + 1] / 256 - 1, h - 2, kBand[i], kBand[i + 1]);
		th_fill (l, &c, 0, h - 1, w - 1, h - 1, UV_BORD, LV_OPA_COVER, 0);
	}
	uint32_t ink = t->alert ? UV_BG : UV_TXT;
	th_text (l, &c, LU (16), ty, w - LU (28), f, ink, LV_TEXT_ALIGN_LEFT, t->left);
	int rx = LU (t->led > 0 ? 34 : 16);
	if (th_fit_report && t->right[0])
	{
		int need = th_text_width (f, t->left) + LU (12) + th_text_width (TH->f_body, t->right);
		if (need > w - LU (16) - rx) th_fit_report (t->right, need, w - LU (16) - rx);
	}
	th_text (l, &c, LU (16), (h - TH->f_body->line_height) / 2, w - LU (16) - rx, TH->f_body, t->alert ? UV_BG : UV_LBL, LV_TEXT_ALIGN_RIGHT, t->right);
	if (t->led > 0) circle (l, &c, w - LU (20), h / 2, LU (6), th_led (t->led), LV_OPA_COVER);
}

lv_obj_t *th_titlebar (lv_obj_t *parent, int x, int y, int w, int h)
{
	lv_obj_t *o = th_box (parent, x, y, w, h);
	title_t *t = (title_t *) lv_malloc_zeroed (sizeof (title_t));
	lv_obj_set_user_data (o, t);
	lv_obj_add_event_cb (o, title_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (o, free_ud, LV_EVENT_DELETE, 0);
	return o;
}

void th_titlebar_led (lv_obj_t *o, int c)
{
	title_t *t = (title_t *) lv_obj_get_user_data (o);
	if (t->led == c) return;
	t->led = (int16_t) c;
	lv_obj_invalidate (o);
}

void th_titlebar_set (lv_obj_t *o, const char *left, const char *right, int alert)
{
	title_t *t = (title_t *) lv_obj_get_user_data (o);
	if (!left) left = t->left;
	if (!right) right = "";
	if (!strcmp (t->left, left) && !strcmp (t->right, right) && t->alert == alert) return;
	copy (t->left, left, sizeof t->left);
	copy (t->right, right, sizeof t->right);
	t->alert = (int16_t) alert;
	lv_obj_invalidate (o);
}

lv_obj_t *th_label (lv_obj_t *parent, int x, int y, const lv_font_t *f, uint32_t rgb, const char *text)
{
	lv_obj_t *l = lv_label_create (parent);
	lv_label_set_text (l, text);
	lv_obj_set_style_text_font (l, f, 0);
	lv_obj_set_style_text_color (l, lv_color_hex (rgb), 0);
	lv_obj_set_pos (l, x, y);
	return l;
}

void th_label_set (lv_obj_t *l, const char *t)
{
	if (strcmp (lv_label_get_text (l), t) != 0) lv_label_set_text (l, t);
}

void th_fit_middle (char *out, int cap, const lv_font_t *f, const char *t, int w)
{
	int n = (int) strlen (t);
	lv_snprintf (out, cap, "%s", t);
	if (th_text_width (f, out) <= w || n < 4) return;
	for (int keep = n - 1; keep >= 2; keep--)		// the most characters that fit, half from each end
	{
		int head = (keep + 1) / 2, tail = keep - head;
		if (head + tail + 3 > cap) continue;
		memcpy (out, t, (size_t) head);
		out[head] = '.', out[head + 1] = '.';
		memcpy (out + head + 2, t + n - tail, (size_t) tail);
		out[head + 2 + tail] = 0;
		if (th_text_width (f, out) <= w) return;
	}
}

void th_label_fit (lv_obj_t *l, const char *t)
{
	char b[160];
	int w = (int) lv_obj_get_style_width (l, 0);		// the width it was given (a plain number: label_w)
	if (w <= 0 || LV_COORD_IS_SPEC (w)) { th_label_set (l, t); return; }
	th_fit_middle (b, sizeof b, lv_obj_get_style_text_font (l, 0), t, w);
	th_label_set (l, b);
}

void th_label_color (lv_obj_t *l, uint32_t rgb)
{
	// UX review L2, the alert grammar: in the OLED theme the attention colour (TH->bad, the FUNC teal's tint) is never
	// plain text - the label is inverted, black on teal, so teal means "look here" and nothing else
	int alert = oled () && rgb == TH->bad;
	lv_color_t c = lv_color_hex (alert ? 0x000000 : rgb);
	if (!lv_color_eq (lv_obj_get_style_text_color (l, 0), c)) lv_obj_set_style_text_color (l, c, 0);
	if (alert != (lv_obj_get_style_bg_opa (l, 0) == LV_OPA_COVER && lv_color_eq (lv_obj_get_style_bg_color (l, 0), lv_color_hex (TH->bad))))
	{
		lv_obj_set_style_bg_color (l, lv_color_hex (TH->bad), 0);
		lv_obj_set_style_bg_opa (l, alert ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	}
}

// ---- meters and sliders

typedef struct { int16_t v, max, kind; uint32_t rgb; char cap[16], val[12]; } meter_t;

static void meter_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	meter_t *m = (meter_t *) lv_obj_get_user_data (o);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c), v = m->v < 0 ? 0 : m->v > m->max ? m->max : m->v;
	if (m->kind == TH_METER_KNOB)
	{
		const lv_font_t *fv = TH->f_micro;
		int ch = m->cap[0] ? TH->f_micro->line_height : 0, vh = m->val[0] ? fv->line_height : 0;
		lv_area_t k = c;
		if (!oled () && vh)					// SIDECAR: the value inside the arc, in the dot font, when it fits
		{
			int kh = h - ch, kr = (w < kh ? w : kh) / 2 - 1, in = 2 * kr - 2 * LU (8);
			const lv_font_t *fn = TH->f_num;
			if (th_text_width (fn, m->val) <= in && fn->line_height <= in)
			{
				k.y2 -= ch;
				th_paint_knob (l, &k, m->v, m->max, strcmp (m->cap, "LEV") ? m->rgb : UV_COR);	// a level coral
				th_text (l, &k, 0, (lv_area_get_height (&k) - fn->line_height) / 2 + 1, w, fn, UV_TXT, LV_TEXT_ALIGN_CENTER, m->val);
				if (ch) th_text (l, &c, 0, h - ch, w, TH->f_micro, UV_LBL, LV_TEXT_ALIGN_CENTER, m->cap);
				return;
			}
		}
		k.y2 -= ch + vh;
		th_paint_knob (l, &k, m->v, m->max, m->rgb);
		if (ch) th_text (l, &c, 0, h - ch - vh, w, TH->f_micro, oled () ? TH->dim : TH->win_dim, LV_TEXT_ALIGN_CENTER, m->cap);
		if (vh) th_text (l, &c, 0, h - vh, w, fv, oled () ? TH->ink : TH->win_ink, LV_TEXT_ALIGN_CENTER, m->val);	// the value under its label
		return;
	}
	int vert = m->kind == TH_METER_V;
	if (oled ())
	{
		// an LED ladder of dots: one dot lit, one dark, the empty part faint
		int len = vert ? h : w, n = (len + 1) / 2, lit = (v * n + m->max / 2) / (m->max ? m->max : 1);
		for (int i = 0; i < n; i++)
		{
			uint32_t col = i < lit ? m->rgb : 0x1E1E1E;
			if (vert) th_fill (l, &c, 0, h - 1 - 2 * i, w - 1, h - 1 - 2 * i, col, LV_OPA_COVER, 0);
			else th_fill (l, &c, 2 * i, 0, 2 * i, h - 1, col, LV_OPA_COVER, 0);
		}
		return;
	}
	if (sq ())
	{
		// SQUARE, GUIDE: a box with a 1 px olive edge, the value a block of the colour inside (green turns amber past 90 %);
		// a thin meter (under 8 px) is a bar on a track
		const int len = vert ? h : w, n = len * v / m->max, thin = (vert ? w : h) < 8, in = thin ? 0 : 1;
		th_fill (l, &c, 0, 0, w - 1, h - 1, thin ? UV_TRK : UV_BG, LV_OPA_COVER, 0);
		if (!thin) th_frame (l, &c, 0, 0, w - 1, h - 1, UV_BORD, 1, 0);
		if (!v || m->v < 0) return;
		const uint32_t col = m->rgb == UV_ACC && n > len * 9 / 10 ? UV_COR : m->rgb;
		if (vert) th_fill (l, &c, in, h - n < in ? in : h - n, w - 1 - in, h - 1 - in, col, LV_OPA_COVER, 0);
		else th_fill (l, &c, in, in, n - 1 < in ? in : n - 1, h - 1 - in, col, LV_OPA_COVER, 0);
		return;
	}
	// SIDECAR: a rounded bar on a track; an accent meter turns magenta past 70 % and coral past 90 % (hot)
	const int r = (vert ? w : h) / 2;
	if (HC)							// high contrast: a white outline, black inside, the fill in the accent
	{
		th_frame (l, &c, 0, 0, w - 1, h - 1, UV_BORD, TH->edge_w, r);
		if (!v || m->v < 0) return;
		const int len = vert ? h : w, n = len * v / m->max, in = TH->edge_w + 1;
		const uint32_t col = m->rgb == UV_ACC && n > len * 9 / 10 ? UV_COR : m->rgb;	// (hot: yellow)
		if (vert) th_fill (l, &c, in, h - n < in ? in : h - n, w - 1 - in, h - 1 - in, col, LV_OPA_COVER, r / 2);
		else th_fill (l, &c, in, in, n - 1 < in ? in : n - 1, h - 1 - in, col, LV_OPA_COVER, r / 2);
		return;
	}
	th_fill (l, &c, 0, 0, w - 1, h - 1, UV_TRK, LV_OPA_COVER, r);
	if (!v || m->v < 0) return;
	const int len = vert ? h : w, n = len * v / m->max;
	const int hot = m->rgb == UV_ACC, m9 = len * 9 / 10, m7 = len * 7 / 10;
	const int ends[3] = {n, hot && n > m9 ? m9 : 0, hot && n > m7 ? m7 : 0};	// the whole value, then the cooler parts over it
	const uint32_t col[3] = {!hot ? m->rgb : n > m9 ? UV_COR : n > m7 ? UV_MAG : UV_ACC, UV_MAG, UV_ACC};
	for (int i = 0; i < 3; i++)
	{
		int e = ends[i];
		if (e < 1) continue;
		if (vert) th_fill (l, &c, 0, h - e, w - 1, h - 1, col[i], LV_OPA_COVER, r);
		else th_fill (l, &c, 0, 0, e - 1, h - 1, col[i], LV_OPA_COVER, r);
	}
}

void th_meter_value (lv_obj_t *o, const char *val)
{
	meter_t *m = (meter_t *) lv_obj_get_user_data (o);
	if (!strcmp (m->val, val)) return;
	copy (m->val, val, sizeof m->val);
	lv_obj_invalidate (o);
}
lv_obj_t *th_meter (lv_obj_t *parent, int x, int y, int w, int h, int max, int kind, uint32_t rgb)
{
	lv_obj_t *o = th_box (parent, x, y, w, h);
	meter_t *m = (meter_t *) lv_malloc_zeroed (sizeof (meter_t));
	m->max = (int16_t) (max > 0 ? max : 1); m->kind = (int16_t) kind; m->rgb = rgb; m->v = -1;
	lv_obj_set_user_data (o, m);
	lv_obj_add_event_cb (o, meter_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (o, free_ud, LV_EVENT_DELETE, 0);
	return o;
}

void th_meter_set (lv_obj_t *o, int v)
{
	meter_t *m = (meter_t *) lv_obj_get_user_data (o);
	if (m->v == v) return;
	m->v = (int16_t) v;
	lv_obj_invalidate (o);
}

void th_meter_label (lv_obj_t *o, const char *cap)
{
	meter_t *m = (meter_t *) lv_obj_get_user_data (o);
	if (!strcmp (m->cap, cap)) return;
	copy (m->cap, cap, sizeof m->cap);
	lv_obj_invalidate (o);
}

typedef struct { int16_t v, min, max, drag; th_cb_t cb; void *ud; } slider_t;

static void slider_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	slider_t *s = (slider_t *) lv_obj_get_user_data (o);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c);
	int span = s->max > s->min ? s->max - s->min : 1;
	if (oled ())
	{
		int kw = LU (5), x = (w - kw) * (s->v - s->min) / span, cy = h / 2;
		th_fill (l, &c, 0, cy, w - 1, cy, DOT_DOTTED, LV_OPA_COVER, 0);		// the track
		th_fill (l, &c, 0, cy - 1, x, cy + 1, TH->ink, LV_OPA_COVER, 0);		// the value
		th_fill (l, &c, x, 0, x + kw - 1, h - 1, s->drag ? TH->sel_bg : TH->ink, LV_OPA_COVER, 0);	// the handle
		th_fill (l, &c, x + kw / 2, 2, x + kw / 2, h - 3, 0x000000, LV_OPA_COVER, 0);
		return;
	}
	if (sq ())
	{
		// SQUARE, GUIDE: a 2 px track, the value green, a square chalk handle (green while dragged)
		int kw = LU (28), x = (w - kw) * (s->v - s->min) / span, hw = LU (12), hh = h < LU (28) ? h : LU (28);
		th_fill (l, &c, 0, h / 2 - 1, w - 1, h / 2, UV_TRK, LV_OPA_COVER, 0);
		th_fill (l, &c, 0, h / 2 - 1, x + kw / 2, h / 2, UV_ACC, LV_OPA_COVER, 0);
		th_fill (l, &c, x + kw / 2 - hw / 2, (h - hh) / 2, x + kw / 2 + hw / 2 - 1, (h + hh) / 2 - 1, s->drag ? UV_ACC : UV_TXT, LV_OPA_COVER, 0);
		return;
	}
	// SIDECAR: a rounded track, the value in the accent, a light round handle (the accent while dragged)
	int kw = LU (28), x = (w - kw) * (s->v - s->min) / span, gh = h / 4 > LU (6) ? LU (6) : h / 4;
	th_fill (l, &c, 0, (h - gh) / 2, w - 1, (h + gh) / 2, UV_TRK, LV_OPA_COVER, gh / 2 + 1);
	th_fill (l, &c, 0, (h - gh) / 2, x + kw / 2, (h + gh) / 2, UV_ACC, LV_OPA_COVER, gh / 2 + 1);
	int hr = (kw < h ? kw : h) / 2 - 1;
	circle (l, &c, x + kw / 2, h / 2, hr, s->drag ? UV_ACC : UV_TXT, LV_OPA_COVER);
}

static void slider_ev (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	slider_t *s = (slider_t *) lv_obj_get_user_data (o);
	lv_event_code_t code = lv_event_get_code (e);
	if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
	lv_point_t p;
	lv_indev_get_point (lv_indev_active (), &p);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), kw = LU (oled () ? 5 : 28);
	int v = s->min + (int) ((long) (p.x - c.x1 - kw / 2) * (s->max - s->min) / (w - kw > 0 ? w - kw : 1));
	v = v < s->min ? s->min : v > s->max ? s->max : v;
	s->drag = code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING;
	if (v != s->v || code != LV_EVENT_PRESSING)
	{
		s->v = (int16_t) v;
		inval_key (o);
		if (s->cb) s->cb (o, v | (code == LV_EVENT_RELEASED ? TH_RELEASED : 0), s->ud);
	}
}

lv_obj_t *th_slider (lv_obj_t *parent, int x, int y, int w, int h, int min, int max, th_cb_t cb, void *ud)
{
	lv_obj_t *o = th_box (parent, x, y, w, h);
	lv_obj_add_flag (o, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area (o, TH->touch / 3);
	slider_t *s = (slider_t *) lv_malloc_zeroed (sizeof (slider_t));
	s->min = (int16_t) min; s->max = (int16_t) max; s->v = (int16_t) max; s->cb = cb; s->ud = ud;
	lv_obj_set_user_data (o, s);
	lv_obj_add_event_cb (o, slider_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (o, slider_ev, LV_EVENT_ALL, 0);
	lv_obj_add_event_cb (o, free_ud, LV_EVENT_DELETE, 0);
	return o;
}

void th_slider_set (lv_obj_t *o, int v)
{
	slider_t *s = (slider_t *) lv_obj_get_user_data (o);
	if (s->v == v || s->drag) return;
	s->v = (int16_t) v;
	inval_key (o);
}

int th_slider_get (lv_obj_t *o) { return ((slider_t *) lv_obj_get_user_data (o))->v; }
int th_slider_dragging (lv_obj_t *o) { return ((slider_t *) lv_obj_get_user_data (o))->drag; }

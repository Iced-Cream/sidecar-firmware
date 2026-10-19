// pi/ui/sidecar_splash.c - the boot screen (sidecar_splash.h): plain pixels into the RGB565 frame buffer, before LVGL.
#include "sidecar_splash.h"
#include "sidecar_wordmark.h"	// "Sidecar" in the docs site's wordmark face, 4-bit alpha (wordmark.py)

// the docs site's dark theme in RGB565, the greys kept grey (as sidecar_ui.c)
#define BG	0x1082		// #101010
#define FG	0xEF5D		// #e8e8e8: the wordmark
#define DIM	0x8C51		// #888888: "EXPANDER"
#define EDGE	0x39C7		// #383838: the rules, the bar's track
#define ACC	0x2DEB		// #2dbd5c: the bar

// 5x7 glyphs, one byte a row (bit 4 = the left column): only the letters of "EXPANDER"
static const struct { char c; unsigned char row[7]; } Font[] = {
	{'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
	{'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
	{'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
	{'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
	{'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
	{'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
	{'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
};

static void rect (uint16_t *fb, unsigned pitch, unsigned w, unsigned h, int x, int y, int rw, int rh, uint16_t c)
{
	for (int j = y; j < y + rh; j++)
	{
		if (j < 0 || j >= (int) h) continue;
		uint16_t *p = fb + (unsigned) j * pitch;
		for (int i = x; i < x + rw; i++)
			if (i >= 0 && i < (int) w) p[i] = c;
	}
}

// the text's width in pixels at scale s, letter gap g (in units of s)
static int text_width (const char *t, int s, int g)
{
	int n = 0;
	for (const char *p = t; *p; p++) n++;
	return n ? n * 5 * s + (n - 1) * g * s : 0;
}

static int text (uint16_t *fb, unsigned pitch, unsigned w, unsigned h, const char *t, int x, int y, int s, int g, uint16_t c)
{
	for (; *t; t++, x += (5 + g) * s)
	{
		for (unsigned k = 0; k < sizeof Font / sizeof Font[0]; k++)
		{
			if (Font[k].c != *t) continue;
			for (int r = 0; r < 7; r++)
				for (int b = 0; b < 5; b++)
					if (Font[k].row[r] & (0x10 >> b)) rect (fb, pitch, w, h, x + b * s, y + r * s, s, s, c);
		}
	}
	return x;
}

// fg over bg at alpha a (0..15), per RGB565 channel
static uint16_t mix (uint16_t bg, uint16_t fg, unsigned a)
{
	int r = (bg >> 11) + (((fg >> 11) - (bg >> 11)) * (int) a) / 15;
	int g = ((bg >> 5) & 63) + ((((fg >> 5) & 63) - ((bg >> 5) & 63)) * (int) a) / 15;
	int b = (bg & 31) + (((fg & 31) - (bg & 31)) * (int) a) / 15;
	return (uint16_t) ((r << 11) | (g << 5) | b);
}

// the wordmark (WordmarkA4: high nibble = left pixel) at (x,y) in c over BG
static void wordmark (uint16_t *fb, unsigned pitch, unsigned w, unsigned h, int x, int y, uint16_t c)
{
	const unsigned stride = (WORDMARK_A4_W + 1) / 2;
	for (int j = 0; j < WORDMARK_A4_H; j++)
	{
		if (y + j < 0 || y + j >= (int) h) continue;
		uint16_t *p = fb + (unsigned) (y + j) * pitch;
		for (int i = 0; i < WORDMARK_A4_W; i++)
		{
			unsigned v = WordmarkA4[j * stride + i / 2], a = i & 1 ? v & 15 : v >> 4;
			if (a && x + i >= 0 && x + i < (int) w) p[x + i] = mix (BG, c, a);
		}
	}
}

// the layout, from the screen's size: the wordmark centred, "EXPANDER" between two rules under it (the docs site's
// home page), the bar under that, all as wide as the wordmark
static void layout (unsigned w, unsigned h, int *pX, int *pY, int *pBarY)
{
	*pX = ((int) w - WORDMARK_A4_W) / 2;
	*pY = (int) h / 2 - WORDMARK_A4_H;
	*pBarY = (int) h / 2 + 46;
}

void sidecar_splash_draw (uint16_t *fb, unsigned pitch, unsigned w, unsigned h)
{
	int x, y, by;
	layout (w, h, &x, &y, &by);
	rect (fb, pitch, w, h, 0, 0, (int) w, (int) h, BG);
	wordmark (fb, pitch, w, h, x, y, FG);
	const int s = 2, g = 3, ty = (int) h / 2 + 12;		// EXPANDER: 5x7 at 2x, wide tracking
	int tw = text_width ("EXPANDER", s, g), tx = ((int) w - tw) / 2;
	text (fb, pitch, w, h, "EXPANDER", tx, ty, s, g, DIM);
	rect (fb, pitch, w, h, x, ty + 7, tx - 14 - x, 1, EDGE);
	rect (fb, pitch, w, h, tx + tw + 14, ty + 7, x + WORDMARK_A4_W - (tx + tw + 14), 1, EDGE);
	rect (fb, pitch, w, h, x, by, WORDMARK_A4_W, 2, EDGE);
}

void sidecar_splash_progress (uint16_t *fb, unsigned pitch, unsigned w, unsigned h, unsigned step, unsigned steps)
{
	int x, y, by;
	layout (w, h, &x, &y, &by);
	if (steps == 0) return;
	if (step > steps) step = steps;
	rect (fb, pitch, w, h, x, by, (int) ((unsigned) WORDMARK_A4_W * step / steps), 2, ACC);
}

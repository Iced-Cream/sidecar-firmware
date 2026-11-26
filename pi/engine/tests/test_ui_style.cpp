// pi/engine/tests/test_ui_style.cpp - ui_style.h: the display theme's words (oled, sidecar, guide; the retired "square" and the old ids read as guide), display.txt's
// uistyle= line (a word not known is none, the caller then reads OLED), and a round trip through the file's text.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "ui_style.h"

using namespace ui_style;

// as kernel.cpp SaveDisplaySettings writes the line
static void save (int s, char *out, size_t cap)
{
	snprintf (out, cap, "screensaver=60\nbrightness=100\ntouchtest=1\nuistyle=%s\nuilook=studio\nuicontrast=off\n", name (s));
}

int main ()
{
	CHECK (parse ("oled") == kOled && parse ("sidecar") == kSidecar && parse ("guide") == kGrid && kStyles == 3);
	CHECK (parse ("classic") == kSidecar);					// the theme SIDECAR replaced
	CHECK (parse ("0") == kOled && parse ("1") == kSidecar && parse ("2") == kGrid);
	// SQUARE is gone (0.5.1 test build): its word and its number, and GRID's old number, load as GRID
	CHECK (parse ("square") == kGrid && parse ("3") == kGrid && kGrid == 2);
	// GUIDE was renamed GRID: the stored word and number are unchanged and an old saved value loads as GRID
	CHECK (parse ("guide") == kGrid && parse ("grid") == kGrid && from_text ("uistyle=guide\n") == kGrid && from_text ("uistyle=grid\n") == kGrid);
	CHECK (!strcmp (name (kGrid), "guide"));
	CHECK (parse ("") < 0 && parse ("SQUARE") < 0 && parse ("4") < 0 && parse ("squares") < 0 && parse ("guid") < 0 && parse (0) < 0);
	CHECK (!strcmp (name (kOled), "oled") && !strcmp (name (kSidecar), "sidecar") && !strcmp (name (kGrid), "guide")
	       && !strcmp (name (9), "oled") && !strcmp (name (-1), "oled"));
	CHECK (!strcmp (name (parse ("square")), "guide"));				// saved again, it is written as guide

	// old cards: no key (none: the caller's default, OLED); the old words and ids
	CHECK (from_text ("") < 0 && from_text ("screensaver=60\nbrightness=100\n") < 0);
	CHECK (from_text ("uistyle=oled\n") == kOled && from_text ("uistyle=classic\n") == kSidecar && from_text ("uistyle=1\n") == kSidecar);
	// the new words in the middle of the file, with CRLF, at the end without a newline
	CHECK (from_text ("brightness=100\nuistyle=square\nuilook=stage\n") == kGrid);		// a card from the test build
	CHECK (from_text ("uistyle=2\n") == kGrid && from_text ("uistyle=3\r\n") == kGrid);
	CHECK (from_text ("uistyle=guide\r\nuilook=stage\r\n") == kGrid);
	CHECK (from_text ("brightness=100\nuistyle=guide") == kGrid);
	// not known: none (and the lookalikes: a key not at a line's start, a longer word)
	CHECK (from_text ("uistyle=round\n") < 0 && from_text ("uistyle=\n") < 0 && from_text ("uistyle=squared\n") < 0);
	CHECK (from_text ("xuistyle=square\n") < 0 && from_text ("uistyle=square-and-more-text\n") < 0);
	CHECK (from_text ("xuistyle=square\nuistyle=sidecar\n") == kSidecar);
	CHECK (parse_line ("square\nrest") == kGrid && parse_line ("guide") == kGrid && parse_line ("guide x") < 0);

	// the round trip
	for (int s = 0; s < kStyles; s++)
	{
		char b[160];
		save (s, b, sizeof b);
		CHECK (from_text (b) == s);
	}
	printf ("ui_style: ok\n");
}

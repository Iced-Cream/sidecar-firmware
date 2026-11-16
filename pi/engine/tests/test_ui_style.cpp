// pi/engine/tests/test_ui_style.cpp - ui_style.h: the display theme's words (oled, sidecar, square, guide), display.txt's
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
	CHECK (parse ("oled") == kOled && parse ("sidecar") == kSidecar && parse ("square") == kSquare && parse ("guide") == kGuide);
	CHECK (parse ("classic") == kSidecar);					// the theme SIDECAR replaced
	CHECK (parse ("0") == kOled && parse ("1") == kSidecar && parse ("2") == kSquare && parse ("3") == kGuide);
	CHECK (parse ("") < 0 && parse ("SQUARE") < 0 && parse ("4") < 0 && parse ("squares") < 0 && parse ("guid") < 0 && parse (0) < 0);
	CHECK (!strcmp (name (kOled), "oled") && !strcmp (name (kSidecar), "sidecar") && !strcmp (name (kSquare), "square")
	       && !strcmp (name (kGuide), "guide") && !strcmp (name (9), "oled") && !strcmp (name (-1), "oled"));

	// old cards: no key (none: the caller's default, OLED); the old words and ids
	CHECK (from_text ("") < 0 && from_text ("screensaver=60\nbrightness=100\n") < 0);
	CHECK (from_text ("uistyle=oled\n") == kOled && from_text ("uistyle=classic\n") == kSidecar && from_text ("uistyle=1\n") == kSidecar);
	// the new words in the middle of the file, with CRLF, at the end without a newline
	CHECK (from_text ("brightness=100\nuistyle=square\nuilook=stage\n") == kSquare);
	CHECK (from_text ("uistyle=guide\r\nuilook=stage\r\n") == kGuide);
	CHECK (from_text ("brightness=100\nuistyle=guide") == kGuide);
	// not known: none (and the lookalikes: a key not at a line's start, a longer word)
	CHECK (from_text ("uistyle=round\n") < 0 && from_text ("uistyle=\n") < 0 && from_text ("uistyle=squared\n") < 0);
	CHECK (from_text ("xuistyle=square\n") < 0 && from_text ("uistyle=square-and-more-text\n") < 0);
	CHECK (from_text ("xuistyle=square\nuistyle=guide\n") == kGuide);
	CHECK (parse_line ("square\nrest") == kSquare && parse_line ("guide") == kGuide && parse_line ("guide x") < 0);

	// the round trip
	for (int s = 0; s < kStyles; s++)
	{
		char b[160];
		save (s, b, sizeof b);
		CHECK (from_text (b) == s);
	}
	printf ("ui_style: ok\n");
}

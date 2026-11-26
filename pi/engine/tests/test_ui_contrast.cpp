// pi/engine/tests/test_ui_contrast.cpp - ui_contrast.h: the SIDECAR theme's high-contrast words and display.txt's
// uicontrast= line (default off, a word not known reads as off, a round trip through the file's text).
#include <cstdio>
#include <cstring>

#include "check.h"
#include "ui_contrast.h"

using namespace ui_contrast;

// as kernel.cpp SaveDisplaySettings writes the line
static void save (int m, char *out, size_t cap)
{
	snprintf (out, cap, "screensaver=60\nbrightness=100\nuistyle=sidecar\nuilook=studio\nuicontrast=%s\n", name (m));
}

int main ()
{
	CHECK (parse ("off") == kOff && parse ("blue") == kBlue && parse ("green") == kGreen && parse ("viper") == kViper);
	CHECK (kOff == 0 && kBlue == 1 && kGreen == 2 && kViper == 3 && kModes == 4);	// the stored numbers of the older looks keep their meaning
	CHECK (parse ("") < 0 && parse ("BLUE") < 0 && parse ("red") < 0 && parse (0) < 0);
	CHECK (!strcmp (name (kOff), "off") && !strcmp (name (kBlue), "blue") && !strcmp (name (kGreen), "green") && !strcmp (name (kViper), "viper") && !strcmp (name (7), "off"));
	CHECK (from_text ("uicontrast=viper\n") == kViper && from_text ("uistyle=sidecar\nuicontrast=viper") == kViper);

	// old cards: no key, so off; the default of a card with nothing at all
	CHECK (from_text ("") == kOff);
	CHECK (from_text ("screensaver=60\nbrightness=100\nuistyle=sidecar\nuilook=stage\n") == kOff);
	// the words, in the middle of the file, with CRLF, at the end without a newline
	CHECK (from_text ("uistyle=sidecar\nuicontrast=blue\nuilook=stage\n") == kBlue);
	CHECK (from_text ("uicontrast=green\r\nuilook=stage\r\n") == kGreen);
	CHECK (from_text ("brightness=100\nuicontrast=green") == kGreen);
	// not known: off (and the lookalikes: a key not at a line's start, a longer word)
	CHECK (from_text ("uicontrast=red\n") == kOff && from_text ("uicontrast=\n") == kOff && from_text ("uicontrast=bluegreen\n") == kOff);
	CHECK (from_text ("xuicontrast=blue\n") == kOff && from_text ("uicontrast=blue-and-more-text\n") == kOff);
	CHECK (from_text ("xuicontrast=blue\nuicontrast=green\n") == kGreen);

	// the round trip
	for (int m = 0; m < kModes; m++)
	{
		char b[160];
		save (m, b, sizeof b);
		CHECK (from_text (b) == m);
	}
	printf ("ui_contrast: ok\n");
}

// pi/engine/ui_contrast.h - the SIDECAR theme's high-contrast option (docs/UI-THEMES.md "High contrast"): its words, the
// console command's value and display.txt's line. Portable, header only (tests/test_ui_contrast.cpp).
//
//   off    the standard SIDECAR palette (the default; a card without the key, or a word not known, reads as this)
//   blue   high contrast, the light blue accent
//   green  high contrast, the Rytm's mute-pad green accent
//   viper  phosphor greens (3: the stored words of off, blue and green keep their meaning)
#ifndef _ui_contrast_h
#define _ui_contrast_h

#include <string.h>

namespace ui_contrast {

enum Mode { kOff = 0, kBlue = 1, kGreen = 2, kViper = 3, kModes };

inline const char *name (int m)		{ return m == kBlue ? "blue" : m == kGreen ? "green" : m == kViper ? "viper" : "off"; }

// a word; -1: none of them
inline int parse (const char *p)
{
	if (p == 0) return -1;
	if (strcmp (p, "off") == 0) return kOff;
	if (strcmp (p, "blue") == 0) return kBlue;
	if (strcmp (p, "green") == 0) return kGreen;
	if (strcmp (p, "viper") == 0) return kViper;
	return -1;
}

// display.txt's text: its uicontrast= line (at a line's start), the value to the line's end; a missing line or a word not
// known is kOff (the stored value of a newer or older build never breaks the display)
inline int from_text (const char *pText)
{
	const char *p = strstr (pText, "uicontrast=");
	while (p != 0 && p != pText && p[-1] != '\n') p = strstr (p + 11, "uicontrast=");
	if (p == 0) return kOff;
	char b[8];
	unsigned n = 0;
	for (p += 11; *p != 0 && *p != '\n' && *p != '\r' && n < sizeof b - 1; p++) b[n++] = *p;
	if (*p != 0 && *p != '\n' && *p != '\r') return kOff;	// longer than any word
	b[n] = 0;
	int m = parse (b);
	return m < 0 ? (int) kOff : m;
}

}	// namespace ui_contrast

#endif

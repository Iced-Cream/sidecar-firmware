# core/ui: the remote screen's drawing

The Sidecar can draw on the Rytm's own 128x64 screen (rytm-mods `0026-remote-ui`). Since the engine strip-down of 2026-11-04 the Sidecar has no pages of its own: `core/screen` is the transport (the connect splash, popups, the pairing code's modal, one client), and this folder is the drawing a client and those use: `ui.h`'s `Canvas`, a 128x64 1-bit frame in the Rytm's framebuffer layout (a tile is the 8 bytes the Rytm's panel takes), text items the Rytm draws with its own fonts, and the CC0 fonts in `fonts/` for previews. The widgets and pages the voice engine had are in git history (`3a8bf7d`).

## Text: the Rytm's own fonts (1021)

The Rytm draws every piece of text itself, with its own text routine and its own fonts (rytm-mods 0026 1021, section
9 of its design.md): the frame is pixels for the graphics plus up to 64 text items (`Canvas::put_text`: slot, x, the
top row, font, alignment, inverted). The host sends the changed items as `3D` with the frame; the Rytm composes them
over the committed pixels. No glyph data of the Rytm is in this repository: a font is named by the Rytm's index
(`RFont` in `ui.h`, with its measured height and widest advance), the Rytm aligns (left / centred / right). `kRSmall`
(index 16) is the stock screens' font: title bar, labels, bottom bar, enum values. `preview()` draws the items with
the CC0 3x5 font in the same boxes, for `sim/rui_render.py` and the tests.

Geometry as the stock screens [measured in rytm-emu]: title bar rows 0..8 (inverted, text at x 2), two rows of 32x22
cells (dials of radius 7 at rows 12..23 / 34..45, labels centred at rows 26 / 48), bottom bar rows 55..63 (inverted,
the touched parameter as stock's "BD LEV:" with a level box).


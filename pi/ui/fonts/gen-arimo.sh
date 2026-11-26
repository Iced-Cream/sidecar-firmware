#!/bin/sh
# pi/ui/fonts/gen-arimo.sh SRC_DIR - the touchscreen themes' text faces (sidecar_theme.c): Arimo Bold (SIL OFL 1.1,
# github.com/googlefonts/Arimo, fonts/ttf/Arimo-Bold.ttf; licence text licenses/OFL-Arimo.txt) at 11 12 14 16 18 19 20 24 30 36
# and 44 px (sidecar_arb<N>: GRID and SIDECAR at the three screen sizes, docs/UI-THEMES.md "Type"), as LVGL tables: 4 bpp,
# not compressed, ASCII 0x20-0x7E and the degree and plus-minus signs.
# If SRC_DIR also holds Arimo-Regular.ttf and FontAwesome5.woff, the symbol tables are regenerated too: Arimo Regular at 13, 18
# and 28 px with the eleven Font Awesome 5 symbols the pages use (as gen.sh), sidecar_ar<N>. The Bold faces carry no symbols; the
# theme asks these tables for them (a font's fallback).
# Needs lv_font_conv 1.5.3 on the PATH (npm i lv_font_conv@1.5.3). The tables are written next to this script.
set -e
SRC=$1
OUT=$(cd "$(dirname "$0")" && pwd)
[ -f "$SRC/Arimo-Bold.ttf" ] || { echo "usage: gen-arimo.sh DIR (Arimo-Bold.ttf; Arimo-Regular.ttf and FontAwesome5.woff for the symbol tables)"; exit 1; }
cd "$SRC"
for s in 11 12 14 16 18 19 20 24 30 36 44; do
	lv_font_conv --bpp 4 --size $s --no-compress --format lvgl --lv-font-name sidecar_arb$s \
		--font Arimo-Bold.ttf -r 0x20-0x7E,0xB0-0xB1 -o arb$s.tmp
	sed -e 's|^#ifdef LV_LVGL_H_INCLUDE_SIMPLE$|#if 1|' -e 's|^ \* Opts: .*| * gen-arimo.sh: Arimo Bold, 4 bpp, ASCII, degree, plus-minus|' arb$s.tmp > "$OUT/sidecar_arb$s.c"
	rm arb$s.tmp
done
if [ -f Arimo-Regular.ttf ] && [ -f FontAwesome5.woff ]; then
	# LV_SYMBOL_ CLOSE OK PLAY PAUSE STOP LEFT RIGHT WARNING UP DOWN TRASH
	SYM=0xF00D,0xF00C,0xF04B,0xF04C,0xF04D,0xF053,0xF054,0xF071,0xF077,0xF078,0xF2ED
	for s in 13 18 28; do
		lv_font_conv --bpp 4 --size $s --no-compress --format lvgl --lv-font-name sidecar_ar$s \
			--font Arimo-Regular.ttf -r 0x20-0x7E,0xB0-0xB1 --font FontAwesome5.woff -r $SYM -o ar$s.tmp
		sed -e 's|^#ifdef LV_LVGL_H_INCLUDE_SIMPLE$|#if 1|' -e 's|^ \* Opts: .*| * gen-arimo.sh: Arimo Regular + Font Awesome 5 symbols, 4 bpp, ASCII, degree, plus-minus|' ar$s.tmp > "$OUT/sidecar_ar$s.c"
		rm ar$s.tmp
	done
fi

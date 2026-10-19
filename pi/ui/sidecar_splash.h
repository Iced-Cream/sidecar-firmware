// pi/ui/sidecar_splash.h - the boot screen: the "Sidecar | Expander" wordmark and a thin progress bar on a dark screen,
// drawn straight into the RGB565 frame buffer from the kernel's first steps (the frame buffer's first use), one bar
// step per boot stage, until LVGL draws the home screen over it (sidecar_display.h). No font engine, no LVGL, no heap: a
// 4-bit bitmap of "Sidecar" (sidecar_wordmark.c) and a 5x7 bitmap font scaled. pitch in pixels. C, so pi/ui/host can render it on the Mac (make splash.png).
#ifndef SIDECAR_SPLASH_H
#define SIDECAR_SPLASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void sidecar_splash_draw (uint16_t *fb, unsigned pitch, unsigned w, unsigned h);
void sidecar_splash_progress (uint16_t *fb, unsigned pitch, unsigned w, unsigned h, unsigned step, unsigned steps);

#ifdef __cplusplus
}
#endif

#endif

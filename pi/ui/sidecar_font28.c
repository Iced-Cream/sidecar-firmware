// pi/ui/sidecar_font28.c - Montserrat 28, CLASSIC's big face at 1280x720 (sidecar_theme.c): LVGL's own table compiled
// here, so that Circle's lv_conf.h and every built liblvgl.a stay as they are (28 is off there).
#include "lvgl.h"
#undef LV_FONT_MONTSERRAT_28
#define LV_FONT_MONTSERRAT_28 1
#include "src/font/lv_font_montserrat_28.c"

# pi/ui/host/Makefile - sidecar_ui on the Mac (shot.c): LVGL from pi/circle/build/lvgl-src (pi/ui/fetch-lvgl.sh),
# with Circle's lv_conf.h (addon/lvgl) minus its halt() assert. make -> shot.png (sips). Objects in obj/.
B    ?= ../../circle/build
LVGL ?= $(B)/lvgl-src
LVCONF ?= $(B)/circle-engine/addon/lvgl/lv_conf.h
SRC  := $(shell find $(LVGL)/src -name '*.c')
OBJ  := $(patsubst $(LVGL)/%.c,obj/%.o,$(SRC))
CFLAGS = -O1 -w -I obj -I $(LVGL) -DLV_CONF_INCLUDE_SIMPLE

# shot.png: the home page; shot-sound.png, shot-fx.png, shot-about.png: the settings pages. ./shot also prints the
# commands that taps on the controls send.
shot.png: shot
	./shot obj/shot
	sips -s format png obj/shot0.ppm --out shot.png >/dev/null
	sips -s format png obj/shot1.ppm --out shot-sound.png >/dev/null
	sips -s format png obj/shot2.ppm --out shot-fx.png >/dev/null
	sips -s format png obj/shot3.ppm --out shot-about.png >/dev/null
	sips -s format png obj/shot-hidden.ppm --out shot-hidden.png >/dev/null
	sips -s format png obj/shot-about-hidden.ppm --out shot-about-hidden.png >/dev/null
# splash.png: the boot screen (sidecar_splash.c), no LVGL
splash.png: ../sidecar_splash.c ../sidecar_splash.h ../sidecar_wordmark.c splash.c
	@mkdir -p obj
	@$(CC) -O1 -Wall -o obj/splash splash.c ../sidecar_splash.c ../sidecar_wordmark.c
	obj/splash obj/splash.ppm
	sips -s format png obj/splash.ppm --out splash.png >/dev/null
shot: shot.c ../sidecar_ui.c ../sidecar_wordmark.c $(OBJ) obj/lv_conf.h
	@$(CC) $(CFLAGS) -o $@ shot.c ../sidecar_ui.c ../sidecar_wordmark.c $(OBJ)
obj/lv_conf.h: $(LVCONF)
	@mkdir -p obj
	sed -e 's|<circle/startup.h>|<stdlib.h>|' -e 's|halt();|abort();|' $< > $@
obj/%.o: $(LVGL)/%.c obj/lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c -o $@ $<
clean:
	rm -rf obj shot shot*.png

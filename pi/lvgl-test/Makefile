# pi/lvgl-test/Makefile - the display test kernel (docs/pi-display.md). Built by ./build.sh in Docker against the
# engine's circle-engine tree with LVGL added (build.sh); flashed by
# CIRCLE_TREE=circle-engine ../circle/flash.sh ../lvgl-test.
CIRCLEHOME ?= ../circle/build/circle-engine
LVGLDIR := $(CIRCLEHOME)/addon/lvgl/lvgl

OBJS = main.o kernel.o ../ui/sidecar_display.o ../ui/sidecar_ui.o ../ui/sidecar_wordmark.o

EXTRAINCLUDE += -I ../ui -I $(LVGLDIR)

LIBS = $(CIRCLEHOME)/addon/lvgl/liblvgl.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)

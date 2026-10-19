# spike2: USB host stub for the Analog Rytm MKII (VID 0x1935 PID 0x0b48), Overbridge alt 3.
# Real Pi 4 only (QEMU has no xHCI/VL805). Run-time options in cmdline.txt: see rytmobdevice.h.
# Needs two Circle patches: circle-usbdevicefactory.patch (factory hook) and
# circle-xhci-urbqueue.patch (16 URBs per xHCI endpoint, event hook)
# (build.sh applies them to build/circle and rebuilds lib/usb).
CIRCLEHOME ?= ../build/circle

OBJS	= main.o kernel.o rytmobdevice.o

# ob_block.h: the block format shared with the Teensy host (teensy/libraries/RytmHost)
EXTRAINCLUDE += -I ../../../teensy/libraries/RytmHost

LIBS	= $(CIRCLEHOME)/lib/sched/libsched.a \
	  $(CIRCLEHOME)/lib/sound/libsound.a \
	  $(CIRCLEHOME)/lib/usb/libusb.a \
	  $(CIRCLEHOME)/lib/input/libinput.a \
	  $(CIRCLEHOME)/lib/fs/libfs.a \
	  $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)

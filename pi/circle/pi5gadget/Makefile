# pi/circle/pi5gadget/Makefile - the Pi 5 USB-C bring-up kernel (main.cpp, kernel.cpp). Built by ../build-pi5gadget.sh.
OBJS = main.o kernel.o
# the console on GPIO14/15 (pins 8/10), as the engine's
DEFINE += -DSERIAL_DEVICE_DEFAULT=0
LIBS = $(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/sched/libsched.a \
       $(CIRCLEHOME)/lib/libcircle.a
include $(CIRCLEHOME)/Rules.mk
-include $(DEPS)

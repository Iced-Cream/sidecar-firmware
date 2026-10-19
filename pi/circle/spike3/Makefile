# spike3: spike2's Overbridge host plus one Plaits voice on MAIN L/R, played by the Rytm's USB MIDI
# notes. Real Pi 4 only. Run-time options in cmdline.txt: rytmobdevice.h and synth.h.
# Needs spike2's two Circle patches (build.sh applies them) and ./setup_plaits.sh (the Teensy's
# Plaits sources, copied into plaits/, git-ignored).
CIRCLEHOME ?= ../build/circle

PLAITS_SRC := $(shell cd plaits 2>/dev/null && find plaits stmlib -name '*.cc' | sort)
PLAITS_OBJS := $(addprefix plaits-obj/,$(PLAITS_SRC:.cc=.o))

OBJS	= main.o kernel.o rytmobdevice.o synth.o plaitsvoice.o $(PLAITS_OBJS)

# ob_block.h: the block format shared with the Teensy host (teensy/libraries/RytmHost)
EXTRAINCLUDE += -I ../../../teensy/libraries/RytmHost

LIBS	= $(CIRCLEHOME)/lib/sched/libsched.a \
	  $(CIRCLEHOME)/lib/sound/libsound.a \
	  $(CIRCLEHOME)/lib/usb/libusb.a \
	  $(CIRCLEHOME)/lib/input/libinput.a \
	  $(CIRCLEHOME)/lib/fs/libfs.a \
	  $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/Rules.mk

# Plaits (and plaitsvoice.cpp, its wrapper) build as ordinary hosted C++: the toolchain's libstdc++
# headers (<algorithm> for std::min/max/copy/fill/sort, header-only), newlib's <math.h>, and
# Rules.mk's libm.a (STDLIB_SUPPORT=1 links the toolchain's newlib libm). Not Circle's flags:
# -nostdinc++ / -ffreestanding / -fno-builtin would hide <algorithm> and turn fabsf into calls.
# -DTEST: stmlib's portable paths (no Cortex-M ssat/usat/vsqrt asm, no .ramtext section).
PLAITS_CPPFLAGS = $(ARCH) -O2 -std=gnu++17 -fsigned-char -fno-exceptions -fno-rtti \
	-fno-threadsafe-statics -fno-math-errno -ffunction-sections -fdata-sections -g \
	-DTEST -Wno-unused-local-typedefs -I plaits

plaits-obj/%.o: plaits/%.cc
	@mkdir -p $(dir $@)
	@echo "  CPP   $@"
	@$(CPP) $(PLAITS_CPPFLAGS) -c -o $@ $<

plaitsvoice.o: plaitsvoice.cpp plaitsvoice.h
	@echo "  CPP   $@ (Plaits flags)"
	@$(CPP) $(PLAITS_CPPFLAGS) -c -o $@ $<

plaitsvoice.d:
	@:

EXTRACLEAN += -r plaits-obj

-include $(filter-out plaitsvoice.d plaits-obj/%,$(DEPS))

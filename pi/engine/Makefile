# pi/engine/Makefile - the Pi engine (docs/pi-engine.md): a Circle kernel (RASPPI=4, AArch64, multi-core) around the
# engine core (core/), generic services only since the strip-down of 2026-11-04 (docs/pi-engine.md "Generic engine").
# Built by ./build.sh in Docker; flashed by CIRCLE_TREE=circle-engine ../circle/flash.sh ../engine.
CIRCLEHOME ?= ../circle/build/circle-engine
# the tree's RASPPI (4: circle-engine, kernel8-rpi4.img; 5: circle-engine-pi5, kernel_2712.img, docs/pi5.md), read here
# for the choices below; Rules.mk includes it again
-include $(CIRCLEHOME)/Config.mk
CORE := ../../core

# Circle-side objects: Circle's flags and headers (Rules.mk's %.o: %.cpp)
OBJS = main.o kernel.o platform_pi.o shared.o ob_usb.o cores.o console.o guard.o tcp_link.o $(GADGET_OBJS) web.o sdfiles.o wifi.o
OBJS += paircode_pi.o          # the pairing code's Pi side (paircode_pi.h; the portable paircode.cpp is in PORT_SRC)
OBJS += midibridge_pi.o        # the Rytm MIDI bridge (midibridge_pi.h; the portable part is the header midibridge.h)
# the volumes besides the card and the storage bench (docs/STORAGE.md): Circle's side; the logic is hosted (PORT_SRC)
OBJS += stor_pi.o
OBJS += blackbox_pi.o          # the black box: the log to /.rytm/blackbox.log (blackbox.h)
# FatFs's disk layer in place of Circle's addon/fatfs/diskio.cpp (linked before libfatfs.a, so the archive's diskio.o
# is never pulled in): the USB drives' transfers kept to the 64 KB TRB rule below FatFs (stor_diskio.cpp)
OBJS += stor_diskio.o
OBJS += code_mmu.o            # shipped code: the code arena's pages (code_mmu.h, core/code)
GADGET_OBJS = gadget.o sidecar_gadget.o
ifeq ($(strip $(RASPPI)),5)
# Pi 5: Circle builds its DWC2 gadget for the Pi <= 4 only (lib/usb/gadget/Makefile; docs/pi5.md), so the USB-C Sidecar and
# P1's MIDI echo are compiled out (gadget.h, sidecar_gadget.h: stand-ins that are never started), unless PI5_GADGET=1
# (build.sh: a Circle tree with circle-pi5-gadget.patch). The serial console: PI5_UART (build.sh, default 0: GPIO14/15,
# pins 8/10, as on the Pi 4; 10: the 3-pin debug connector, Circle's default)
DEFINE += -DSERIAL_DEVICE_DEFAULT=$(or $(PI5_UART),0)
ifneq ($(PI5_GADGET),1)
DEFINE += -DSIDECAR_NO_GADGET
GADGET_OBJS =
else
# the test kernels' testcard=1 (card_guard.h, docs/pi5.md "Engine test mode"): FatFs's calls that change a name go
# through card_guard.cpp, the libraries' too. Nothing here in a kernel that can be released.
OBJS += card_guard.o
LDFLAGS += --wrap=f_open --wrap=f_unlink --wrap=f_rename --wrap=f_mkdir
endif
endif

# Hosted objects: the toolchain's C++ headers, not Circle's: the core and the engine's portable parts. Named *.cpp.o
# under hosted/. The core (core/generic/generic.h): the host link's router, the remote screen's transport, the Rytm's
# clock, the processor slots, the device hello, the card's atomic writes and the RAM store, the card queue, 51 STORE
# and the Pi-side locks, the card browse, the USB-A MIDI devices; sha256 for the pairing code and the file service.
CORE_SRC := $(addprefix $(CORE)/,generic/generic.cpp screen/screen.cpp clock/clock.cpp proc/proc.cpp hooks.cpp ui/canvas.cpp \
            msamp/cardq.cpp msamp/sfz.cpp msamp/elmulti.cpp wavetable/tables.cpp lib/sha256.cpp device/device.cpp device/digitakt.cpp browse/browse.cpp devmidi/devmidi.cpp)
CORE_SRC += $(wildcard $(CORE)/fmt/*.cpp) $(wildcard $(CORE)/store/*.cpp) $(wildcard $(CORE)/kv/*.cpp) $(wildcard $(CORE)/locks/*.cpp) $(CORE)/route/route.cpp $(CORE)/dsp/dsp.cpp $(CORE)/samples/samples.cpp $(CORE)/code/loader.cpp $(CORE)/code/code.cpp
PORT_SRC := midi_in.cpp bridge.cpp emu_proto.cpp tones.cpp fsweb.cpp wifi_conf.cpp paircode.cpp power_state.cpp \
            stor_vol.cpp stor_bench.cpp audio_return.cpp
HOSTED_OBJS := $(patsubst $(CORE)/%,hosted/core/%.o,$(CORE_SRC)) $(patsubst %,hosted/%.o,$(PORT_SRC))
OBJS += $(HOSTED_OBJS)

EXTRAINCLUDE += -I . -I $(CIRCLEHOME)/addon -I $(CORE)

LIBS = $(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
       $(CIRCLEHOME)/addon/wlan/libwlan.a \
       $(if $(filter 5,$(strip $(RASPPI))),$(CIRCLEHOME)/addon/nvme/libnvme.a,) \
       $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
       $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
       $(CIRCLEHOME)/lib/net/libnet.a \
       $(CIRCLEHOME)/lib/sched/libsched.a \
       $(if $(GADGET_OBJS),$(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a,) \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/Rules.mk

HOSTED_FLAGS = $(ARCH) -O2 -std=gnu++17 -fsigned-char -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-fno-math-errno -ffp-contract=fast -ffunction-sections -fdata-sections -g -MMD -MP -I. -I$(CORE)

hosted/core/%.o: $(CORE)/%
	@mkdir -p $(dir $@)
	@echo "  CPP   $@ (hosted)"
	@$(CPP) $(HOSTED_FLAGS) -c -o $@ $<
hosted/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CPP   $@ (hosted)"
	@$(CPP) $(HOSTED_FLAGS) -c -o $@ $<

EXTRACLEAN += -r hosted

# Circle's objects: Rules.mk's %.d rules make their .d files when included (as spike3). Hosted objects: -MMD -MP
# writes hosted/**/*.d beside them during the compile; include the ones that exist.
-include $(filter-out hosted/%,$(DEPS))
-include $(wildcard $(HOSTED_OBJS:.o=.d))

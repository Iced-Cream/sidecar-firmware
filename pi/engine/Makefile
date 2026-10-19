# pi/engine/Makefile - the Pi engine (docs/pi-engine.md): a Circle kernel (RASPPI=4, AArch64, multi-core) around the
# engine core (core/) and the Teensy's curated engines (engines/: core/engines/setup_engines.sh, git-ignored).
# Built by ./build.sh in Docker; flashed by CIRCLE_TREE=circle-engine ../circle/flash.sh ../engine.
CIRCLEHOME ?= ../circle/build/circle-engine
CORE := ../../core
ENG  := engines
include $(CORE)/engines/engines.mk

# Circle-side objects: Circle's flags and headers (Rules.mk's %.o: %.cpp)
OBJS = main.o kernel.o platform_pi.o shared.o ob_usb.o cores.o console.o selftest.o guard.o tcp_link.o gadget.o sidecar_gadget.o web.o sdfiles.o um_pi.o wifi.o rec_pi.o
# the HDMI display (docs/pi-display.md): pi/ui, LVGL from the tree's addon/lvgl (build.sh adds it)
OBJS += ../ui/sidecar_display.o ../ui/sidecar_ui.o ../ui/sidecar_splash.o ../ui/sidecar_wordmark.o

# Hosted objects: the toolchain's C++ headers and newlib's libm, not Circle's (as spike3's Plaits): the core,
# the engines and the engine's portable parts. Named *.cpp.o / *.cc.o / *.c.o under hosted/.
# core/host/newlib_rand.cpp: DAISYSP's rand() with newlib's sequence, without newlib's libc (the one host file here).
CORE_SRC := $(addprefix $(CORE)/,lfo.cpp hooks.cpp notes.cpp voices.cpp sysex.cpp host_link.cpp slots.cpp engine.cpp fx/send_fx.cpp fx/send_fx_card.cpp \
            ui/canvas.cpp ui/widgets.cpp ui/pages.cpp ui/remote.cpp \
            pool/card_index.cpp pool/pool.cpp pool/sample_ram_pool.cpp presets/libs.cpp host/newlib_rand.cpp user/um.cpp user/lists.cpp)
CORE_SRC += $(wildcard $(CORE)/ui/page_*.cpp)   # the remote UI's pages drawn by code (core/ui/extra.h)
CORE_SRC += $(CORE)/rec/rec_names.cpp   # the recorder's sources, channel lists and take names (shared with the Sidecar Mini)
# the plugin loader's portable parts (teensy/libraries/RytmPlugins/src: verify, place, manifest, generations, the slot
# messages, the slot map), hosted: user machines on the Pi (core/user/um.cpp, docs/user-machines-pi.md)
RMPSRC := ../../teensy/libraries/RytmPlugins/src
RMP_SRC := rmp_loader.cpp rmp_manifest.cpp rmp_meta.cpp rmp_proto.cpp rmp_resolve.cpp rmp_slots.cpp rmp_presets.cpp rmp_pool.cpp
PORT_SRC := output.cpp midi_in.cpp bridge.cpp emu_proto.cpp tones.cpp sendfx_pi.cpp fsweb.cpp recorder.cpp rec_play.cpp wifi_conf.cpp
HOSTED_OBJS := $(patsubst $(CORE)/%,hosted/core/%.o,$(CORE_SRC)) $(patsubst %,hosted/%.o,$(PORT_SRC)) \
               $(patsubst %,hosted/rmp/%.o,$(RMP_SRC)) \
               $(patsubst %,hosted/%.o,$(ENG_SRC) $(ENG_C_SRC))
OBJS += $(HOSTED_OBJS)

EXTRAINCLUDE += -I . -I ../../teensy/libraries/RytmHost -I $(CIRCLEHOME)/addon -I $(CORE) -I $(RMPSRC)
EXTRAINCLUDE += -I ../ui -I $(CIRCLEHOME)/addon/lvgl/lvgl

LIBS = $(CIRCLEHOME)/addon/lvgl/liblvgl.a \
       $(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
       $(CIRCLEHOME)/addon/wlan/libwlan.a \
       $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
       $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
       $(CIRCLEHOME)/lib/net/libnet.a \
       $(CIRCLEHOME)/lib/sched/libsched.a \
       $(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/Rules.mk

# RYTM_RENDER_CORES=4: the render state voices share is per core (cores 1 and 2 render at once; rytm_render_core() in
# platform_pi.cpp): a 6-OP FM reload's stage and bank pointer, GRAIN's budget, stmlib's Random and rand(). Every
# hosted object needs the same value (the per-core arrays are inline functions' statics).
HOSTED_FLAGS = $(ARCH) -DRYTM_RENDER_CORES=4 -O2 -std=gnu++17 -fsigned-char -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-fno-math-errno -ffp-contract=fast -ffunction-sections -fdata-sections -g -MMD -MP -I. -I$(CORE) -I$(RMPSRC) -DRMP_HOST_A64 $(ENG_INC) $(ENG_FLAGS)
HOSTED_CFLAGS = $(ARCH) -O2 -std=c99 -fno-math-errno -ffp-contract=fast -g -MMD -MP -w $(ENG_INC)

hosted/core/%.o: $(CORE)/%
	@mkdir -p $(dir $@)
	@echo "  CPP   $@ (hosted)"
	@$(CPP) $(HOSTED_FLAGS) -c -o $@ $<
hosted/rmp/%.cpp.o: $(RMPSRC)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CPP   $@ (hosted)"
	@$(CPP) $(HOSTED_FLAGS) -include core_port.h -DRMP_SNPRINTF=sniprintf -c -o $@ $<
hosted/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CPP   $@ (hosted)"
	@$(CPP) $(HOSTED_FLAGS) -c -o $@ $<
hosted/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CPP   $@ (hosted)"
	@$(CPP) $(HOSTED_FLAGS) -w -c -o $@ $<
hosted/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC    $@ (hosted)"
	@$(CC) $(HOSTED_CFLAGS) -c -o $@ $<

EXTRACLEAN += -r hosted

# Circle's objects: Rules.mk's %.d rules make their .d files when included (as spike3). Hosted objects: -MMD -MP
# writes hosted/**/*.d beside them during the compile; include the ones that exist.
-include $(filter-out hosted/%,$(DEPS))
-include $(wildcard $(HOSTED_OBJS:.o=.d))

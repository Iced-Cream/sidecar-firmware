# plugins/noiseplethora/Makefile - Noise Plethora algorithms as machine packages.
# ./fetch.sh first (vendor/np, git-ignored). The plugin headers are copied from
# vendor/np into build/np/ without NP's framework (Plugin.hpp is shim/'s); the
# Teensy Audio objects are compiled from the installed Teensyduino (MIT), unchanged.
#   make [PKG=atari] np_atari.rmp libnp_atari.dylib   - Atari alone (np_single.cpp)
#   make PKG=ring np_ring.rmp libnp_ring.dylib         - a package of banks.h (np_bank.cpp)
#   make packages                                      - the eight small packages and Atari, .rmp and .dylib
#   make packages-a64                                  - the same for the Pi (np_<pkg>.a64.rmp)
#   make legacy                                        - the first grouping A, B, C (15-23 KB each)
PKG ?= atari
NP_SMALL := prime fibo phase fm pw ring noise walk
NPDIR := vendor/np
AUDIO := $(shell ls -d $(HOME)/Library/Arduino15/packages/teensy/hardware/avr/*/libraries/Audio 2>/dev/null | sort -V | tail -1)
CORE := $(abspath $(AUDIO)/../../cores/teensy4)
SHIMA := ../../sdk/rmp/shim/audio
BUILD := build/$(PKG)
ifeq ($(PKG),atari)
NAME := np_atari
NP_BLOCK ?= 128
# Atari's graph holds at most 2 blocks at once (measured)
NP_POOL := 4
NP_PERMILLE := 60
NP_CLASSES := Atari
AUDIO_OBJS := synth_waveform.cpp data_waveforms.c data_bandlimit_step.c
SRCS := np_single.cpp AudioStream.cpp $(AUDIO_OBJS)
PKG_FLAGS := -DNP_HEADER='"P_Atari.hpp"' -DNP_PLUGIN=Atari -DNP_PERMILLE=$(NP_PERMILLE)
else ifneq ($(shell grep -c '^.define NP_PKG_$(shell echo $(PKG) | tr a-z A-Z)(X) ' banks.h),0)
PKGU := $(shell echo $(PKG) | tr a-z A-Z)
NAME := np_$(PKG)
MANIFEST := $(PKG).json
# per package: block size (the one each algorithm had in A, B, C: the clusters 16, banks 1 and 3 128),
# and the pool (most blocks a member's graph held at once, measured, + margin; 20 unless set)
NP_BLOCK_prime := 16
NP_BLOCK_fibo := 16
NP_BLOCK_phase := 16
NP_BLOCK_fm := 16
NP_BLOCK_pw := 16
NP_BLOCK_ring := 128
NP_BLOCK_noise := 128
NP_BLOCK_walk := 128
NP_BLOCK_a := 16
NP_BLOCK_b := 16
NP_BLOCK_c := 128
# declared CPU per voice (cpu_permille: the guard limit of a render call is 2 x permille x 150 + 2000
# cycles), at least twice the longest call measured with 8 voices (docs/plugins.md 9.5; a C call of
# 79464 cycles was seen once, 1.8 x its 44872 worst, against the old 63200 limit):
# prime 2 x 16068 (PrimeCluster) -> 100, phase 2 x 20088 (phasingCluster) -> 130, the 128-sample
# packages 400 (limit 122000 = 1.5 x that spike); fibo, fm, pw keep A's and B's 62 / 76 (2 x their worst fits)
NP_PERMILLE_prime := 100
NP_PERMILLE_fibo := 62
NP_PERMILLE_phase := 130
NP_PERMILLE_fm := 76
NP_PERMILLE_pw := 76
NP_PERMILLE_ring := 400
NP_PERMILLE_noise := 400
NP_PERMILLE_walk := 400
NP_PERMILLE_a := 100
NP_PERMILLE_b := 130
NP_PERMILLE_c := 400
NP_PERMILLE ?= $(NP_PERMILLE_$(PKG))
NP_BLOCK ?= $(NP_BLOCK_$(PKG))
NP_POOL := $(or $(NP_POOL_$(PKG)),20)
NP_CLASSES := $(shell sed -n 's/^.define NP_PKG_$(PKGU).X. //p' banks.h | tr ' ' '\n' | sed -n 's/^X.\([A-Za-z0-9_]*\),$$/\1/p')
# every object Audio.h offers: --gc-sections keeps only what the members use
AUDIO_OBJS := synth_waveform.cpp synth_sine.cpp synth_whitenoise.cpp synth_pinknoise.cpp synth_pwm.cpp synth_dc.cpp \
  mixer.cpp filter_variable.cpp effect_multiply.cpp effect_wavefolder.cpp data_waveforms.c data_bandlimit_step.c
SRCS := np_bank.cpp AudioStream.cpp $(AUDIO_OBJS)
PKG_FLAGS := -DNP_LIST=NP_PKG_$(PKGU) -DNP_PERMILLE=$(NP_PERMILLE) -I$(BUILD)   # np_members.h: the members' headers (generated)
else
$(error PKG=$(PKG): atari or a package of banks.h ($(NP_SMALL), a, b, c))
endif
vpath %.cpp $(SHIMA) $(AUDIO)
vpath %.c $(AUDIO)
# CORE after the system headers: only for <arm_math.h> (CMSIS) on the Teensy, via the shim's include_next
INCS := -Ishim -Ibuild/np -I$(SHIMA) -I$(AUDIO) -idirafter $(CORE)
# Block size (samples per graph pass, NP_BLOCK): 16, or 128 (NP's own) for a package with a
# feedback-loop plugin (radioOhNo, xModRingSqr, XModRingSine, CrossModRing, Atari): the loop's
# one-block delay shapes their sound. A multiple of 16 from 16 to 128. Atari and C: 128.
ifneq ($(filter $(NP_BLOCK),16 32 48 64 80 96 112 128),$(NP_BLOCK))
$(error NP_BLOCK=$(NP_BLOCK): a multiple of 16 from 16 to 128)
endif
CFLAGS_EXTRA := -DAUDIO_BLOCK_SAMPLES=$(NP_BLOCK) -DAUDIO_SAMPLE_RATE_EXACT=48000.0f -DAUDIO_GRAPH_BLOCKS=$(NP_POOL) \
  -include $(SHIMA)/Arduino.h $(PKG_FLAGS)
# the Pi's build runs in the rytm-circle image when there is no local aarch64-none-elf-gcc: Teensyduino's
# Audio library and core (outside the repository) mounted read-only, the package named
A64_DOCKER_ARGS := -v $(abspath $(AUDIO)/../..):$(abspath $(AUDIO)/../..):ro
A64_MAKE_ARGS := PKG=$(PKG) AUDIO=$(AUDIO)
include ../../sdk/rmp/plugin.mk
# builtins back on for the C++ (plugin.mk turns them off): NP's pow(x, 2) is then x * x instead of
# libm's double pow (7.8 KB with its tables), and fixed-size memcpy is inlined
ARMCXX += -fbuiltin
# The NP plugin classes (constructors, destructors, init, process: run at an ALG change or a params()
# call, not per sample) and the glue are compiled -Os on the Teensy: a third to a half of their code
# (the constructors inline every Audio object's and every AudioConnection's). The Audio objects' update()
# (the per-sample work) stays -O2. The output is bit-identical to -O2 (an ARM emulation of every
# algorithm, docs/plugins.md 10); shim/Plugin.hpp keeps pow(x, 2) a multiply at -Os.
NP_COLD := -Os
$(BUILD)/np_bank.o $(BUILD)/np_single.o: ARMFLAGS += $(NP_COLD)

# The plugin header and any other NP plugin header it includes (P_*.hpp, GPL) are copied;
# its "Plugin.hpp" resolves to shim/. Any other NP file (the CC BY-NC-SA framework) is refused.
build/np/P_%.hpp: $(NPDIR)/P_%.hpp
	@mkdir -p $(dir $@)
	@for h in $$(sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*"\([^"]*\)".*/\1/p' $<); do \
	  case $$h in Plugin.hpp) ;; \
	    P_*.hpp) $(MAKE) --no-print-directory build/np/$$h || exit 1 ;; \
	    *) echo "$<: includes $$h, NP framework code - not used" >&2; exit 1 ;; \
	  esac; done
	cp $< $@
$(BUILD)/np_single.o $(BUILD)/native/np_single.o $(BUILD)/np_bank.o $(BUILD)/native/np_bank.o: \
  $(foreach c,$(NP_CLASSES),build/np/P_$(c).hpp) $(if $(PKGU),$(BUILD)/np_members.h)
$(BUILD)/np_members.h: banks.h Makefile
	@mkdir -p $(BUILD)
	printf '#include "P_%s.hpp"\n' $(NP_CLASSES) > $@
# The package's configuration (block size, pool, permille, flags) in build/<pkg>/cfg.txt: when it
# differs from the current one, the whole build/<pkg>/ and the outputs go before anything is made
# (a stale object with another AUDIO_GRAPH_BLOCKS or AUDIO_BLOCK_SAMPLES would corrupt memory; a
# deletion does not depend on make 3.81's 1-second timestamps). Objects also depend on this Makefile.
CFG_NOW := $(NP_BLOCK) $(NP_POOL) $(NP_PERMILLE) $(NP_COLD) $(subst ',,$(PKG_FLAGS))
CFG_OLD := $(shell cat $(BUILD)/cfg.txt 2>/dev/null)
ifneq ($(CFG_NOW),$(CFG_OLD))
$(shell rm -rf $(BUILD) $(NAME).elf $(NAME).rmp lib$(NAME).dylib; mkdir -p $(BUILD); printf '%s\n' '$(CFG_NOW)' > $(BUILD)/cfg.txt)
endif
$(OBJS) $(NOBJS): Makefile $(BUILD)/cfg.txt

packages:
	for p in atari $(NP_SMALL); do $(MAKE) --no-print-directory PKG=$$p np_$$p.rmp libnp_$$p.dylib || exit 1; done
# the same set for the Pi (RMPA, np_<pkg>.a64.rmp; -O2 throughout, the Pi has no arena shortage)
packages-a64:
	for p in atari $(NP_SMALL); do $(MAKE) --no-print-directory PKG=$$p a64 || exit 1; done
legacy:
	for p in a b c; do $(MAKE) --no-print-directory PKG=$$p np_$$p.rmp libnp_$$p.dylib || exit 1; done
clean-all:
	rm -rf build np_*.elf np_*.rmp libnp_*.dylib
.PHONY: packages packages-a64 legacy clean-all

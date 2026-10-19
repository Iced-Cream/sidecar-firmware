# plugin.mk - include from a plugin's Makefile after setting NAME, SRCS (C/C++),
# and optionally CFLAGS_EXTRA / INCS, MANIFEST (default manifest.json) and BUILD (the object
# directory, default build; one per package when a Makefile builds several). Targets: $(NAME).rmp
# (Teensy), lib$(NAME).dylib (Mac), and `make a64`: $(NAME).a64.rmp (the Pi, an RMPA package: the same
# sources built for AArch64 with aarch64-none-elf-gcc, from PATH or else in the rytm-circle Docker image that
# pi/engine/build.sh uses; docs/user-machines-pi.md).
ifeq ($(.DEFAULT_GOAL),)
.DEFAULT_GOAL := all   # a plain `make` builds the package and the Mac library (unless the Makefile named a goal first)
endif
MANIFEST ?= manifest.json
BUILD ?= build
SDK := $(dir $(lastword $(MAKEFILE_LIST)))
# Teensyduino's compiler: the newest installed teensy-compile (sort -V), else the one this was built with
ARMBIN ?= $(or $(shell ls -d $(HOME)/Library/Arduino15/packages/teensy/tools/teensy-compile/*/arm/bin 2>/dev/null | sort -V | tail -1),$(HOME)/Library/Arduino15/packages/teensy/tools/teensy-compile/15.2.1/arm/bin)
ACC := $(ARMBIN)/arm-none-eabi-gcc
ACXX := $(ARMBIN)/arm-none-eabi-g++
ARMFLAGS := -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 -O2 \
  -ffunction-sections -fdata-sections -fno-common -mword-relocations \
  -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-math-errno \
  -ffreestanding -I$(SDK).. -I$(SDK) $(INCS) $(CFLAGS_EXTRA)
ARMCXX := -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit   # C++ is compiled without -ffreestanding (libstdc++ <math.h>/<cmath> refuse it); -fno-builtin keeps mem* from being recognised
SHIM := $(SDK)shim/rmp_libc.c
PY ?= ../../.venv/bin/python

# Each source is compiled with its own driver (C as C, C++ as C++), then linked with g++.
OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(notdir $(basename $(SRCS) $(SHIM)))))
vpath %.c $(SDK)shim
vpath %.cpp $(SDK)shim
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(ACC) $(ARMFLAGS) -fno-tree-loop-distribute-patterns -MMD -MP -c -o $@ $<
$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(ACXX) $(filter-out -ffreestanding,$(ARMFLAGS)) -fno-builtin $(ARMCXX) -MMD -MP -c -o $@ $<
$(BUILD)/%.o: %.cc
	@mkdir -p $(dir $@)
	$(ACXX) $(filter-out -ffreestanding,$(ARMFLAGS)) -fno-builtin $(ARMCXX) -MMD -MP -c -o $@ $<

# rmp_cxx.cpp (operator delete stubs) is always linked: it is tiny, and gc-sections drops it when unused.
CXXSHIM := $(SDK)shim/rmp_cxx.cpp
OBJS += $(BUILD)/rmp_cxx.o
$(NAME).elf: $(OBJS)
	$(ACXX) $(ARMFLAGS) -nostdlib -nostartfiles -Wl,--emit-relocs -Wl,--gc-sections \
	  -Wl,-T,$(SDK)plugin.ld -o $@ $(OBJS) -lm -lgcc

# the packer and the header it reads (RMP_API) are prerequisites: a packer change repacks every plugin
PACK := $(SDK)../../tools/rmp_pack.py
$(NAME).rmp: $(NAME).elf $(MANIFEST) $(PACK) $(SDK)rmp_api.h
	$(PY) $(PACK) $(NAME).elf $(MANIFEST) $@

NOBJS := $(addprefix $(BUILD)/native/,$(addsuffix .o,$(notdir $(basename $(SRCS)))))
NFLAGS := -O2 -fPIC -DRMP_NATIVE -I$(SDK).. -I$(SDK) $(INCS) $(CFLAGS_EXTRA)
$(BUILD)/native/%.o: %.c
	@mkdir -p $(dir $@)
	cc -std=c11 $(NFLAGS) -MMD -MP -c -o $@ $<
$(BUILD)/native/%.o: %.cpp
	@mkdir -p $(dir $@)
	c++ -std=c++17 $(NFLAGS) -MMD -MP -c -o $@ $<
$(BUILD)/native/%.o: %.cc
	@mkdir -p $(dir $@)
	c++ -std=c++17 $(NFLAGS) -MMD -MP -c -o $@ $<
lib$(NAME).dylib: $(NOBJS)
	c++ -shared -o $@ $(NOBJS)

-include $(OBJS:.o=.d) $(NOBJS:.o=.d)

# ---- AArch64 (the Pi): the same image layout (plugin.ld), no PIC (ADRP/:lo12: are page-relative: the loader places
# the image 4096-aligned), x18 left alone (the arm64 Mac's platform register: the Mac's core lib runs these images
# too), no outline atomics (no libgcc helpers the image cannot hold).
A64PREFIX ?= aarch64-none-elf-
A64LOCAL := $(shell command -v $(A64PREFIX)gcc 2>/dev/null)
A64FLAGS := -mcpu=cortex-a72 -O2 -fno-pic -fno-pie -mcmodel=small -ffixed-x18 -mno-outline-atomics \
  -ffunction-sections -fdata-sections -fno-common -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
  -fno-math-errno -I$(SDK).. -I$(SDK) $(INCS) $(CFLAGS_EXTRA)
AOBJS := $(addprefix $(BUILD)/a64/,$(addsuffix .o,$(notdir $(basename $(SRCS) $(SHIM) $(CXXSHIM)))))
$(BUILD)/a64/%.o: %.c
	@mkdir -p $(dir $@)
	$(A64PREFIX)gcc $(A64FLAGS) -ffreestanding -fno-tree-loop-distribute-patterns -MMD -MP -c -o $@ $<
$(BUILD)/a64/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(A64PREFIX)g++ $(A64FLAGS) -fno-builtin $(ARMCXX) -MMD -MP -c -o $@ $<
$(BUILD)/a64/%.o: %.cc
	@mkdir -p $(dir $@)
	$(A64PREFIX)g++ $(A64FLAGS) -fno-builtin $(ARMCXX) -MMD -MP -c -o $@ $<
$(NAME).a64.elf: $(AOBJS)
	$(A64PREFIX)g++ $(A64FLAGS) -static -nostdlib -nostartfiles -Wl,--emit-relocs -Wl,--gc-sections -Wl,-z,max-page-size=4096 \
	  -Wl,--no-warn-rwx-segments -Wl,-T,$(SDK)plugin_a64.ld -o $@ $(AOBJS) -lm -lgcc
REPO_ROOT := $(abspath $(SDK)../..)
# A64_DOCKER_ARGS: more docker run arguments (mounts of sources outside the repository); A64_MAKE_ARGS: the
# variables the inner make needs (a Makefile that builds several packages passes its PKG=)
ifeq ($(A64LOCAL),)
a64-elf:
	docker run --rm -v $(REPO_ROOT):$(REPO_ROOT) $(A64_DOCKER_ARGS) -w $(CURDIR) rytm-circle make $(A64_MAKE_ARGS) $(NAME).a64.elf
else
a64-elf: $(NAME).a64.elf
endif
a64: a64-elf $(MANIFEST) $(PACK) $(SDK)rmp_api.h
	$(PY) $(PACK) $(NAME).a64.elf $(MANIFEST) $(NAME).a64.rmp

-include $(AOBJS:.o=.d)

all: $(NAME).rmp lib$(NAME).dylib
clean:
	rm -rf $(BUILD) $(NAME).elf $(NAME).rmp lib$(NAME).dylib $(NAME).a64.elf $(NAME).a64.rmp
.PHONY: all clean a64 a64-elf

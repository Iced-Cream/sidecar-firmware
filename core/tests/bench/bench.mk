# core/tests/bench/bench.mk - pi/engine/tests/bench_render.cpp built from the core's sources (make -C core -f tests/bench/bench.mk):
# TAG names the build (build/bench-TAG), PICK the compiler flags: mac (this Mac's clang as core/Makefile compiles) or
# pi (GCC with the Pi's flags, pi/engine/Makefile HOSTED_FLAGS: run by bench.sh in the rytm-circle image). OPT overrides -O2.
include Makefile
.DEFAULT_GOAL := $(B)/bench-$(or $(TAG),mac)/bench_render
TAG  ?= mac
PICK ?= mac
OPT  ?= -O2
HOTOPT ?= -O3
ifeq ($(PICK),pi)
HOT  ?= $(HOT_O3) $(addprefix $(ENG)/,$(HOT_O3_ENG))
endif
BB   := $(B)/bench-$(TAG)
ifeq ($(PICK),pi)
CC   := gcc
CXX  := g++
# the Pi's ARCH + HOSTED_FLAGS, as far as a hosted Linux build takes them (no RYTM_RENDER_CORES: this bench is one core)
PIF  := -mcpu=cortex-a72 -mlittle-endian $(OPT) -fsigned-char -fno-math-errno -ffp-contract=fast -ffunction-sections -fdata-sections -g
BXX  := -include string.h -include strings.h -include stdlib.h -include stdio.h -include math.h $(PIF) -std=gnu++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -DCORE_UI_TOOLS=1 -DCORE_POLY=1 -DCORE_MSAMP=1 -w -Itests/bench/shim -I. -Ihost -I$(RMPSRC) -I../sdk $(ENG_INC) $(ENG_FLAGS)
BC   := $(PIF) -std=c99 -w $(ENG_INC)
BLD  := -static -Wl,--gc-sections
else
BXX  := $(filter-out -O2 -Wall -Wextra,$(CXXFLAGS)) $(OPT) -w
BC   := $(filter-out -O2,$(CFLAGS)) $(OPT)
BLD  :=
endif
SRCS := $(CORE_SRC) $(ENG_SRC) $(ENG_C_SRC) ../pi/engine/bridge.cpp ../pi/engine/output.cpp ../pi/engine/neon_bench.cpp ../pi/engine/tests/bench_render.cpp
obj   = $(BB)/$(subst ../,up/,$(1)).o
OBJS := $(foreach s,$(SRCS),$(call obj,$(s)))
$(BB)/bench_render: $(OBJS)
	$(CXX) -o $@ $^ $(BLD) -lm -lpthread
define R
$(call obj,$(1)): $(1)
	@mkdir -p $$(dir $$@)
	@$$(if $$(filter %.c,$(1)),$(CC) $(BC),$(CXX) $(BXX) -I../pi/engine) -MMD -MP $$(if $$(filter $(1),$(HOT)),$(HOTOPT),) -c -o $$@ $(1)
endef
$(foreach s,$(SRCS),$(eval $(call R,$(s))))
-include $(OBJS:.o=.d)

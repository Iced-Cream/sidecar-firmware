# core/Makefile - the engine core on the Mac (docs/core.md).
#   make -C core engines   once, and after the Teensy's engine libraries change: the curated engine sources into
#                          build/engines (engines/setup_engines.sh) and build/libengines.a
#   make -C core test      build and run every tests/test_*.cpp, tests/mc/test_*.cpp (two render cores: the
#                          engines and the core again with RYTM_RENDER_CORES=3, in build/mc) and tests/small/test_*.cpp
#                          (the Teensy's send FX set, RYTM_SENDFX_LARGE=0, in build/small); the last line says
#                          ALL CORE TESTS PASS
#   make -C core lib       build/libcore.dylib for Python (sim/coreengine.py, sim/tests/test_core_*.py)
# version.h (VERSION, the commit): regenerated when it changed; kept out of DEPS_H so a new commit does not rebuild every test
$(shell ../tools/gen_version.sh)
CXX      ?= c++
CC       ?= cc
B        := build
ENG      := $(B)/engines
include engines/engines.mk
RMPSRC   := ../teensy/libraries/RytmPlugins/src
CXXFLAGS := -std=c++17 -O2 -DCORE_UI_TOOLS=1 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=fast -fPIC -I. -Ihost -I$(RMPSRC) -I../sdk $(ENG_INC) $(ENG_FLAGS)
CFLAGS   := -std=c99 -O2 -g -ffp-contract=fast -fPIC -w $(ENG_INC)
# The core's own sources (grows task by task) and the Mac port.
CORE_SRC := engine.cpp lfo.cpp hooks.cpp notes.cpp voices.cpp sysex.cpp host_link.cpp slots.cpp fx/send_fx.cpp fx/send_fx_card.cpp ui/canvas.cpp ui/widgets.cpp ui/pages.cpp ui/remote.cpp $(wildcard ui/page_*.cpp) $(wildcard rec/*.cpp) pool/card_index.cpp pool/pool.cpp pool/sample_ram_pool.cpp presets/libs.cpp host/platform_host.cpp host/newlib_rand.cpp \
            user/um.cpp user/lists.cpp host/um_host.cpp $(addprefix $(RMPSRC)/,rmp_loader.cpp rmp_manifest.cpp rmp_meta.cpp rmp_proto.cpp rmp_resolve.cpp rmp_slots.cpp rmp_presets.cpp rmp_pool.cpp)
TESTS    := $(patsubst tests/%.cpp,$(B)/%,$(wildcard tests/test_*.cpp))
ENG_OBJ  := $(patsubst $(ENG)/%,$(B)/eng-obj/%.o,$(ENG_SRC) $(ENG_C_SRC))
DEPS_H   := $(filter-out version.h,$(wildcard *.h */*.h host/*.h tests/*.h))
# Two render cores at once (the Pi engine's split) and a control core: engines and core built with RYTM_RENDER_CORES=3
MC       := $(B)/mc
MC_FLAGS := -DRYTM_RENDER_CORES=3
MC_OBJ   := $(patsubst $(ENG)/%,$(MC)/eng-obj/%.o,$(ENG_SRC) $(ENG_C_SRC))

.PHONY: engines test lib clean
engines:
	engines/setup_engines.sh $(abspath $(ENG))
	$(MAKE) $(B)/libengines.a

$(B)/libengines.a: $(ENG_OBJ)
	@test -d $(ENG)/Plaits || { echo "core: run 'make -C core engines' first"; exit 1; }
	rm -f $@ && ar rcs $@ $^

$(B)/eng-obj/%.cc.o: $(ENG)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -w -c -o $@ $<
$(B)/eng-obj/%.cpp.o: $(ENG)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -w -c -o $@ $<
$(MC)/eng-obj/%.cc.o: $(ENG)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(MC_FLAGS) -w -c -o $@ $<
$(MC)/eng-obj/%.cpp.o: $(ENG)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(MC_FLAGS) -w -c -o $@ $<
$(MC)/eng-obj/%.c.o: $(ENG)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(MC)/libengines.a: $(MC_OBJ)
	@test -d $(ENG)/Plaits || { echo "core: run 'make -C core engines' first"; exit 1; }
	rm -f $@ && ar rcs $@ $^
MC_TESTS := $(patsubst tests/mc/%.cpp,$(MC)/%,$(wildcard tests/mc/test_*.cpp))
$(MC)/test_%: tests/mc/test_%.cpp $(CORE_SRC) $(DEPS_H) $(MC)/libengines.a
	$(CXX) $(CXXFLAGS) $(MC_FLAGS) -pthread -o $@ $< $(CORE_SRC) $(MC)/libengines.a

$(B)/eng-obj/%.c.o: $(ENG)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# The Teensy's send FX set (RYTM_SENDFX_LARGE=0: CHORUS COMB CRUSH RINGMOD, 16-bit lines): tests/small, with the core
SMALL       := $(B)/small
SMALL_FLAGS := -DRYTM_SENDFX_LARGE=0
SMALL_TESTS := $(patsubst tests/small/%.cpp,$(SMALL)/%,$(wildcard tests/small/test_*.cpp))
$(SMALL)/test_%: tests/small/test_%.cpp $(CORE_SRC) $(DEPS_H) $(B)/libengines.a
	@mkdir -p $(SMALL)
	$(CXX) $(CXXFLAGS) $(SMALL_FLAGS) -Itests -o $@ $< $(CORE_SRC) $(B)/libengines.a

test: $(TESTS) $(MC_TESTS) $(SMALL_TESTS)
	@set -e; for t in $(TESTS) $(MC_TESTS) $(SMALL_TESTS); do ./$$t; done; echo "ALL CORE TESTS PASS"

$(B)/test_%: tests/test_%.cpp $(CORE_SRC) $(DEPS_H) $(B)/libengines.a
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -o $@ $< $(CORE_SRC) $(B)/libengines.a

lib: $(B)/libcore.dylib
$(B)/libcore.dylib: host/core_c.cpp $(CORE_SRC) $(DEPS_H) $(B)/libengines.a
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -shared -o $@ host/core_c.cpp $(CORE_SRC) $(B)/libengines.a

clean:
	rm -rf $(B)

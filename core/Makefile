# core/Makefile - the engine core on the host (docs/core.md): the generic services of the Pi engine (the strip-down of
# 2026-11-04; docs/pi-engine.md "Generic engine"), built and tested with the host's compiler (macOS or Linux).
#   make -C core test        build and run every tests/test_*.cpp against the core's sources; the last line says
#                            ALL CORE TESTS PASS
# version.h (VERSION, the commit): regenerated when it changed; kept out of DEPS_H so a new commit does not rebuild every test
$(shell ../tools/gen_version.sh)
CXX      ?= c++
B        := build
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -DCORE_UI_TOOLS=1 -I. -Ihost
CORE_SRC := generic/generic.cpp screen/screen.cpp clock/clock.cpp proc/proc.cpp device/device.cpp device/digitakt.cpp hooks.cpp ui/canvas.cpp \
            msamp/cardq.cpp lib/sha256.cpp browse/browse.cpp devmidi/devmidi.cpp host/platform_host.cpp \
            $(wildcard fmt/*.cpp) $(wildcard store/*.cpp) $(wildcard kv/*.cpp) $(wildcard locks/*.cpp) route/route.cpp dsp/dsp.cpp code/loader.cpp code/code.cpp
CORE_OBJ := $(patsubst %.cpp,$(B)/obj/%.o,$(CORE_SRC))
DEPS_H   := $(filter-out version.h,$(wildcard *.h */*.h */*/*.h))
TESTS    := $(patsubst tests/%.cpp,$(B)/%,$(wildcard tests/test_*.cpp))

test: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done; echo "ALL CORE TESTS PASS"
.PHONY: test

$(B)/obj/%.o: %.cpp $(DEPS_H)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/test_%: tests/test_%.cpp $(CORE_OBJ) $(DEPS_H)
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -o $@ $< $(CORE_OBJ)

# The same suite built for AArch64 and run under qemu-aarch64 user mode: the only place a blob's code runs off the Pi
# (test_code loads TGAIN, sidecar-core blobs/: SCX_TGAIN=<path to tgain.scx>).
#   make -C core test-aarch64 SCX_TGAIN=../../sidecar-core/blobs/build/tgain.scx
A64_CXX ?= aarch64-linux-gnu-g++
QEMU    ?= qemu-aarch64
A64     := build-aarch64
A64_OBJ := $(patsubst %.cpp,$(A64)/obj/%.o,$(CORE_SRC))
A64_TESTS := $(patsubst tests/%.cpp,$(A64)/%,$(wildcard tests/test_*.cpp))
test-aarch64: $(A64_TESTS)
	@test -n "$(SCX_TGAIN)" || { echo "SCX_TGAIN=<tgain.scx> is needed"; exit 1; }
	@set -e; for t in $(A64_TESTS); do SCX_TGAIN=$(abspath $(SCX_TGAIN)) $(QEMU) ./$$t; done; echo "ALL CORE TESTS PASS (AArch64, qemu)"
.PHONY: test-aarch64
$(A64)/obj/%.o: %.cpp $(DEPS_H)
	@mkdir -p $(dir $@)
	$(A64_CXX) $(CXXFLAGS) -c -o $@ $<
$(A64)/test_%: tests/test_%.cpp $(A64_OBJ) $(DEPS_H)
	@mkdir -p $(A64)
	$(A64_CXX) $(CXXFLAGS) -static -o $@ $< $(A64_OBJ)

clean:
	rm -rf $(B) $(A64)
.PHONY: clean

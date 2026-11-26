# plugins/bp303/Makefile - BP303, a 303-style bass line voice: BLACKPYRAMIDS_303's voice (Zac Kyoti and the OT Kyoti FW
# contributors, MIT: LICENSE.kyoti; built on Open303, Robin Schmidt, MIT: LICENSE.open303; NOTICE) in float.
#   make a64        bp303.a64.rmp, the Pi package (aarch64-none-elf-gcc from PATH, else the rytm-circle docker image)
#   make libbp303.dylib   the Mac build (tests/demo.py, ../sc_render.py); a plain `make` also makes the Teensy's
#                   bp303.rmp (Teensyduino's arm-none-eabi-gcc): not built or run so far, BP303 is a Pi machine
#   make package    out/: bp303.a64.rmp, LICENSE.txt (ours, BLACKPYRAMIDS_303's and Open303's), NOTICE, SHA256SUMS,
#                   BUILD-INFO-plugins.txt
#   make check      tests/bp303_state.c (bp303.c compiled in: no subnormal in the state, 1-frame calls, the VOL 64 / 65
#                   hand-over, the accent sweep) and tests/bp303_check.cpp: the package loaded and run on this arm64 Mac
#                   as the Pi runs it (silence at rest, finite, CUT / RES sweeps under the ceiling, a slide keeps one
#                   note, accent, ns a voice)
#   make demo       libbp303.dylib, then tests/demo.py: acid lines as WAV files (DEMO=directory)
# MIT, Copyright 2026 trigcondition and contributors.
NAME := bp303
SRCS := bp303.c
# declared CPU per voice (per mille of a Teensy voice; the Pi reads it as permille x 0.158 % of a Pi 4 core, x 2.5 for
# its budget): the Mac's timing against sine_test (17 permille measured on the Teensy), make check's cpu line
BP303_PERMILLE ?= 300
CFLAGS_EXTRA := -DBP303_PERMILLE=$(BP303_PERMILLE)
include ../../sdk/rmp/plugin.mk
$(OBJS) $(NOBJS) $(AOBJS): Makefile bp303_tables.h

package: a64
	@rm -rf out; mkdir -p out
	mv $(NAME).a64.rmp out/   # (not left here: core/tests/test_api3_compat compares every package it finds under plugins/)
	[ "$$(head -c 4 out/$(NAME).a64.rmp)" = RMPA ] || { echo "not an RMPA package"; exit 1; }
	cat LICENSE > out/LICENSE.txt
	{ echo; echo "---- BLACKPYRAMIDS_303 (the voice BP303 is ported from) ----"; cat LICENSE.kyoti; } >> out/LICENSE.txt
	{ echo; echo "---- Open303 (the algorithms BLACKPYRAMIDS_303 is built on) ----"; cat LICENSE.open303; } >> out/LICENSE.txt
	cp NOTICE out/NOTICE.txt
	cd out && shasum -a 256 *.rmp > SHA256SUMS
	echo "source commit: $$(git rev-parse HEAD)$$(git diff --quiet HEAD -- . || echo -dirty)" > out/BUILD-INFO-plugins.txt
	@ls -l out
check: package lib$(NAME).dylib
	$(MAKE) -C tests run
demo: lib$(NAME).dylib
	$(PY) tests/demo.py $(DEMO)
clean-all: clean
	rm -rf out
	$(MAKE) -C tests clean
.PHONY: package check demo clean-all

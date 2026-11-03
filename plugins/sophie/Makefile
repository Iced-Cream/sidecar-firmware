# plugins/sophie/Makefile - SOPHIE, metallic FM percussion (kicks, clangs, zaps, bells, shards): Sophie's DSP (Matt
# Estela, MIT: LICENSE.sophie, NOTICE) as one track a state (two hits ring at once); a poly track plays its notes on
# instances (manifest max_voices 8).
#   make a64        sophie.a64.rmp, the Pi package (aarch64-none-elf-gcc from PATH, else the rytm-circle docker image)
#   make            sophie.rmp (the Teensy, Teensyduino's arm-none-eabi-gcc) and libsophie.dylib (the Mac)
#   make package    out/: sophie.a64.rmp, LICENSE.txt (ours and Sophie's), NOTICE, SHA256SUMS, BUILD-INFO-plugins.txt
#   make check      tests/sophie_check.cpp: the package loaded and run on this arm64 Mac as the Pi runs it (every
#                   model, rms, finite, no denormals, guard bytes, decay to silence, each P at 0 and 127, overlapping
#                   hits, ns a voice)
#   make demo       libsophie.dylib, then tests/demo.py: Sophie's factory kit as a drum pattern in a WAV (DEMO=path)
# MIT, Copyright 2026 trigcondition and contributors.
NAME := sophie
SRCS := sophie.c
# declared CPU per voice (per mille of a Teensy voice; the Pi reads it as permille x 0.158 % of a Pi 4 core, x 2.5 for
# its budget): the Mac's timing against sine_test (17 permille measured on the Teensy), make check's cpu line
SOPH_PERMILLE ?= 220
CFLAGS_EXTRA := -DSOPH_PERMILLE=$(SOPH_PERMILLE)
include ../../sdk/rmp/plugin.mk
$(OBJS) $(NOBJS) $(AOBJS): Makefile sophie.h

package: a64
	@rm -rf out; mkdir -p out
	cp $(NAME).a64.rmp out/
	[ "$$(head -c 4 out/$(NAME).a64.rmp)" = RMPA ] || { echo "not an RMPA package"; exit 1; }
	cat LICENSE > out/LICENSE.txt
	{ echo; echo "---- Sophie (the DSP SOPHIE is built from) ----"; cat LICENSE.sophie; } >> out/LICENSE.txt
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

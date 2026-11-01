# plugins/modele/Makefile - MODEL E, a three-oscillator voice with a ladder filter (basses, leads): ModelDigi's
# oscillator bank (third_party/modeldigi, GPL-2.0-or-later: NOTICE) with a filter, contours and glide, one voice a
# state; glide on a mono track, chords on a poly track's instances (manifest max_voices).
#   make a64        modele.a64.rmp, the Pi package (aarch64-none-elf-gcc from PATH, else the rytm-circle docker image)
#   make            modele.rmp (the Teensy, Teensyduino's arm-none-eabi-gcc) and libmodele.dylib (the Mac)
#   make package    out/: modele.a64.rmp, LICENSE.txt (the GPL), NOTICE.txt, modele-source.tar.gz (the package's
#                   complete source, which the GPL asks to go with the binary), SHA256SUMS, BUILD-INFO-plugins.txt
#   make check      tests/md_check.cpp: the package loaded and run on this arm64 Mac as the Pi runs it: pitch, filter,
#                   self-oscillation, glide, clicks, extremes, denormals, level, ns a voice against HUSH1's
#   make demo       libmodele.dylib, then tests/demo.py: the four demo WAVs (DEMO=directory)
# GPL-2.0-or-later, Copyright 2026 trigcondition and contributors.
NAME := modele
SRCS := modele.c oscillator.c
vpath oscillator.c third_party/modeldigi/dsp
INCS := -Ithird_party/modeldigi
# declared CPU per voice (per mille of a Teensy voice; the Pi reads it as permille x 0.158 % of a Pi 4 core, x 2.5 for
# its budget): make check's cpu lines
MODELE_PERMILLE ?= 180
CFLAGS_EXTRA := -DMODELE_PERMILLE=$(MODELE_PERMILLE)
include ../../sdk/rmp/plugin.mk
$(OBJS) $(NOBJS) $(AOBJS): Makefile

# the complete corresponding source of the package: the plugin and everything of the repository its build reads
SRCDIR := plugins/modele
SRCFILES := $(SRCDIR)/LICENSE $(SRCDIR)/NOTICE $(SRCDIR)/README.md $(SRCDIR)/Makefile $(SRCDIR)/manifest.json \
  $(SRCDIR)/modele.c $(SRCDIR)/third_party $(SRCDIR)/tests sdk/rmp tools/rmp_pack.py

package: a64
	@rm -rf out; mkdir -p out
	cp $(NAME).a64.rmp out/
	[ "$$(head -c 4 out/$(NAME).a64.rmp)" = RMPA ] || { echo "not an RMPA package"; exit 1; }
	cat LICENSE third_party/modeldigi/LICENSE > out/LICENSE.txt
	cp NOTICE out/NOTICE.txt
	COPYFILE_DISABLE=1 tar -C ../.. --no-xattrs --no-mac-metadata --uid 0 --gid 0 --uname "" --gname "" \
	  --exclude 'build' --exclude '*.pyc' --exclude '__pycache__' -cf - $(SRCFILES) | gzip -n > out/$(NAME)-source.tar.gz
	cd out && shasum -a 256 *.rmp *.tar.gz > SHA256SUMS
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

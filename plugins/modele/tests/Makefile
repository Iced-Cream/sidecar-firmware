# plugins/modele/tests/Makefile - the host checks (md_check.cpp). `make -C .. check` builds the package first.
# GPL-2.0-or-later, Copyright 2026 trigcondition and contributors.
RMPSRC := ../../../teensy/libraries/RytmPlugins/src
B := ../build/check
$(B)/md_check: md_check.cpp $(RMPSRC)/rmp_loader.cpp
	@mkdir -p $(B)
	c++ -std=c++17 -O2 -Wall -I$(RMPSRC) -o $@ md_check.cpp $(RMPSRC)/rmp_loader.cpp
# the CPU references: the machines with a known Pi figure (HUSH1, NUSAW, VOX) and the sine test (17 permille measured)
REFS := ../../hush1/libhush1.dylib ../../nusaw/libnusaw.dylib ../../vox/libvox.dylib ../../sine_test/libsine_test.dylib
$(REFS):
	$(MAKE) -C $(dir $@) $(notdir $@)
run: $(B)/md_check $(REFS)
	$(B)/md_check ../out/modele.a64.rmp ../libmodele.dylib $(REFS)
clean:
	rm -rf $(B)
.PHONY: run clean

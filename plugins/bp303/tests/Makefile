# plugins/bp303/tests/Makefile - the host checks: bp303_check.cpp (the package; `make -C .. check` builds it first) and
# bp303_state.c (bp303.c compiled in: what needs the voice's state; `make state` runs it alone, no package needed).
# MIT, Copyright 2026 trigcondition and contributors.
RMPSRC := ../../../teensy/libraries/RytmPlugins/src
B := ../build/check
$(B)/bp303_check: bp303_check.cpp $(RMPSRC)/rmp_loader.cpp
	@mkdir -p $(B)
	c++ -std=c++17 -O2 -Wall -I$(RMPSRC) -o $@ bp303_check.cpp $(RMPSRC)/rmp_loader.cpp
# the CPU references: the native builds of sine_test (17 permille measured on the Teensy) and two starter machines
REFS := ../../sine_test/libsine_test.dylib ../../sc_acid/libsc_acid.dylib ../../sc_wavepad/libsc_wavepad.dylib
$(REFS):
	$(MAKE) -C $(dir $@) $(notdir $@)
$(B)/bp303_state: bp303_state.c ../bp303.c ../bp303_tables.h
	@mkdir -p $(B)
	cc -std=c11 -O2 -Wall -I../../../sdk -I../../../sdk/rmp -o $@ bp303_state.c
state: $(B)/bp303_state
	$(B)/bp303_state
run: state $(B)/bp303_check $(REFS)
	$(B)/bp303_check ../out/bp303.a64.rmp ../libbp303.dylib $(REFS)
clean:
	rm -rf $(B)
.PHONY: run state clean

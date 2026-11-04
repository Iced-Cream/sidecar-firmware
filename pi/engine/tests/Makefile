# pi/engine/tests/Makefile - the Pi engine's portable parts on the host: make -C pi/engine/tests (shim/: the few
# Circle headers the gadget and HID tests need). Also the build's version stamp (tools/tests/test_gen_version.sh) and the Rytm MIDI
# bridge's relay (tools/tests/test_rytm_bridge.py).
$(shell ../../../tools/gen_version.sh)
CXX ?= c++
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -I.. -I../../../core -Ishim -fsanitize=address,undefined -fno-sanitize-recover=all
SRC := ../midi_in.cpp ../emu_proto.cpp
TESTS := $(patsubst %.cpp,build/%,$(wildcard test_*.cpp)) build/test_stor_fatfs_exfat build/test_stor_fatfs_lfn3
CORE := ../../../core
.PHONY: all clean fatfs-mac
all: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done; ../../../tools/tests/test_gen_version.sh; python3 ../../../tools/tests/test_rytm_bridge.py -q; python3 test_sdboot.py -q; python3 test_bootimg.py -q; echo "ALL PI PORTABLE TESTS PASS"
build/%: %.cpp $(SRC) $(wildcard ../*.h) $(wildcard shim/circle/*.h) check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< $(SRC)
# the real ob_usb.cpp on a fake Circle (shim_ob/); copied, so its quoted includes find the shim's neighbours before the real ones
build/test_ob_usb: test_ob_usb.cpp ../ob_usb.cpp ../ob_usb.h ../ob_block.h $(CORE)/device/digitakt.cpp $(CORE)/device/device.h $(CORE)/device/audio.h $(wildcard shim_ob/*.h) $(wildcard shim_ob/circle/*.h) $(wildcard shim_ob/circle/usb/*.h) check.h
	@mkdir -p build/ob_src
	cp ../ob_usb.cpp ../ob_usb.h build/ob_src/
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=all -Ibuild/ob_src -Ishim_ob -I.. -I../../../core -o $@ test_ob_usb.cpp build/ob_src/ob_usb.cpp $(CORE)/device/digitakt.cpp -DOB_USB_TEST
# the Overbridge block's layout and parser (ob_block.h)
build/test_ob_block: test_ob_block.cpp ../ob_block.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
# the USB touch screen's descriptor parser and contact tracker (pi/circle/hid_digitizer.h, circle-touch.patch)
build/test_hid_digitizer: test_hid_digitizer.cpp ../../circle/hid_digitizer.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
# when a halted HID report endpoint is escalated to a device re-enumeration (pi/circle/hid_reenum.h,
# circle-hid-reenum2.patch): the timeline measured on the real panel, replayed
build/test_hid_reenum: test_hid_reenum.cpp ../../circle/hid_reenum.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
# the DWC2 gadget's bus suspend and resume (pi/circle/gadget_wake.h, circle-gadget-wake.patch)
build/test_gadget_wake: test_gadget_wake.cpp ../../circle/gadget_wake.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
# a USB reset's TX FIFO flush at IRQ level (pi/circle/gadget_flush.h, circle-gadget-reset.patch)
build/test_gadget_flush: test_gadget_flush.cpp ../../circle/gadget_flush.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<
# the web file browser (fsweb.cpp) on a temp folder
build/test_fsweb: test_fsweb.cpp ../fsweb.cpp ../fsweb.h $(CORE)/lib/sha256.cpp check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -DFSWEB_HOST -o $@ $< ../fsweb.cpp $(CORE)/lib/sha256.cpp
# the generic engine's Pi side: bridge.cpp and the audio return over the generic core (core/generic)
GENERIC_CORE := $(addprefix $(CORE)/,generic/generic.cpp screen/screen.cpp clock/clock.cpp proc/proc.cpp device/device.cpp \
                device/digitakt.cpp store/store.cpp msamp/cardq.cpp hooks.cpp ui/canvas.cpp lib/sha256.cpp \
                host/platform_host.cpp browse/browse.cpp devmidi/devmidi.cpp) \
                $(wildcard $(CORE)/fmt/*.cpp) $(wildcard $(CORE)/kv/*.cpp) $(wildcard $(CORE)/locks/*.cpp)
build/test_generic_pi: test_generic_pi.cpp ../bridge.cpp ../audio_return.cpp ../audio_return.h ../bridge.h $(GENERIC_CORE) check.h
	@mkdir -p build
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -DCORE_UI_TOOLS=1 -I$(CORE)/host -I$(CORE) -I.. \
	  -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $< ../bridge.cpp ../audio_return.cpp $(GENERIC_CORE)
# the power supply's state (power_state.cpp): get_throttled and the Pi 5's max_current to the warning and /status
build/test_power_state: test_power_state.cpp ../power_state.cpp ../power_state.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../power_state.cpp
# the Wi-Fi settings (wifi_conf.cpp) on a temp folder
build/test_wifi_conf: test_wifi_conf.cpp ../wifi_conf.cpp ../wifi_conf.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../wifi_conf.cpp
# the Rytm MIDI bridge (midibridge.h, usbmidi_merge.h: header only), with the core's SHA-256 (its HMAC)
build/test_midibridge: test_midibridge.cpp ../midibridge.h ../usbmidi_merge.h ../usbmidi_pack.h $(CORE)/lib/sha256.cpp check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< $(CORE)/lib/sha256.cpp
# the pairing code (paircode.cpp) on a temp folder, with the core's SHA-256
build/test_paircode: test_paircode.cpp ../paircode.cpp ../paircode.h ../wifi_conf.cpp ../wifi_conf.h $(CORE)/lib/sha256.cpp check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../paircode.cpp ../wifi_conf.cpp $(CORE)/lib/sha256.cpp
# the storage layer (docs/STORAGE.md): the volumes' state machine and rules, the bench on a simulated drive and a temp folder
build/test_stor_vol: test_stor_vol.cpp ../stor_vol.cpp ../stor_vol.h ../stor_out.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../stor_vol.cpp
build/test_stor_bench: test_stor_bench.cpp ../stor_bench.cpp ../stor_bench.h ../stor_vol.cpp ../stor_vol.h ../stor_out.h check.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< ../stor_bench.cpp ../stor_vol.cpp
# FatFs on file images (test_stor_fatfs.cpp): Circle's addon/fatfs sources (CIRCLE_BUILD: pi/circle/build, or the main
# checkout's from a worktree), copied into build/fatfs-{fat,exfat} with the host's changes (no lock, f_mkfs, no clock,
# LFN on the stack); exfat: the STOR_EXFAT switch's two (pi/engine/build.sh: FF_FS_EXFAT 1, FF_LBA64 1).
# make fatfs-mac: the same against images macOS makes (hdiutil: MBR / GPT x FAT32 / exFAT), removed after
CIRCLE_BUILD ?= $(firstword $(wildcard ../../circle/build/circle) $(shell git -C ../../.. worktree list --porcelain 2>/dev/null | sed -n '1s/^worktree //p')/pi/circle/build/circle)
FATFS_SRC ?= $(if $(filter %/circle,$(CIRCLE_BUILD)),$(CIRCLE_BUILD),$(CIRCLE_BUILD)/circle)/addon/fatfs
FATFS_HOST := -e 's/^\#define FF_FS_REENTRANT.*/\#define FF_FS_REENTRANT 0/' -e 's/^\#define FF_USE_MKFS.*/\#define FF_USE_MKFS 1/' \
	      -e 's/^\#define FF_FS_NORTC.*/\#define FF_FS_NORTC 1/' -e 's/^\#define FF_USE_LFN.*/\#define FF_USE_LFN 1/'
STOR_EXFAT_SED := -e 's/^\#define FF_FS_EXFAT.*/\#define FF_FS_EXFAT 1/' -e 's/^\#define FF_LBA64.*/\#define FF_LBA64 1/'
build/fatfs-%/ff.h: $(FATFS_SRC)/ff.c $(FATFS_SRC)/ffconf.h
	@test -f $(FATFS_SRC)/ff.c || { echo "fatfs: no $(FATFS_SRC) (CIRCLE_BUILD=.../pi/circle/build)"; exit 1; }
	@mkdir -p $(dir $@)
	cp $(FATFS_SRC)/ff.c $(FATFS_SRC)/ff.h $(FATFS_SRC)/diskio.h $(FATFS_SRC)/ffunicode.c $(dir $@)
	sed $(FATFS_HOST) $(if $(filter exfat,$*),$(STOR_EXFAT_SED),) $(FATFS_SRC)/ffconf.h > $(dir $@)ffconf.h
FATFS_DEPS = test_stor_fatfs.cpp ../stor_vol.cpp ../stor_vol.h check.h
build/test_stor_fatfs: build/fatfs-fat/ff.h $(FATFS_DEPS)
	cc -O1 -g -w -c -o build/fatfs-fat/ff.o build/fatfs-fat/ff.c && cc -O1 -g -w -c -o build/fatfs-fat/ffunicode.o build/fatfs-fat/ffunicode.c
	$(CXX) $(CXXFLAGS) -Ibuild/fatfs-fat -o $@ test_stor_fatfs.cpp ../stor_vol.cpp build/fatfs-fat/ff.o build/fatfs-fat/ffunicode.o
# the Pi's FF_USE_LFN 3 (ff_memalloc: dir_clear's 32 KB zero-fill buffer) instead of the host's 1
build/fatfs-lfn3/ff.h: $(FATFS_SRC)/ff.c $(FATFS_SRC)/ffconf.h
	@mkdir -p $(dir $@)
	cp $(FATFS_SRC)/ff.c $(FATFS_SRC)/ff.h $(FATFS_SRC)/diskio.h $(FATFS_SRC)/ffunicode.c $(dir $@)
	sed $(FATFS_HOST) -e 's/^\#define FF_USE_LFN.*/\#define FF_USE_LFN 3/' $(FATFS_SRC)/ffconf.h > $(dir $@)ffconf.h
build/test_stor_fatfs_lfn3: build/fatfs-lfn3/ff.h $(FATFS_DEPS)
	cc -O1 -g -w -c -o build/fatfs-lfn3/ff.o build/fatfs-lfn3/ff.c && cc -O1 -g -w -c -o build/fatfs-lfn3/ffunicode.o build/fatfs-lfn3/ffunicode.c
	$(CXX) $(CXXFLAGS) -Ibuild/fatfs-lfn3 -o $@ test_stor_fatfs.cpp ../stor_vol.cpp build/fatfs-lfn3/ff.o build/fatfs-lfn3/ffunicode.o
build/test_stor_fatfs_exfat: build/fatfs-exfat/ff.h $(FATFS_DEPS)
	cc -O1 -g -w -c -o build/fatfs-exfat/ff.o build/fatfs-exfat/ff.c && cc -O1 -g -w -c -o build/fatfs-exfat/ffunicode.o build/fatfs-exfat/ffunicode.c
	$(CXX) $(CXXFLAGS) -Ibuild/fatfs-exfat -o $@ test_stor_fatfs.cpp ../stor_vol.cpp build/fatfs-exfat/ff.o build/fatfs-exfat/ffunicode.o
MACIMG := build/macimg
fatfs-mac: build/test_stor_fatfs build/test_stor_fatfs_exfat
	@rm -rf $(MACIMG); mkdir -p $(MACIMG)
	hdiutil create -quiet -size 64m -fs "MS-DOS FAT32" -layout MBRSPUD -volname SIDECAR -o $(MACIMG)/fat32-mbr.dmg
	hdiutil create -quiet -size 64m -fs "MS-DOS FAT32" -layout GPTSPUD -volname SIDECAR -o $(MACIMG)/fat32-gpt.dmg
	hdiutil create -quiet -size 64m -fs ExFAT -layout MBRSPUD -volname SIDECAR -o $(MACIMG)/exfat-mbr.dmg
	hdiutil create -quiet -size 64m -fs ExFAT -layout GPTSPUD -volname SIDECAR -o $(MACIMG)/exfat-gpt.dmg
	./build/test_stor_fatfs $(MACIMG) && ./build/test_stor_fatfs_exfat $(MACIMG); s=$$?; rm -rf $(MACIMG); exit $$s
clean:
	rm -rf build

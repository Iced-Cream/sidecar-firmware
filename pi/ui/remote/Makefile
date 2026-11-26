# pi/ui/remote/Makefile - the remote UI protocol library's tests on the Mac (no LVGL, no Circle): make test
CC ?= cc
CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -fno-sanitize-recover=all
LIB := rmt_proto.c rmt_views.c rmt_cli.c rmt_sha256.c
HDR := $(wildcard *.h) $(wildcard ../sidecar_status.h ../sidecar_voice.h ../sidecar_insfx.h ../sidecar_mfx.h ../sidecar_seqed.h ../rytm_pattern.h)
.PHONY: test clean
test: obj/test_rmt
	obj/test_rmt
obj/test_rmt: test_rmt.c $(LIB) $(HDR)
	@mkdir -p obj
	$(CC) $(CFLAGS) -o $@ test_rmt.c $(LIB)
clean:
	rm -rf obj

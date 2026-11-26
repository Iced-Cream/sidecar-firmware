# plugins/ufx_nimbus/Makefile - NIMBUS, an ensemble reverb user effect (Plugin API 3; nimbus.c; MIT).
#   make a64        ufx_nimbus.a64.rmp (aarch64-none-elf-gcc from PATH, else the rytm-circle Docker image)
#   make fixtures   also copied to core/tests/api3_pkgs (CI has no AArch64 compiler)
NAME := ufx_nimbus
SRCS := nimbus.c
include ../../sdk/rmp/plugin.mk
fixtures: a64
	cp ufx_nimbus.a64.rmp ../../core/tests/api3_pkgs/
.PHONY: fixtures

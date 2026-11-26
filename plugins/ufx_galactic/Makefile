# plugins/ufx_galactic/Makefile - GALACTIC, Airwindows' Galactic reverb as a user effect (Plugin API 3; galactic.c; MIT).
#   make a64        ufx_galactic.a64.rmp (aarch64-none-elf-gcc from PATH, else the rytm-circle Docker image)
#   make fixtures   also copied to core/tests/api3_pkgs (CI has no AArch64 compiler)
NAME := ufx_galactic
SRCS := galactic.c
include ../../sdk/rmp/plugin.mk
fixtures: a64
	cp ufx_galactic.a64.rmp ../../core/tests/api3_pkgs/
.PHONY: fixtures

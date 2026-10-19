# spike1: banner + timer ticks on the serial console. Built against a Circle tree
# (CIRCLEHOME, default ../build/circle; build.sh builds it).
CIRCLEHOME ?= ../build/circle

OBJS	= main.o kernel.o

LIBS	= $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)

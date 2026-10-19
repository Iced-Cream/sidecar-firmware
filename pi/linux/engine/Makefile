CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -pthread $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0) -lm -pthread

engine: engine.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f engine
.PHONY: clean

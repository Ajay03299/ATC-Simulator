# ATC Simulator - Makefile
# Works on macOS (clang) and Linux (gcc).

CC      := cc
CFLAGS  := -Wall -Wextra -O2 -g -pthread
LDFLAGS := -pthread

SRC_COMMON := src/common/protocol.c
OBJ_COMMON := $(SRC_COMMON:.c=.o)

BINS := bin/atc_server bin/pilot bin/radar

all: $(BINS)

bin:
	@mkdir -p bin
	@mkdir -p logs

bin/atc_server: src/server/main.c $(OBJ_COMMON) | bin
	$(CC) $(CFLAGS) -o $@ src/server/main.c $(OBJ_COMMON) $(LDFLAGS)

bin/pilot: src/clients/pilot.c $(OBJ_COMMON) | bin
	$(CC) $(CFLAGS) -o $@ src/clients/pilot.c $(OBJ_COMMON) $(LDFLAGS)

bin/radar: src/clients/radar.c $(OBJ_COMMON) | bin
	$(CC) $(CFLAGS) -o $@ src/clients/radar.c $(OBJ_COMMON) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $

clean:
	rm -rf bin src/common/*.o logs/*.log
	rm -f /tmp/atc.sock /tmp/atc_handoff.fifo

.PHONY: all clean

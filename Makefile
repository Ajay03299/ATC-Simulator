# ATC Simulator - Makefile
# Works on macOS (clang) and Linux (gcc).

CC      := cc
CFLAGS  := -Wall -Wextra -O2 -g -pthread
LDFLAGS := -pthread

# Shared code used by every binary
SRC_COMMON := src/common/protocol.c
OBJ_COMMON := $(SRC_COMMON:.c=.o)

# Server-side modules (auth, runway locks, gate semaphore, handoff pipe)
SRC_SERVER_EXTRA :=     src/server/auth.c     src/server/runway.c     src/server/gates.c     src/server/handoff.c
OBJ_SERVER_EXTRA := $(SRC_SERVER_EXTRA:.c=.o)

BINS := bin/atc_server bin/pilot bin/radar bin/controller

all: $(BINS)

bin:
	@mkdir -p bin
	@mkdir -p logs

bin/atc_server: src/server/main.c $(OBJ_COMMON) $(OBJ_SERVER_EXTRA) | bin
	$(CC) $(CFLAGS) -o $@ src/server/main.c $(OBJ_COMMON) $(OBJ_SERVER_EXTRA) $(LDFLAGS)

bin/pilot: src/clients/pilot.c $(OBJ_COMMON) | bin
	$(CC) $(CFLAGS) -o $@ src/clients/pilot.c $(OBJ_COMMON) $(LDFLAGS)

bin/radar: src/clients/radar.c $(OBJ_COMMON) | bin
	$(CC) $(CFLAGS) -o $@ src/clients/radar.c $(OBJ_COMMON) $(LDFLAGS)

bin/controller: src/clients/controller.c $(OBJ_COMMON) | bin
	$(CC) $(CFLAGS) -o $@ src/clients/controller.c $(OBJ_COMMON) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf bin src/common/*.o src/server/*.o logs/*.log
	rm -f /tmp/atc.sock /tmp/atc_handoff.fifo

.PHONY: all clean

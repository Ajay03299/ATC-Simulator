# ATC Simulator

A multi-controller air traffic control simulator written in C. 

Everything runs locally — the network is a Unix domain socket and the
different systems are different terminal tabs. Keeps things simple and
lets you actually stress-test concurrency.

## What it does

A central server holds the state of an airspace. Clients connect to it:

- **Pilots** spawn aircraft and stream live position updates
- **Approach, Tower, and Ground controllers** each manage a slice of
  the operation through a REPL — server enforces who can do what
- **Admin** reads logs, kills aircraft, triggers weather alerts
- **Radar** is read-only and renders an ASCII grid of every aircraft

Behind the scenes the server is juggling threads, mutexes, a semaphore,
flock'd log files, a named pipe for handoffs, and a couple of signal
handlers for emergencies.

## Build and run

Open a few terminals. Server first, then anything else:
./bin/atc_server                        # terminal 1
./bin/radar                             # terminal 2
./bin/pilot AI203 10000                 # terminal 3
./bin/controller --role=tower           # terminal 4
./bin/controller --role=ground          # terminal 5
./bin/controller --role=admin           # terminal 6

Type `help` in any controller terminal to see what commands work.

## Demo flow

1. Spawn an aircraft — `+` mark appears on the radar.
2. From Approach: `HANDOFF_TOWER 1`. All controllers see an alert
   coming through the **named pipe**.
3. From Tower: `CLEAR_LAND 1 0`. Runway is held for 2s — try the same
   command from another Tower terminal and it blocks. That's the
   **pthread mutex**.
4. From Ground: `TAXI 1 1` four times. The fifth blocks because the
   gate **semaphore** is at zero.
5. In a pilot terminal, hit `Ctrl+\` for socket-based mayday.
6. From any terminal: `kill -USR1 $(cat /tmp/atc_server.pid)` — this is
   **signal-based IPC**, doesn't touch the socket.
7. Same with `kill -USR2 ...` for weather.
8. From Admin: `VIEW_LOGS` reads the flight log under a shared lock.

`Ctrl+C` on the server to shut down.

## Project layout
src/
├── common/    types.h, protocol.h/.c    (shared)
├── server/    main, auth, runway, gates, handoff, logger, emergency
└── clients/   pilot.c, radar.c, controller.c

## Where each required concept lives

| Concept                  | File                                    |
|--------------------------|-----------------------------------------|
| Role-based authorization | `src/server/auth.c`                     |
| File locking             | `src/server/logger.c` (LOCK_EX/LOCK_SH) |
| Concurrency — mutex      | `main.c`, `runway.c`                    |
| Concurrency — semaphore  | `src/server/gates.c`                    |
| Data consistency         | mutex-protected updates + flock'd logs  |
| Sockets                  | AF_UNIX at `/tmp/atc.sock`              |
| IPC — named pipe         | `src/server/handoff.c`                  |
| IPC — signals            | `src/server/emergency.c`                |

## Notes

- **macOS doesn't support unnamed semaphores** (`sem_init` returns
  ENOSYS). Used `sem_open` instead.
- **Stream sockets don't preserve message boundaries**, so every
  message has a 4-byte big-endian length prefix.
- **Signal handlers** only set a `volatile sig_atomic_t` flag; the
  main loop polls it and does the real work.

## Cleanup
make clean

Removes binaries, object files, logs, and stale `/tmp/` files.

## Tested on

macOS with clang. Should also work on Linux with gcc — pure POSIX.

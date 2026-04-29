# ATC Simulator — Project Report

**Course:** EGC 301P / Operating Systems Lab
**Project:** Multi-Controller Air Traffic Control Simulator
**Author:** Ajay Javali - BT2024079
**Repository:** https://github.com/Ajay03299/ATC-Simulator

---

## 1. Problem Statement

I wanted to build something that genuinely needs every concept the
course covers, instead of bolting them onto a project that doesn't
care. Air traffic control turned out to be a great fit — multiple
controllers managing aircraft through clearly separated zones, sharing
a single source of truth, enforcing role boundaries, and handling
emergencies without losing data.

The simulator runs locally. A central server holds the aircraft
state. Pilots, controllers, an admin, and a read-only radar each run
as their own process in different terminals. Between them they
exercise role-based authorization, file locking, mutexes, a
semaphore, data consistency, sockets, named pipes, and signals —
all six required concepts plus an extra IPC mechanism.

The "network" is a Unix domain socket and the "different systems"
are different terminal tabs. The project guidelines explicitly
allow this.

---

## 2. System Architecture
+-----------+      +------------+      +-----------+
|  Pilots   |      | Controllers|      |   Admin   |
+-----+-----+      +-----+------+      +-----+-----+
|                  |                   |
|   AF_UNIX socket /tmp/atc.sock       |
+------------------+-------------------+
|
+--------+--------+
|   ATC server    |
|  - aircraft tbl |
|  - mutexes      |
|  - semaphore    |
|  - signal hdlr  |
+--------+--------+
|
+----------------+-----------------+
|                |                 |
/tmp/atc_handoff   logs/flights.log   logs/incidents.log
.fifo              (flock'd)          (flock'd)
|
+------+-----+
|   Radar    |
| (read only)|
+------------+

The server is multi-threaded: an accept loop, one thread per
connected client, a broadcaster thread that ticks every 500ms
(STATE updates + draining signal flags), and a handoff-reader
thread pulling events from the named pipe.

**Codebase layout:**
src/
├── common/   types.h, protocol.h/.c
├── server/   main.c + auth, runway, gates, handoff, logger, emergency
└── clients/  pilot.c, radar.c, controller.c

---

## 3. How Each Required Concept Is Implemented

### 3.1 Role-based Authorization — `src/server/auth.c`

The whole authorization model is one declarative table mapping each
command to a bitmask of allowed roles:

```c
{ "CLEAR_LAND", R(ROLE_TOWER) | R(ROLE_ADMIN) },
{ "TAXI",      R(ROLE_GROUND) | R(ROLE_ADMIN) },
{ "VIEW_LOGS", R(ROLE_ADMIN) },
```

Every incoming command passes through `is_authorized()` before
anything happens. Unknown commands default to denied — secure by
default. Adding a new command is one new row.

**Demo:** `05_ground_role_auth.png` — Ground denied a Tower-only
command.

### 3.2 File Locking — `src/server/logger.c`

Two append-only logs, both written by many threads. Used `flock()`
over `fcntl()` because flock is whole-file (right granularity for
append-only logs) and auto-releases when the fd closes (no stuck
locks if the server crashes).

Write path uses **exclusive** lock:
```c
flock(fd, LOCK_EX);  /* blocks all other readers and writers */
```

Read path (admin's `VIEW_LOGS`) uses **shared** lock:
```c
flock(fd, LOCK_SH);  /* multiple readers OK, blocks writers */
```

That's the read/write lock distinction in two function calls.
`O_APPEND` + `LOCK_EX` + `fsync` together guarantee atomic, durable
writes that can never interleave.

**Demo:** `07_admin_view_logs.png` (shared read), `08_server_lifecycle.png`
(serialized writes).

### 3.3 Concurrency Control

#### Mutexes — `src/server/main.c`, `src/server/runway.c`

Three different mutexes for three different shared structures:
- `g_aircraft_mtx` — global aircraft table
- `g_clients_mtx` — connected-clients list
- A per-runway mutex array — each runway is independent, so two
  runways can be in use simultaneously

Per-resource locks instead of one global lock = no false contention.
Two threads touching different data don't have to wait for each
other.

#### Semaphore — `src/server/gates.c`

The 4 parking gates are a counting resource — any of them is
interchangeable, up to 4 holders at once. That's exactly what a
semaphore counts.

Used a **named** semaphore (`sem_open`) instead of unnamed because
macOS doesn't actually implement unnamed POSIX semaphores —
`sem_init` returns `ENOSYS`.

`TAXI` calls `sem_wait`; `PUSHBACK` calls `sem_post`. The fifth
`TAXI` (when all 4 gates are taken) blocks until somebody PUSHBACKs.

**Mutex vs semaphore in one line:** mutex is binary and guards a
single shared structure; semaphore counts identical resources.

### 3.4 Data Consistency

Three things working together:

1. **All shared state is mutex-protected.** No torn reads.
2. **State changes are paired with log writes.** Every
   `aircraft_set_state` is followed by `log_flight()` under
   `flock(LOCK_EX)` + `fsync`. The log on disk reflects exactly
   how far we got.
3. **Append-only logs prevent lost updates.** `O_APPEND` + flock
   means two concurrent writers can't lose either write or
   interleave bytes.

**Concrete example** — Tower clearing a landing:
- Take runway mutex
- Update aircraft state under aircraft mutex
- Log RUNWAY_ACQUIRE under exclusive flock with fsync
- (occupancy delay)
- Log RUNWAY_RELEASE
- Drop runway mutex

If the server crashes mid-sequence, mutexes are auto-released by the
kernel, the log on disk is correct, and there's no half-state.

### 3.5 Sockets — `src/server/main.c` + clients

`AF_UNIX` stream socket bound to `/tmp/atc.sock`. Every client
connects to the same socket. The accept loop spawns one detached
thread per client.

**Custom wire protocol because stream sockets don't preserve
message boundaries.** Two `write()`s can come back as one `read()`,
or split across two. Fix: 4-byte big-endian length prefix on every
message:
[4 bytes: htonl(N)] [N bytes: payload]

`send_msg` and `recv_msg` in `protocol.c` handle framing. Helpers
`read_all`/`write_all` loop until the exact byte count is satisfied,
handling short reads, short writes, and `EINTR`.

Payload is plain text (`HELLO PILOT`, `CLEAR_LAND 1 0`), which
makes the protocol debuggable.

### 3.6 Inter-Process Communication

Spec says "at least one IPC mechanism." Implemented two with very
different semantics — useful contrast.

#### Named Pipe — `src/server/handoff.c`

`mkfifo("/tmp/atc_handoff.fifo", 0644)`. When a controller hands
off an aircraft, the server writes one line to the FIFO:
HANDOFF 3 APPROACH TOWER

A reader thread pulls lines out and re-broadcasts them as ALERT
messages to all controllers. Writer uses `O_NONBLOCK` so a missing
reader doesn't deadlock the controller thread. POSIX guarantees
writes ≤ `PIPE_BUF` (512 bytes) are atomic on a pipe, so concurrent
writes from different threads can't interleave.

Cool side effect: the FIFO is a real file. You can `cat` it during
the demo to watch raw events flow.

#### Signals — `src/server/emergency.c`

`SIGUSR1` for mayday, `SIGUSR2` for weather alerts. Bypasses the
socket entirely — works even if the socket protocol is jammed.

POSIX restricts what's safe in a signal handler — only async-signal-
safe functions, no printf/malloc/mutex calls. Standard fix: handler
only sets a `volatile sig_atomic_t` flag; the broadcaster thread
polls the flag once per tick and does the actual work in normal
code.

Server writes its PID to `/tmp/atc_server.pid` on startup, so
clients can do:

```bash
kill -USR1 $(cat /tmp/atc_server.pid)
```

Standard Unix daemon pattern, same as nginx/Apache.

**Demo:** `06_signal_mayday.png`.

---

## 4. Concept ↔ Code Quick Reference

| Concept                      | File                       | Key symbols                          |
|------------------------------|----------------------------|--------------------------------------|
| Role-based authorization     | `auth.c`                   | `is_authorized`, `TABLE[]`           |
| File locking — exclusive     | `logger.c`                 | `flock(LOCK_EX)`                     |
| File locking — shared        | `logger.c`                 | `flock(LOCK_SH)`                     |
| Concurrency — mutex          | `main.c`, `runway.c`       | `g_aircraft_mtx`, runway locks       |
| Concurrency — semaphore      | `gates.c`                  | `sem_open`, `sem_wait`               |
| Data consistency             | `main.c` + `logger.c`      | mutex + atomic flock'd appends       |
| Sockets                      | `main.c`, clients          | `AF_UNIX`, `socket`, `accept`        |
| Wire framing                 | `protocol.c`               | `send_msg`, `recv_msg`               |
| IPC — named pipe             | `handoff.c`                | `mkfifo`, `handoff_post`             |
| IPC — signals                | `emergency.c`              | `sigaction`, `SIGUSR1`, `SIGUSR2`    |

---

## 5. Demonstration & Output

**Figure 1 — `01_server_startup.png`**
Server boot. Each subsystem prints a line as it initializes (logger,
gates, handoff FIFO, signal handlers, socket).

**Figure 2 — `02_radar_display.png`**
ASCII radar with aircraft as `+` symbols. Refreshes every 500ms from
server STATE broadcasts.

**Figure 3 — `03_radar_active_flight.png`**
Radar at a different point in the simulation — proves state evolves
and updates flow reliably.

**Figure 4 — `04_tower_clear_land.png`**
Tower issuing `CLEAR_LAND`. Server replies OK; runway mutex held for
2s during the command — that's the mutex critical section.

**Figure 5 — `05_ground_role_auth.png`**
Role-based auth in action. Ground denied a Tower-only command.

**Figure 6 — `06_signal_mayday.png`**
`kill -USR1` and `kill -USR2` to the server PID. Controllers receive
ALERTs even though signals never touched the socket.

**Figure 7 — `07_admin_view_logs.png`**
Admin running `VIEW_LOGS`. Flight log read under `flock(LOCK_SH)`.

**Figure 8 — `08_server_lifecycle.png`**
Server log showing the full sequence — connects, runway
acquire/release pairs, signal events handled.

---

## 6. Challenges and Solutions

**macOS doesn't support unnamed POSIX semaphores.** First version
used `sem_init()` — returns `ENOSYS` on macOS. Switched to
`sem_open()` with a name. Now portable across macOS and Linux.

**Stream sockets don't preserve message boundaries.** Two `write()`s
were occasionally getting concatenated into one `read()` and the
parser failed. Fixed by adding a 4-byte length prefix and looping
helpers (`read_all`/`write_all`).

**Signal handlers can deadlock on mutexes.** Early version called
`broadcast()` directly from the SIGUSR1 handler. If the main thread
was holding `g_clients_mtx` when the signal arrived, the handler
ran on the same thread and self-deadlocked. Fixed with the standard
flag-then-poll pattern — handler only sets a `volatile sig_atomic_t`,
broadcaster thread does the real work in normal code.

**Stale socket and FIFO files between runs.** If the server crashed,
`/tmp/atc.sock` and `/tmp/atc_handoff.fifo` were left behind, making
the next `bind()` fail with `EADDRINUSE`. Fixed with `unlink()` at
startup, before `bind` and `mkfifo`. Startup is now idempotent.

---

## 7. Conclusion

The simulator implements every required concept, each demonstrated
by a runnable scenario and a screenshot. 11 modular source files
across 3 directories.

What I actually learned:

- **Choice of primitive matters.** Mutex vs semaphore, named pipe
  vs signal, flock vs fcntl — none are interchangeable. Picking the
  right one is half the design problem.
- **Defensive defaults beat clever ones.** Default-deny in auth,
  `unlink` before `bind`, `O_NONBLOCK` when a reader might be
  missing — every one saved real debugging time.
- **POSIX is portable but not uniform.** macOS and Linux differ in
  real ways (unnamed semaphores being the most notable).
- **Logging is a feature, not an afterthought.** Running
  `tail -f logs/flights.log` alongside the simulator made
  debugging concurrency issues dramatically faster.

The system works, demonstrates every required concept, and could
realistically be extended (multiple airports, weather physics,
persistent state) without a ground-up rewrite.

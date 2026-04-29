# ATC Simulator — Project Report

**Course:** EGC 301P / Operating Systems Lab
**Project:** Multi-Controller Air Traffic Control Simulator
**Author:** Ajay Javali - Roll No: BT2024079
**Repository:** https://github.com/Ajay03299/ATC-Simulator

---

## 1. Problem Statement

I wanted to build something that genuinely needs every concept the
course covers, instead of bolting them onto a project that doesn't
really care about them. Air traffic control turned out to be a great
fit. Real ATC is one of the most safety-critical concurrent systems
out there — multiple human controllers manage hundreds of aircraft
through clearly separated zones, share a single source of truth for
state, enforce strict role boundaries, and have to handle emergencies
without losing data or crashing.

So the project is a small but realistic version of that. A central
server holds the aircraft state. Pilots, controllers, an admin, and a
read-only radar all connect to it as separate clients. Each client is
a separate process running in its own terminal. Between them they
exercise role-based authorization, file locking, mutexes, a semaphore,
data consistency, sockets, named pipes, and signals — all six
required concepts plus an extra IPC mechanism.

Everything runs locally. The "network" is a Unix domain socket and
the "different systems" are different terminal tabs. The project
guidelines explicitly allow this.

---

## 2. System Architecture

The big picture:
+-----------+      +-----------+      +-----------+
|  Pilots   |      | Controllers|      |   Admin   |
| (clients) |      | (clients)  |      | (client)  |
+-----+-----+      +-----+------+      +-----+-----+
|                  |                   |
|     AF_UNIX socket /tmp/atc.sock     |
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
/tmp/atc_handoff  logs/flights.log  logs/incidents.log
.fifo (named pipe)  (flock'd)         (flock'd)
|
+------+-----+
|   Radar    |
| (read only)|
+------------+

The server is multi-threaded. There's the accept loop in main, one
thread per connected client, a broadcaster thread that ticks every
500ms (sends STATE updates and drains pending signal flags), and a
handoff-reader thread that pulls events out of the named pipe.

Codebase layout:
src/
├── common/
│   ├── types.h          shared structs and constants
│   ├── protocol.h       wire protocol declarations
│   └── protocol.c       length-prefixed framed I/O
├── server/
│   ├── main.c           entry point, accept loop, command dispatch
│   ├── auth.{h,c}       role-based permission table
│   ├── runway.{h,c}     per-runway pthread mutexes
│   ├── gates.{h,c}      POSIX named semaphore for parking gates
│   ├── handoff.{h,c}    named-pipe IPC for controller handoffs
│   ├── logger.{h,c}     flock-protected flight and incident logs
│   └── emergency.{h,c}  SIGUSR1/SIGUSR2 handlers
└── clients/
├── pilot.c          pilot client
├── radar.c          ASCII radar display
└── controller.c     interactive controller REPL

---

## 3. How Each Required Concept Is Implemented

### 3.1 Role-based Authorization

**Where:** `src/server/auth.c`

The whole authorization model is one declarative table. Each row maps
a command to a bitmask of roles that can run it.

```c
static const Permission TABLE[] = {
    { "SPAWN",         R(ROLE_PILOT) },
    { "CLEAR_LAND",    R(ROLE_TOWER) | R(ROLE_ADMIN) },
    { "TAXI",          R(ROLE_GROUND) | R(ROLE_ADMIN) },
    { "VIEW_LOGS",     R(ROLE_ADMIN) },
    /* ... */
};
```

Every command coming into the server has to pass through
`is_authorized()` before anything happens:

```c
if (!is_authorized(role, verb)) {
    reply_err(fd, deny_reason(role, verb));
    continue;
}
```

The reason I went with this layout: policy is in the table,
enforcement is one line. Adding a new command is one new row. Unknown
commands are denied by default, which is the safe choice — you don't
want a typo to silently grant access.

You can see this in screenshot `05_ground_role_auth.png` — the Ground
controller tries to issue `CLEAR_LAND` and gets shot down because
that's a Tower-only command.

### 3.2 File Locking

**Where:** `src/server/logger.c`

Two log files: `logs/flights.log` for routine state changes,
`logs/incidents.log` for mayday, kill, and weather events. Both
written by lots of threads at once, both have to stay coherent.

I went with `flock()` over `fcntl()` for two reasons. First, flock
is whole-file — exactly the granularity I need for an append-only
log. Second, flock auto-releases when the fd is closed (or the
process dies), so no risk of a stuck lock if the server crashes mid-
write.

The write path:

```c
int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
flock(fd, LOCK_EX);                /* exclusive: blocks everyone */
/* format timestamped line, write, fsync */
flock(fd, LOCK_UN);
close(fd);
```

The read path (admin's `VIEW_LOGS`) uses the shared variant:

```c
flock(fd, LOCK_SH);  /* multiple readers OK, blocks writers */
```

That's the read/write lock distinction — many admins could run
`VIEW_LOGS` at the same time without blocking each other, but a
writer would still correctly exclude all of them.

The combination of `O_APPEND` + `flock(LOCK_EX)` + `fsync` means:
1. The kernel guarantees `O_APPEND` writes go atomically to end of
   file.
2. `flock` prevents two writers from interleaving even before the
   bytes hit the kernel.
3. `fsync` makes sure data is on disk before we release the lock.

Visible in `08_server_lifecycle.png` (full event log) and
`07_admin_view_logs.png` (admin reading it back).

### 3.3 Concurrency Control

#### 3.3.1 Mutexes

**Where:** `src/server/main.c`, `src/server/runway.c`

Three different mutexes for three different shared structures:

- `g_aircraft_mtx` protects the global aircraft table.
- `g_clients_mtx` protects the connected-clients list.
- An array of `NUM_RUNWAYS` mutexes (one per runway) lives in
  `runway.c`.

I deliberately didn't use a single global "server lock". Two threads
touching completely different data shouldn't have to wait for each
other. Per-runway locks also let two different runways be in use at
the same time, which is the whole point of having more than one
runway.

Here's what happens when Tower clears a landing:

```c
runway_acquire(rw, id);             /* pthread_mutex_lock - blocks */
aircraft_set_state(id, STATE_LANDING, ROLE_TOWER);
log_flight("RUNWAY_ACQUIRE ...");
sleep(2);                           /* simulates runway occupied */
runway_release(rw);                 /* unlock */
log_flight("RUNWAY_RELEASE ...");
```

A second `CLEAR_LAND` on the same runway during those two seconds
visibly blocks until the first one releases. I tested this and it
works exactly the way you'd expect.

#### 3.3.2 Semaphore

**Where:** `src/server/gates.c`

The 4 parking gates are a counting resource — any of them is
interchangeable, and we want up to 4 holders at once. That's exactly
what a semaphore is for.

```c
sem_open(GATE_SEM_NAME, O_CREAT | O_EXCL, 0644, NUM_GATES);
sem_wait(g_gate_sem);   /* decrement, block if zero */
sem_post(g_gate_sem);   /* increment, wake one waiter */
```

I used a **named** semaphore (`sem_open`) instead of an unnamed one
because macOS doesn't actually implement unnamed POSIX semaphores —
`sem_init` returns `ENOSYS`. Named semaphores work on both macOS and
Linux, so the code is portable.

`TAXI` calls `sem_wait`, `PUSHBACK` calls `sem_post`. When all four
gates are taken, the fifth `TAXI` blocks until somebody PUSHBACKs. I
verified this in the smoke test by issuing five TAXI commands and
watching the fifth controller terminal hang.

The short version of mutex versus semaphore: mutex is binary and
guards a single shared structure. Semaphore counts identical
resources. Different problem, different primitive.

### 3.4 Data Consistency

Data consistency isn't a single technique — it's what you get from
combining the other primitives correctly. The strategy here has three
parts:

1. **All shared in-memory state is mutex-protected.** Every read and
   write of the aircraft table happens while holding
   `g_aircraft_mtx`. No torn reads, no half-finished updates.

2. **State changes are paired with log writes.** Every
   `aircraft_set_state` call is followed by a `log_flight()` call.
   Even if the server crashes, the log either has the line or it
   doesn't — `flock` + `fsync` ensures the bytes are on disk before
   the next operation starts. No silent half-states.

3. **Append-only logs prevent lost updates.** Two threads writing to
   `flights.log` at the same time can't lose either write. The
   kernel guarantees `O_APPEND` writes go atomically to end of
   file, and the exclusive flock keeps the gap between "compute the
   line" and "actually write" exclusive too.

Concrete example — Tower clearing a landing:

- Take the runway mutex.
- Update `aircraft.state` while holding the aircraft mutex.
- Log `RUNWAY_ACQUIRE` under exclusive flock with fsync.
- Sleep (simulates runway occupied).
- Log `RUNWAY_RELEASE`.
- Drop the runway mutex.

If anything in this sequence crashes, the log on disk reflects
exactly how far we got, and the runway mutex is automatically
released by the kernel when the process dies. That's the kind of
invariant you can actually rely on.

### 3.5 Sockets — Client-Server Model

**Where:** `src/server/main.c`, the three clients, `src/common/protocol.c`

The server uses an `AF_UNIX` stream socket bound to `/tmp/atc.sock`.
Every client (pilot, radar, controller, admin) connects to the same
socket. The accept loop in main spawns one detached thread per
client.

One problem I hit early: stream sockets don't preserve message
boundaries. Two `write()`s from the client can come back as one
`read()` on the server, or get split across two reads. So I wrote a
length-prefixed wire protocol — every message starts with a 4-byte
big-endian length, then exactly that many payload bytes:
[4 bytes: htonl(N)] [N bytes: payload]

`send_msg` and `recv_msg` in `protocol.c` handle this. The receiver
reads exactly 4 bytes, decodes N, then reads exactly N more. Two
helpers, `read_all` and `write_all`, handle short reads, short
writes, and `EINTR` correctly.

Payload is plain text — `HELLO PILOT`, `CLEAR_LAND 1 0`,
`STATE |1,AI203,...|2,UA450,...`. Text is easy to debug — you can
slap a printf in temporarily and immediately see what's going over
the wire.

Visible in `01_server_startup.png`, `02_radar_display.png`, and
`04_tower_clear_land.png`.

### 3.6 Inter-Process Communication

The spec says "at least one IPC mechanism." I went with two because
they have very different semantics and the contrast is genuinely
useful to demo.

#### 3.6.1 Named Pipe — Controller Handoffs

**Where:** `src/server/handoff.c`

Created with `mkfifo("/tmp/atc_handoff.fifo", 0644)`. When a
controller hands an aircraft off (Approach to Tower, Tower to
Ground), the server writes one line to the FIFO:
HANDOFF 3 APPROACH TOWER

A dedicated reader thread `open()`s the FIFO and loops on `read()`,
parsing line by line and re-broadcasting each event as an `ALERT`
to all controllers and admin via the existing `broadcast()` helper.

Two implementation details worth mentioning:

- The writer opens with `O_NONBLOCK | O_WRONLY` so a missing reader
  doesn't deadlock the controller thread. If `errno == ENXIO` (no
  reader), I just drop the event silently. Handoff alerts are
  notifications, not critical messages.
- POSIX guarantees writes up to `PIPE_BUF` (at least 512 bytes) are
  atomic on a pipe. Our handoff lines are ~30 bytes, so simultaneous
  writes can't interleave.

Cool side effect: the FIFO is visible at the filesystem level. You
can do `ls -la /tmp/atc_handoff.fifo` and see the `p` permission bit,
or even `cat` the FIFO during the demo to watch raw events flow
through.

#### 3.6.2 Signals — Emergency and Weather

**Where:** `src/server/emergency.c`

The server installs handlers for two user-defined signals:

- `SIGUSR1` — mayday. Pilot or any operator can send this to the
  server PID. It bypasses the socket completely, which is the whole
  point — signals are an out-of-band channel that works even if the
  socket is jammed.
- `SIGUSR2` — weather alert.

The handlers themselves are tiny because POSIX is strict about what
you can call from a signal handler. No printf, no malloc, no
`pthread_mutex_lock` — only async-signal-safe functions. So:

```c
static volatile sig_atomic_t g_usr1_pending = 0;

static void on_sigusr1(int sig) {
    g_usr1_pending = 1;   /* literally that's it */
}
```

The actual work — broadcasting and writing to the incident log —
happens in `emergency_poll()`, which the broadcaster thread calls
once per tick (every 500ms). At that point we're back in normal
code where we can safely take mutexes and call non-async-signal-safe
functions.

The server writes its PID to `/tmp/atc_server.pid` on startup, so
any client can do:

```bash
kill -USR1 $(cat /tmp/atc_server.pid)
```

This is the standard Unix daemon idiom — same pattern as nginx and
Apache.

Visible in `06_signal_mayday.png`.

---

## 4. Concept ↔ Code Quick Reference

For evaluators looking for a specific concept:

| Concept                      | File                          | Key symbols                          |
|------------------------------|-------------------------------|--------------------------------------|
| Role-based authorization     | `src/server/auth.c`           | `is_authorized`, `TABLE[]`           |
| File locking — exclusive     | `src/server/logger.c`         | `append_line`, `flock(LOCK_EX)`      |
| File locking — shared        | `src/server/logger.c`         | `log_read_flights`, `flock(LOCK_SH)` |
| Concurrency — mutex          | `src/server/main.c`           | `g_aircraft_mtx`, `g_clients_mtx`    |
| Concurrency — runway mutexes | `src/server/runway.c`         | `runway_acquire`, `runway_release`   |
| Concurrency — semaphore      | `src/server/gates.c`          | `sem_open`, `gate_acquire`           |
| Data consistency             | `main.c` + `logger.c`         | mutex-coupled state + log writes     |
| Sockets                      | `src/server/main.c`           | `AF_UNIX`, `socket`, `bind`, `accept`|
| Wire framing                 | `src/common/protocol.c`       | `send_msg`, `recv_msg`               |
| IPC — named pipe             | `src/server/handoff.c`        | `mkfifo`, `handoff_post`             |
| IPC — signals                | `src/server/emergency.c`      | `sigaction`, `SIGUSR1`, `SIGUSR2`    |

---

## 5. Demonstration & Output

What each screenshot proves:

**Figure 1 — `01_server_startup.png`**
Server boot sequence. Each subsystem prints a line as it initializes:
logger opens both log files, gate semaphore initializes with 4 free
gates, handoff FIFO is created, signal handlers are installed, socket
starts listening. Confirms everything wires together cleanly.

**Figure 2 — `02_radar_display.png`**
ASCII radar showing aircraft as `+` symbols on a 60×20 grid, with a
table below listing each aircraft's ID, callsign, position,
altitude, fuel, and state. Refreshes every 500ms from server STATE
broadcasts.

**Figure 3 — `03_radar_active_flight.png`**
Second radar capture at a different point in the simulation. Proves
state actually changes over time and updates flow through the socket
reliably.

**Figure 4 — `04_tower_clear_land.png`**
Tower terminal issuing `CLEAR_LAND 1 0`. Server replies
`OK CLEAR_LAND id=1 runway=0`. The runway mutex is held for 2
seconds during this command — that's the mutex critical section in
action.

**Figure 5 — `05_ground_role_auth.png`**
Ground controller terminal demonstrating the role-based auth model.
Ground is denied permission to issue Tower-only commands.

**Figure 6 — `06_signal_mayday.png`**
A separate terminal sending `kill -USR1` and `kill -USR2` to the
server PID. The controller terminals receive ALERT broadcasts even
though the signals never touched the socket. This is the
signals → flag → poll → broadcast pipeline working end to end.

**Figure 7 — `07_admin_view_logs.png`**
Admin terminal running `VIEW_LOGS`. The flight log contents come
back having been read under `flock(LOCK_SH)` — the shared lock side
of the reader/writer model.

**Figure 8 — `08_server_lifecycle.png`**
Server log showing the full sequence of events during the demo —
client connects/disconnects, runway acquire/release pairs, signal
events handled. Complete picture of a typical session from the
server's view.

---

## 6. Challenges and Solutions

A handful of real engineering problems came up. Each one was a small
but useful lesson.

### 6.1 macOS doesn't support unnamed POSIX semaphores

First version used `sem_init()` for the gate semaphore. On macOS
this returns `ENOSYS` — the function is declared in
`<semaphore.h>` but isn't actually implemented. Switched to
`sem_open()` with a name (`/atc_gate_sem`). Named semaphores work
on both macOS and Linux, so the code is now portable.

### 6.2 Stream sockets don't preserve message boundaries

First iteration used `read()` and `write()` directly without
framing. Two pilot UPDATE messages occasionally got concatenated
into one read, and the server failed to parse them. Fix: a length-
prefixed wire protocol. Every message starts with 4 big-endian
bytes giving the payload length, and `read_all`/`write_all` loop
until the exact byte count is satisfied. After this change the
protocol became deterministic.

### 6.3 Signal handlers can deadlock on mutexes

Early version of `emergency.c` tried to call `broadcast()` directly
from the SIGUSR1 handler. Works most of the time, but if the main
thread happens to be holding `g_clients_mtx` when the signal
arrives, the handler runs on the same thread, tries to acquire the
same mutex, and the process self-deadlocks.

Fix: standard pattern. Handler only sets a `volatile sig_atomic_t`
flag. The broadcaster thread polls this flag once per tick and does
the actual work in normal code. Handler is now async-signal-safe
and the deadlock is impossible.

### 6.4 Stale socket and FIFO files between runs

If the server crashes (or `Ctrl+C` doesn't reach cleanup),
`/tmp/atc.sock` and `/tmp/atc_handoff.fifo` are left behind. The
next run's `bind()` then fails with `EADDRINUSE`, and `mkfifo()`
fails with `EEXIST`.

Fix: call `unlink()` on these paths at startup, before the
respective `bind`/`mkfifo`. Now startup is idempotent — server
runs cleanly regardless of how the previous run ended.

### 6.5 Heredocs and escape characters

Smaller issue but real: pasting Makefiles into the terminal with
heredocs occasionally caused the `<` in `$<` (Make's "first
prerequisite" variable) to be eaten by the shell, producing silent
build failures. During development I worked around this by writing
the Makefile through a Python helper or directly in nano, which
write raw bytes without shell interpretation.

---

## 7. Conclusion

The simulator implements every concept the spec requires, with each
one demonstrated by a runnable scenario and a screenshot. The
codebase is modular — 11 source files across 3 directories, a
well-defined wire protocol, and a single source of truth for
authorization rules.

What I actually learned from this, beyond "how each primitive works":

- **Choice of primitive matters.** Mutex versus semaphore, named
  pipe versus signal, `flock` versus `fcntl` — none of these are
  interchangeable. Picking the right one is half the design problem.

- **Defensive defaults beat clever ones.** Default-deny in the
  authorization table, `unlink` before `bind`, `O_NONBLOCK` when a
  reader might be missing — every one of these saved real debugging
  time later.

- **POSIX is portable but not uniform.** macOS and Linux differ in
  real ways (unnamed semaphores being the most notable). Sticking
  to the most portable subset is worth the small cost.

- **Logging is a feature, not an afterthought.** Having
  `tail -f logs/flights.log` running alongside the simulator made
  debugging concurrency issues dramatically faster than printf-
  driven development would have been.

The system works, demonstrates every required concept, and could
realistically be extended (multiple airports, weather physics,
persistent state, ILS approach cones) without a ground-up rewrite.

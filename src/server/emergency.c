#include "emergency.h"
#include "../common/types.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

/*
* Two flags set by signal handlers, drained by emergency_poll().
*
* volatile sig_atomic_t is the ONLY type POSIX guarantees you can
* read and write safely from a handler. Any other type (including
* plain int) can be interrupted mid-word on some architectures.
*/
static volatile sig_atomic_t g_usr1_pending = 0; /* mayday */
static volatile sig_atomic_t g_usr2_pending = 0; /* weather */

/* broadcast() lives in main.c. We call it from emergency_poll() to
* fan out alerts, same pattern as handoff.c. */
extern void broadcast(const char *msg, int role_mask);

/* ---- Signal handlers (minimal - just set a flag) ----- */

static void on_sigusr1(int sig) {
(void)sig;
g_usr1_pending = 1;
/* That's it. No printf, no broadcast, no mutex. Doing any of
* those from a handler would be undefined behavior. */
}

static void on_sigusr2(int sig) {
(void)sig;
g_usr2_pending = 1;
}

/* ---- Setup ------ */

void emergency_init(void) {
/* Install handlers with sigaction. SA_RESTART auto-restarts
* interrupted syscalls (like read()) which is usually what you
* want. Without it, EVERY slow syscall in EVERY thread would
* have to check for EINTR. */
struct sigaction sa = {0};
sa.sa_flags = SA_RESTART;

sa.sa_handler = on_sigusr1;
if (sigaction(SIGUSR1, &sa, NULL) < 0) {
perror("[emergency] sigaction SIGUSR1");
exit(1);
}

sa.sa_handler = on_sigusr2;
if (sigaction(SIGUSR2, &sa, NULL) < 0) {
perror("[emergency] sigaction SIGUSR2");
exit(1);
}

/* Write our PID to /tmp/atc_server.pid so pilots can signal us.
* Standard Unix daemon pattern. */
int fd = open(PID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
if (fd < 0) {
perror("[emergency] open pidfile");
exit(1);
}
char pidstr[32];
int n = snprintf(pidstr, sizeof(pidstr), "%d\n", (int)getpid());
if (write(fd, pidstr, (size_t)n) < 0) {
perror("[emergency] write pidfile");
}
close(fd);

printf("[emergency] signal handlers installed. PID=%d written to %s\n",
(int)getpid(), PID_FILE);
printf("[emergency] pilot mayday: kill -USR1 $(cat %s)\n", PID_FILE);
printf("[emergency] weather alert: kill -USR2 $(cat %s)\n", PID_FILE);
}

void emergency_destroy(void) {
unlink(PID_FILE);
}

/* ---- Poll (called from main loop tick) ------ */

void emergency_poll(void) {
/* Check and clear USR1 atomically.
* Since sig_atomic_t reads are atomic, we snapshot then clear. */
if (g_usr1_pending) {
g_usr1_pending = 0;

/* Broadcast a mayday alert to all controllers + admin.
* We don't know WHICH aircraft - the signal has no payload.
* In a real system the pilot would also have set a state
* flag we could read; here we just announce incoming mayday. */
const char *msg = "ALERT MAYDAY_SIGNAL received - check all aircraft";
int mask = (1 << ROLE_TOWER) | (1 << ROLE_GROUND) |
(1 << ROLE_APPROACH) | (1 << ROLE_ADMIN);
broadcast(msg, mask);

/* Log the incident under flock(LOCK_EX). */
log_incident("MAYDAY_SIGNAL received via SIGUSR1");

printf("[emergency] SIGUSR1 mayday handled\n");
}

if (g_usr2_pending) {
g_usr2_pending = 0;

const char *msg = "ALERT WEATHER severe conditions - ground stop advised";
int mask = (1 << ROLE_TOWER) | (1 << ROLE_GROUND) |
(1 << ROLE_APPROACH);
broadcast(msg, mask);

log_incident("WEATHER alert broadcast via SIGUSR2");

printf("[emergency] SIGUSR2 weather alert handled\n");
}
}

/*
* ATC Server - Day 2 integration
*
* Responsibilities:
* - Unix domain socket listener on SOCKET_PATH
* - Thread-per-client accept loop
* - Shared aircraft table protected by a mutex (CONCURRENCY CONTROL)
* - HELLO handshake with role identification
* - Dispatch of pilot and controller commands
* - Role-based authorization (auth.c) on every mutating command
* - Runway mutexes (runway.c) on CLEAR_LAND / CLEAR_TAKEOFF
* - Gate semaphore (gates.c) on TAXI / PUSHBACK
* - Handoff notifications via named pipe (handoff.c)
* - STATE broadcasts to radar/controller clients
* - Clean shutdown on SIGINT (closes socket, unlinks path)
*
* Day 3 will add: file locking on flight log, full signal-based
* mayday, weather alerts via SIGUSR2.
*/

#include "../common/types.h"
#include "../common/protocol.h"
#include "auth.h"
#include "runway.h"
#include "gates.h"
#include "handoff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <time.h>

/* ---- Global state (all access guarded) --- */

static Aircraft g_aircraft[MAX_AIRCRAFT];
static int g_aircraft_count = 0;
static int g_next_id = 1;
static pthread_mutex_t g_aircraft_mtx = PTHREAD_MUTEX_INITIALIZER;

static int g_client_fds[MAX_CLIENTS];
static Role g_client_roles[MAX_CLIENTS];
static int g_client_count = 0;
static pthread_mutex_t g_clients_mtx = PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t g_shutdown = 0;
static int g_listen_fd = -1;

/* ---- Signal handling --- */

static void on_sigint(int sig) {
(void)sig;
g_shutdown = 1;
if (g_listen_fd >= 0) close(g_listen_fd);
}

/* ---- Client registry ---- */

static void register_client(int fd, Role r) {
pthread_mutex_lock(&g_clients_mtx);
if (g_client_count < MAX_CLIENTS) {
g_client_fds[g_client_count] = fd;
g_client_roles[g_client_count] = r;
g_client_count++;
}
pthread_mutex_unlock(&g_clients_mtx);
}

static void unregister_client(int fd) {
pthread_mutex_lock(&g_clients_mtx);
for (int i = 0; i < g_client_count; i++) {
if (g_client_fds[i] == fd) {
g_client_fds[i] = g_client_fds[g_client_count - 1];
g_client_roles[i] = g_client_roles[g_client_count - 1];
g_client_count--;
break;
}
}
pthread_mutex_unlock(&g_clients_mtx);
}

/* Non-static: handoff.c calls this via extern declaration. */
void broadcast(const char *msg, int role_mask) {
pthread_mutex_lock(&g_clients_mtx);
for (int i = 0; i < g_client_count; i++) {
if (role_mask == -1 || (role_mask & (1 << g_client_roles[i]))) {
send_msg(g_client_fds[i], msg);
}
}
pthread_mutex_unlock(&g_clients_mtx);
}

/* ---- Aircraft table operations (mutex-protected) ----- */

static int aircraft_spawn(const char *callsign, int altitude) {
pthread_mutex_lock(&g_aircraft_mtx);
if (g_aircraft_count >= MAX_AIRCRAFT) {
pthread_mutex_unlock(&g_aircraft_mtx);
return -1;
}
Aircraft *a = &g_aircraft[g_aircraft_count++];
memset(a, 0, sizeof(*a));
a->id = g_next_id++;
strncpy(a->callsign, callsign, CALLSIGN_LEN - 1);
a->callsign[CALLSIGN_LEN - 1] = '\0';
a->x = rand() % RADAR_WIDTH;
a->y = rand() % RADAR_HEIGHT;
a->altitude = altitude;
a->fuel = 100;
a->state = STATE_APPROACHING;
a->controller = ROLE_APPROACH;
a->runway = -1;
a->gate = -1;
a->last_update = time(NULL);
int id = a->id;
pthread_mutex_unlock(&g_aircraft_mtx);
return id;
}

static int aircraft_update(int id, int x, int y, int alt, int fuel) {
pthread_mutex_lock(&g_aircraft_mtx);
for (int i = 0; i < g_aircraft_count; i++) {
if (g_aircraft[i].id == id) {
g_aircraft[i].x = x;
g_aircraft[i].y = y;
g_aircraft[i].altitude = alt;
g_aircraft[i].fuel = fuel;
g_aircraft[i].last_update = time(NULL);
pthread_mutex_unlock(&g_aircraft_mtx);
return 0;
}
}
pthread_mutex_unlock(&g_aircraft_mtx);
return -1;
}

static int aircraft_mayday(int id) {
pthread_mutex_lock(&g_aircraft_mtx);
for (int i = 0; i < g_aircraft_count; i++) {
if (g_aircraft[i].id == id) {
g_aircraft[i].emergency = 1;
g_aircraft[i].state = STATE_EMERGENCY;
pthread_mutex_unlock(&g_aircraft_mtx);
return 0;
}
}
pthread_mutex_unlock(&g_aircraft_mtx);
return -1;
}

static int aircraft_set_state(int id, FlightState s, Role owner) {
pthread_mutex_lock(&g_aircraft_mtx);
for (int i = 0; i < g_aircraft_count; i++) {
if (g_aircraft[i].id == id) {
g_aircraft[i].state = s;
g_aircraft[i].controller = owner;
pthread_mutex_unlock(&g_aircraft_mtx);
return 0;
}
}
pthread_mutex_unlock(&g_aircraft_mtx);
return -1;
}

static int aircraft_remove(int id) {
pthread_mutex_lock(&g_aircraft_mtx);
for (int i = 0; i < g_aircraft_count; i++) {
if (g_aircraft[i].id == id) {
g_aircraft[i] = g_aircraft[g_aircraft_count - 1];
g_aircraft_count--;
pthread_mutex_unlock(&g_aircraft_mtx);
return 0;
}
}
pthread_mutex_unlock(&g_aircraft_mtx);
return -1;
}

static void aircraft_snapshot(char *buf, size_t bufsz) {
size_t off = 0;
off += snprintf(buf + off, bufsz - off, "STATE");
pthread_mutex_lock(&g_aircraft_mtx);
for (int i = 0; i < g_aircraft_count && off < bufsz - 64; i++) {
Aircraft *a = &g_aircraft[i];
off += snprintf(buf + off, bufsz - off,
" |%d,%s,%d,%d,%d,%d,%s,%d",
a->id, a->callsign, a->x, a->y, a->altitude, a->fuel,
state_name(a->state), a->emergency);
}
pthread_mutex_unlock(&g_aircraft_mtx);
}

/* ---- Command dispatch helpers ---- */

/* Tokenize: pulls the first whitespace-separated word out of buf,
* returns a pointer to the rest. Mutates buf. */
static char *take_verb(char *buf) {
char *sp = strchr(buf, ' ');
if (sp) { *sp = '\0'; return sp + 1; }
return buf + strlen(buf);
}

/* Reply helper. */
static void reply_ok(int fd, const char *msg) {
char out[MSG_MAX];
snprintf(out, sizeof(out), "OK %s", msg);
send_msg(fd, out);
}
static void reply_err(int fd, const char *msg) {
char out[MSG_MAX];
snprintf(out, sizeof(out), "ERR %s", msg);
send_msg(fd, out);
}

/* ---- Per-client handler thread ----- */

typedef struct {
int fd;
Role role;
} ClientCtx;

static void *client_thread(void *arg) {
ClientCtx *ctx = (ClientCtx *)arg;
int fd = ctx->fd;
Role role = ctx->role;
free(ctx);

char buf[MSG_MAX];
ssize_t n;

while ((n = recv_msg(fd, buf, sizeof(buf))) > 0) {
/* Copy for parsing; we want the original for logging. */
char work[MSG_MAX];
strncpy(work, buf, sizeof(work));
work[sizeof(work) - 1] = '\0';

char *verb = work;
char *args = take_verb(work);

/* Role-based authorization - deny early if role can't do this. */
if (!is_authorized(role, verb)) {
reply_err(fd, deny_reason(role, verb));
continue;
}

/* ---- Pilot commands ---- */
if (!strcmp(verb, "SPAWN")) {
char callsign[CALLSIGN_LEN]; int alt;
if (sscanf(args, "%15s %d", callsign, &alt) != 2) {
reply_err(fd, "bad SPAWN args"); continue;
}
int id = aircraft_spawn(callsign, alt);
if (id < 0) { reply_err(fd, "airspace full"); continue; }
char ok[64]; snprintf(ok, sizeof(ok), "%d", id);
reply_ok(fd, ok);
printf("[server] SPAWN %s id=%d alt=%d\n", callsign, id, alt);
}
else if (!strcmp(verb, "UPDATE")) {
int id, x, y, alt, fuel;
if (sscanf(args, "%d %d %d %d %d", &id, &x, &y, &alt, &fuel) != 5) {
reply_err(fd, "bad UPDATE args"); continue;
}
aircraft_update(id, x, y, alt, fuel);
}
else if (!strcmp(verb, "MAYDAY")) {
int id = atoi(args);
if (aircraft_mayday(id) == 0) {
char alert[64];
snprintf(alert, sizeof(alert), "ALERT MAYDAY %d", id);
int mask = (1 << ROLE_TOWER) | (1 << ROLE_GROUND) |
(1 << ROLE_APPROACH) | (1 << ROLE_ADMIN);
broadcast(alert, mask);
printf("[server] !! MAYDAY id=%d\n", id);
} else reply_err(fd, "unknown aircraft");
}

/* ---- Tower commands: runway critical section ---- */
else if (!strcmp(verb, "CLEAR_LAND") || !strcmp(verb, "CLEAR_TAKEOFF")) {
int id, rw;
if (sscanf(args, "%d %d", &id, &rw) != 2) {
reply_err(fd, "bad args"); continue;
}
if (runway_acquire(rw, id) < 0) {
reply_err(fd, "invalid runway"); continue;
}
FlightState s = (!strcmp(verb, "CLEAR_LAND")) ? STATE_LANDING : STATE_TAKEOFF;
if (aircraft_set_state(id, s, ROLE_TOWER) < 0) {
runway_release(rw);
reply_err(fd, "unknown aircraft"); continue;
}
char ok[64]; snprintf(ok, sizeof(ok), "%s id=%d runway=%d", verb, id, rw);
reply_ok(fd, ok);

/* Simulate runway occupancy for a couple seconds, then release.
* Done inline so the demo shows contention clearly. */
sleep(2);
runway_release(rw);
}
else if (!strcmp(verb, "HANDOFF_GROUND")) {
int id = atoi(args);
if (aircraft_set_state(id, STATE_TAXIING_IN, ROLE_GROUND) < 0) {
reply_err(fd, "unknown aircraft"); continue;
}
handoff_post(id, ROLE_TOWER, ROLE_GROUND);
reply_ok(fd, "handoff posted");
}

/* ---- Ground commands: gate semaphore ---- */
else if (!strcmp(verb, "TAXI")) {
int id, gate;
if (sscanf(args, "%d %d", &id, &gate) != 2) {
reply_err(fd, "bad TAXI args"); continue;
}
/* Block until a gate slot is free. */
if (gate_acquire() < 0) { reply_err(fd, "gate_acquire failed"); continue; }
aircraft_set_state(id, STATE_PARKED, ROLE_GROUND);
char ok[64]; snprintf(ok, sizeof(ok), "TAXI id=%d gate=%d", id, gate);
reply_ok(fd, ok);
}
else if (!strcmp(verb, "PUSHBACK")) {
int id = atoi(args);
aircraft_set_state(id, STATE_TAXIING_OUT, ROLE_GROUND);
gate_release();
reply_ok(fd, "pushback complete");
}
else if (!strcmp(verb, "HANDOFF_TOWER_FROM_GROUND")) {
int id = atoi(args);
aircraft_set_state(id, STATE_TAXIING_OUT, ROLE_TOWER);
handoff_post(id, ROLE_GROUND, ROLE_TOWER);
reply_ok(fd, "handoff posted");
}

/* ---- Approach commands ---- */
else if (!strcmp(verb, "VECTOR")) {
int id, hdg;
if (sscanf(args, "%d %d", &id, &hdg) != 2) {
reply_err(fd, "bad VECTOR args"); continue;
}
char ok[64]; snprintf(ok, sizeof(ok), "VECTOR id=%d heading=%d", id, hdg);
reply_ok(fd, ok);
}
else if (!strcmp(verb, "HANDOFF_TOWER")) {
int id = atoi(args);
aircraft_set_state(id, STATE_LANDING, ROLE_TOWER);
handoff_post(id, ROLE_APPROACH, ROLE_TOWER);
reply_ok(fd, "handoff posted");
}

/* ---- Admin commands ---- */
else if (!strcmp(verb, "KILL")) {
int id = atoi(args);
if (aircraft_remove(id) == 0) reply_ok(fd, "aircraft removed");
else reply_err(fd, "unknown aircraft");
}
else if (!strcmp(verb, "WEATHER")) {
char alert[MSG_MAX];
snprintf(alert, sizeof(alert), "ALERT WEATHER %s", args);
int mask = (1 << ROLE_TOWER) | (1 << ROLE_GROUND) |
(1 << ROLE_APPROACH);
broadcast(alert, mask);
reply_ok(fd, "weather broadcast");
}
else if (!strcmp(verb, "VIEW_LOGS")) {
/* Day 3 wires this up with flock + log file read. */
reply_ok(fd, "logs view coming on Day 3");
}

else if (!strcmp(verb, "PING")) {
send_msg(fd, "PONG");
}
else {
reply_err(fd, "unknown command");
}
}

printf("[server] client fd=%d (%s) disconnected\n", fd, role_name(role));
unregister_client(fd);
close(fd);
return NULL;
}

/* ---- Broadcaster thread ----- */

static void *broadcaster_thread(void *arg) {
(void)arg;
char snap[MSG_MAX];
while (!g_shutdown) {
aircraft_snapshot(snap, sizeof(snap));
int mask = (1 << ROLE_RADAR) | (1 << ROLE_ADMIN) |
(1 << ROLE_TOWER) | (1 << ROLE_GROUND) |
(1 << ROLE_APPROACH);
broadcast(snap, mask);
usleep(500 * 1000);
}
return NULL;
}

/* ---- main -----*/

int main(void) {
srand((unsigned)time(NULL));

struct sigaction sa = {0};
sa.sa_handler = on_sigint;
sigaction(SIGINT, &sa, NULL);
signal(SIGPIPE, SIG_IGN);

/* Initialize sync primitives and IPC. */
runway_init();
gates_init();
handoff_init();
handoff_start_reader();

/* Socket setup. */
unlink(SOCKET_PATH);
g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
if (g_listen_fd < 0) { perror("socket"); return 1; }

struct sockaddr_un addr = {0};
addr.sun_family = AF_UNIX;
strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
perror("bind"); return 1;
}
if (listen(g_listen_fd, 8) < 0) { perror("listen"); return 1; }
printf("[server] ATC online at %s\n", SOCKET_PATH);

pthread_t bcast_tid;
pthread_create(&bcast_tid, NULL, broadcaster_thread, NULL);
pthread_detach(bcast_tid);

while (!g_shutdown) {
int cfd = accept(g_listen_fd, NULL, NULL);
if (cfd < 0) {
if (g_shutdown) break;
if (errno == EINTR) continue;
perror("accept");
break;
}

char hello[MSG_MAX];
ssize_t hn = recv_msg(cfd, hello, sizeof(hello));
if (hn <= 0 || strncmp(hello, "HELLO ", 6) != 0) {
send_msg(cfd, "ERR expected HELLO");
close(cfd);
continue;
}
Role r = parse_role(hello + 6);
if (r == ROLE_UNKNOWN) {
send_msg(cfd, "ERR unknown role");
close(cfd);
continue;
}
send_msg(cfd, "OK welcome");
printf("[server] client fd=%d connected as %s\n", cfd, role_name(r));

register_client(cfd, r);

ClientCtx *ctx = malloc(sizeof(*ctx));
ctx->fd = cfd;
ctx->role = r;
pthread_t tid;
pthread_create(&tid, NULL, client_thread, ctx);
pthread_detach(tid);
}

printf("\n[server] shutting down\n");
handoff_destroy();
gates_destroy();
runway_destroy();
unlink(SOCKET_PATH);
return 0;
}

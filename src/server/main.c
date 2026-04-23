/*
 * ATC Server - Day 1 skeleton
 *
 * Responsibilities implemented in this file:
 *   - Unix domain socket listener on SOCKET_PATH
 *   - Thread-per-client accept loop
 *   - Shared aircraft table protected by a mutex (CONCURRENCY CONTROL)
 *   - HELLO handshake with role identification
 *   - SPAWN / UPDATE / MAYDAY handling from pilots
 *   - STATE broadcasts to radar/controller clients
 *   - Clean shutdown on SIGINT (closes socket, unlinks path)
 *
 * Day 2 will add: role-based command authorization, runway mutexes,
 *                 gate semaphore, handoff logic via named pipe.
 * Day 3 will add: file locking on flight log, full signal-based mayday,
 *                 weather alerts via SIGUSR2.
 */

#include "../common/types.h"
#include "../common/protocol.h"

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

/* ---- Global state (all access guarded) ------------------------------- */

static Aircraft       g_aircraft[MAX_AIRCRAFT];
static int            g_aircraft_count = 0;
static int            g_next_id = 1;
static pthread_mutex_t g_aircraft_mtx = PTHREAD_MUTEX_INITIALIZER;

static int            g_client_fds[MAX_CLIENTS];
static Role           g_client_roles[MAX_CLIENTS];
static int            g_client_count = 0;
static pthread_mutex_t g_clients_mtx = PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t g_shutdown = 0;
static int g_listen_fd = -1;

/* ---- Signal handling ------------------------------------------------- */

static void on_sigint(int sig) {
    (void)sig;
    g_shutdown = 1;
    /* Closing the listen fd unblocks accept() in the main thread. */
    if (g_listen_fd >= 0) close(g_listen_fd);
}

/* ---- Client registry ------------------------------------------------- */

static void register_client(int fd, Role r) {
    pthread_mutex_lock(&g_clients_mtx);
    if (g_client_count < MAX_CLIENTS) {
        g_client_fds[g_client_count]   = fd;
        g_client_roles[g_client_count] = r;
        g_client_count++;
    }
    pthread_mutex_unlock(&g_clients_mtx);
}

static void unregister_client(int fd) {
    pthread_mutex_lock(&g_clients_mtx);
    for (int i = 0; i < g_client_count; i++) {
        if (g_client_fds[i] == fd) {
            g_client_fds[i]   = g_client_fds[g_client_count - 1];
            g_client_roles[i] = g_client_roles[g_client_count - 1];
            g_client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_mtx);
}

/* Broadcast a message to all clients whose role matches any bit in mask.
 * Pass -1 to broadcast to everyone. */
static void broadcast(const char *msg, int role_mask) {
    pthread_mutex_lock(&g_clients_mtx);
    for (int i = 0; i < g_client_count; i++) {
        if (role_mask == -1 || (role_mask & (1 << g_client_roles[i]))) {
            send_msg(g_client_fds[i], msg);
        }
    }
    pthread_mutex_unlock(&g_clients_mtx);
}

/* ---- Aircraft table operations (mutex-protected) --------------------- */

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

/* Build a snapshot string of the aircraft table. Caller provides buffer. */
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

/* ---- Per-client handler thread --------------------------------------- */

typedef struct {
    int  fd;
    Role role;
} ClientCtx;

static void *client_thread(void *arg) {
    ClientCtx *ctx = (ClientCtx *)arg;
    int  fd   = ctx->fd;
    Role role = ctx->role;
    free(ctx);

    char buf[MSG_MAX];
    ssize_t n;

    while ((n = recv_msg(fd, buf, sizeof(buf))) > 0) {
        /* Simple command dispatch. Day 2 will move this to auth.c and
         * add role-based validation. For now we just handle the basics. */

        if (!strncmp(buf, "SPAWN ", 6)) {
            if (role != ROLE_PILOT) {
                send_msg(fd, "ERR only pilots can SPAWN");
                continue;
            }
            char callsign[CALLSIGN_LEN];
            int alt;
            if (sscanf(buf + 6, "%15s %d", callsign, &alt) != 2) {
                send_msg(fd, "ERR bad SPAWN args");
                continue;
            }
            int id = aircraft_spawn(callsign, alt);
            if (id < 0) {
                send_msg(fd, "ERR airspace full");
            } else {
                char ok[64];
                snprintf(ok, sizeof(ok), "OK %d", id);
                send_msg(fd, ok);
                printf("[server] SPAWN %s id=%d alt=%d\n", callsign, id, alt);
            }
        }
        else if (!strncmp(buf, "UPDATE ", 7)) {
            int id, x, y, alt, fuel;
            if (sscanf(buf + 7, "%d %d %d %d %d", &id, &x, &y, &alt, &fuel) != 5) {
                send_msg(fd, "ERR bad UPDATE args");
                continue;
            }
            aircraft_update(id, x, y, alt, fuel);
            /* No reply - high frequency. */
        }
        else if (!strncmp(buf, "MAYDAY ", 7)) {
            int id = atoi(buf + 7);
            if (aircraft_mayday(id) == 0) {
                char alert[64];
                snprintf(alert, sizeof(alert), "ALERT MAYDAY %d", id);
                /* Broadcast to all controllers + admin. */
                int mask = (1 << ROLE_TOWER) | (1 << ROLE_GROUND) |
                           (1 << ROLE_APPROACH) | (1 << ROLE_ADMIN);
                broadcast(alert, mask);
                printf("[server] !! MAYDAY id=%d\n", id);
            } else {
                send_msg(fd, "ERR unknown aircraft");
            }
        }
        else if (!strncmp(buf, "PING", 4)) {
            send_msg(fd, "PONG");
        }
        else {
            send_msg(fd, "ERR unknown command");
        }
    }

    printf("[server] client fd=%d (%s) disconnected\n", fd, role_name(role));
    unregister_client(fd);
    close(fd);
    return NULL;
}

/* ---- Broadcaster thread: push STATE to radar/admin every 500ms ------- */

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

/* ---- main ------------------------------------------------------------ */

int main(void) {
    srand((unsigned)time(NULL));

    /* Install SIGINT handler using sigaction (safer than signal()). */
    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);
    /* Ignore SIGPIPE so a dead client doesn't kill the server. */
    signal(SIGPIPE, SIG_IGN);

    /* Set up the Unix domain socket. */
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

    /* Start the broadcaster thread. */
    pthread_t bcast_tid;
    pthread_create(&bcast_tid, NULL, broadcaster_thread, NULL);
    pthread_detach(bcast_tid);

    /* Accept loop. */
    while (!g_shutdown) {
        int cfd = accept(g_listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (g_shutdown) break;
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        /* Handshake: first message must be HELLO <ROLE> */
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
    unlink(SOCKET_PATH);
    return 0;
}

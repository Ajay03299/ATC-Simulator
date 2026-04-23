/*
 * Controller client
 *
 * Usage:
 *     ./controller --role=tower
 *     ./controller --role=ground
 *     ./controller --role=approach
 *     ./controller --role=admin
 *
 * An interactive terminal client for a human controller. Commands the
 * user types are sent to the server. In parallel, a reader thread
 * prints incoming STATE snapshots and ALERT broadcasts.
 *
 * Role-based authorization is enforced server-side (see auth.c). This
 * client can type whatever, but the server will reply "ERR ..." if
 * the role isn't allowed to issue that command.
 *
 * Available commands (examples):
 *     CLEAR_LAND <aircraft_id> <runway>     (tower)
 *     CLEAR_TAKEOFF <aircraft_id> <runway>  (tower)
 *     TAXI <aircraft_id> <gate>             (ground)
 *     VECTOR <aircraft_id> <heading>        (approach)
 *     HANDOFF_TOWER <aircraft_id>           (approach)
 *     VIEW_LOGS                             (admin)
 *     KILL <aircraft_id>                    (admin)
 *     WEATHER <message>                     (admin)
 *     PING
 *     help
 *     quit
 */

#include "../common/types.h"
#include "../common/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

static volatile sig_atomic_t g_stop = 0;
static void on_int(int s) { (void)s; g_stop = 1; }

/* Show the last STATE snapshot only occasionally - otherwise the
 * controller's typed input gets interleaved with radar spam. */
static int g_show_state = 0;
static pthread_mutex_t g_out_mtx = PTHREAD_MUTEX_INITIALIZER;

static void print_help(Role r) {
    printf("\nCommands for role %s:\n", role_name(r));
    switch (r) {
    case ROLE_TOWER:
        printf("  CLEAR_LAND <id> <runway>      clear aircraft to land\n");
        printf("  CLEAR_TAKEOFF <id> <runway>   clear aircraft for takeoff\n");
        printf("  HANDOFF_GROUND <id>           hand aircraft over to Ground\n");
        break;
    case ROLE_GROUND:
        printf("  TAXI <id> <gate>              taxi aircraft to a gate\n");
        printf("  PUSHBACK <id>                 pushback from gate\n");
        printf("  HANDOFF_TOWER_FROM_GROUND <id> hand aircraft over to Tower\n");
        break;
    case ROLE_APPROACH:
        printf("  VECTOR <id> <heading>         give aircraft a heading\n");
        printf("  HANDOFF_TOWER <id>            hand aircraft over to Tower\n");
        break;
    case ROLE_ADMIN:
        printf("  VIEW_LOGS                     read flight log (Day 3)\n");
        printf("  KILL <id>                     remove an aircraft\n");
        printf("  WEATHER <message>             broadcast weather alert\n");
        printf("  (admin can issue any controller command too)\n");
        break;
    default: break;
    }
    printf("  PING                          liveness check\n");
    printf("  state on|off                  toggle live STATE display\n");
    printf("  help                          this message\n");
    printf("  quit                          disconnect\n\n");
}

/* Reader thread: prints anything the server sends us. */
static void *reader_thread(void *arg) {
    int fd = *(int *)arg;
    char buf[MSG_MAX];
    ssize_t n;
    while ((n = recv_msg(fd, buf, sizeof(buf))) > 0) {
        pthread_mutex_lock(&g_out_mtx);
        if (!strncmp(buf, "STATE", 5)) {
            if (g_show_state) {
                printf("\r\033[2K[state] %s\n> ", buf + 6);
                fflush(stdout);
            }
        } else if (!strncmp(buf, "ALERT", 5)) {
            /* \r\033[2K = return to column 0 and clear the line, so we
             * don't clobber whatever the user is typing. */
            printf("\r\033[2K*** %s ***\n> ", buf);
            fflush(stdout);
        } else if (!strncmp(buf, "OK", 2) || !strncmp(buf, "ERR", 3)) {
            printf("\r\033[2K[server] %s\n> ", buf);
            fflush(stdout);
        } else {
            printf("\r\033[2K[server] %s\n> ", buf);
            fflush(stdout);
        }
        pthread_mutex_unlock(&g_out_mtx);
    }
    return NULL;
}

int main(int argc, char **argv) {
    Role role = ROLE_UNKNOWN;

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--role=", 7)) {
            role = parse_role(argv[i] + 7);
        }
    }
    if (role == ROLE_UNKNOWN ||
        role == ROLE_PILOT || role == ROLE_RADAR) {
        fprintf(stderr,
            "usage: %s --role={tower|ground|approach|admin}\n", argv[0]);
        return 1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_int;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Connect and handshake. */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); return 1;
    }

    char hello[64];
    snprintf(hello, sizeof(hello), "HELLO %s", role_name(role));
    send_msg(fd, hello);

    char reply[MSG_MAX];
    if (recv_msg(fd, reply, sizeof(reply)) <= 0 ||
        strncmp(reply, "OK", 2) != 0) {
        fprintf(stderr, "handshake failed: %s\n", reply);
        return 1;
    }
    printf("Connected as %s. Type 'help' for commands.\n", role_name(role));

    /* Start reader thread. */
    pthread_t rt;
    pthread_create(&rt, NULL, reader_thread, &fd);
    pthread_detach(rt);

    /* Interactive REPL. */
    char line[MSG_MAX];
    while (!g_stop) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        /* strip trailing newline */
        size_t L = strlen(line);
        while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r'))
            line[--L] = '\0';
        if (L == 0) continue;

        /* Client-side commands (don't hit the server) */
        if (!strcmp(line, "help")) { print_help(role); continue; }
        if (!strcmp(line, "quit") || !strcmp(line, "exit")) break;
        if (!strcmp(line, "state on"))  { g_show_state = 1; continue; }
        if (!strcmp(line, "state off")) { g_show_state = 0; continue; }

        /* Everything else is shipped to the server. */
        if (send_msg(fd, line) < 0) {
            fprintf(stderr, "[controller] server gone\n");
            break;
        }

        /* Short pause so the reader thread has a chance to print the
         * reply before we print the next prompt. UX, not correctness. */
        usleep(100 * 1000);
    }

    close(fd);
    printf("\n[controller] disconnected\n");
    return 0;
}

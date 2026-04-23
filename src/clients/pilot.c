/*
 * Pilot client - Day 1 skeleton
 *
 * Usage: ./pilot <callsign> [starting_altitude]
 *
 * Flies a simple descending path across the radar grid, sending UPDATE
 * messages at 2Hz. Press Ctrl+\ (SIGQUIT) to declare a mayday - this
 * sends a MAYDAY message to the server. On Day 3 we'll also wire this
 * to SIGUSR1 sent to the server process for the "signals as IPC" demo.
 */

#include "../common/types.h"
#include "../common/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

static volatile sig_atomic_t g_mayday = 0;
static volatile sig_atomic_t g_stop   = 0;

static void on_quit(int sig) { (void)sig; g_mayday = 1; }
static void on_int(int sig)  { (void)sig; g_stop = 1; }

static int connect_server(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <callsign> [altitude]\n", argv[0]);
        return 1;
    }
    const char *callsign = argv[1];
    int altitude = (argc >= 3) ? atoi(argv[2]) : 10000;

    struct sigaction sa = {0};
    sa.sa_handler = on_quit;
    sigaction(SIGQUIT, &sa, NULL);
    sa.sa_handler = on_int;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    srand((unsigned)(time(NULL) ^ getpid()));

    int fd = connect_server();
    if (fd < 0) return 1;

    /* HELLO handshake. */
    send_msg(fd, "HELLO PILOT");
    char reply[MSG_MAX];
    if (recv_msg(fd, reply, sizeof(reply)) <= 0 || strncmp(reply, "OK", 2) != 0) {
        fprintf(stderr, "server rejected HELLO: %s\n", reply);
        close(fd);
        return 1;
    }

    /* SPAWN */
    char cmd[MSG_MAX];
    snprintf(cmd, sizeof(cmd), "SPAWN %s %d", callsign, altitude);
    send_msg(fd, cmd);
    if (recv_msg(fd, reply, sizeof(reply)) <= 0 || strncmp(reply, "OK", 2) != 0) {
        fprintf(stderr, "spawn failed: %s\n", reply);
        close(fd);
        return 1;
    }
    int id = atoi(reply + 3);
    printf("[pilot %s] spawned id=%d at %d ft.  Ctrl+\\ = mayday, Ctrl+C = quit\n",
           callsign, id, altitude);

    /* Flight loop - descend across the grid. */
    int x = rand() % RADAR_WIDTH;
    int y = rand() % RADAR_HEIGHT;
    int alt = altitude;
    int fuel = 100;
    int dx = (rand() % 2) ? 1 : -1;
    int dy = (rand() % 2) ? 1 : -1;

    while (!g_stop) {
        if (g_mayday) {
            snprintf(cmd, sizeof(cmd), "MAYDAY %d", id);
            send_msg(fd, cmd);
            printf("[pilot %s] !!! MAYDAY DECLARED !!!\n", callsign);
            g_mayday = 0;
        }

        /* move */
        x += dx; y += dy;
        if (x <= 0 || x >= RADAR_WIDTH - 1)  dx = -dx;
        if (y <= 0 || y >= RADAR_HEIGHT - 1) dy = -dy;
        if (alt > 0) alt -= 100;
        if (fuel > 0) fuel -= 1;

        snprintf(cmd, sizeof(cmd), "UPDATE %d %d %d %d %d", id, x, y, alt, fuel);
        if (send_msg(fd, cmd) < 0) {
            fprintf(stderr, "[pilot %s] server gone\n", callsign);
            break;
        }

        usleep(500 * 1000);
    }

    close(fd);
    printf("[pilot %s] disconnected\n", callsign);
    return 0;
}

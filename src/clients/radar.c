/*
 * Radar client - Day 1 skeleton
 *
 * Connects as ROLE_RADAR, receives STATE broadcasts from the server
 * every 500ms, and renders an ASCII grid.
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

static volatile sig_atomic_t g_stop = 0;
static void on_int(int s) { (void)s; g_stop = 1; }

typedef struct {
    int  id;
    char callsign[CALLSIGN_LEN];
    int  x, y, alt, fuel;
    char state[16];
    int  emergency;
} Contact;

#define MAX_CONTACTS 32
static Contact g_contacts[MAX_CONTACTS];
static int g_contact_count = 0;

/* Parse: "STATE |id,callsign,x,y,alt,fuel,state,emergency |..." */
static void parse_state(char *msg) {
    g_contact_count = 0;
    char *tok = strtok(msg, "|");
    /* first token is "STATE " - skip */
    while ((tok = strtok(NULL, "|")) != NULL && g_contact_count < MAX_CONTACTS) {
        Contact *c = &g_contacts[g_contact_count];
        int em;
        int got = sscanf(tok, "%d,%15[^,],%d,%d,%d,%d,%15[^,],%d",
                         &c->id, c->callsign, &c->x, &c->y,
                         &c->alt, &c->fuel, c->state, &em);
        if (got == 8) {
            c->emergency = em;
            g_contact_count++;
        }
    }
}

static void render(void) {
    /* Clear screen + home cursor (ANSI). */
    printf("\033[2J\033[H");
    printf("=== ATC RADAR ===   aircraft: %d\n", g_contact_count);

    /* Build blank grid. */
    static char grid[RADAR_HEIGHT][RADAR_WIDTH + 1];
    for (int r = 0; r < RADAR_HEIGHT; r++) {
        memset(grid[r], '.', RADAR_WIDTH);
        grid[r][RADAR_WIDTH] = '\0';
    }

    for (int i = 0; i < g_contact_count; i++) {
        Contact *c = &g_contacts[i];
        if (c->x >= 0 && c->x < RADAR_WIDTH &&
            c->y >= 0 && c->y < RADAR_HEIGHT) {
            grid[c->y][c->x] = c->emergency ? '!' : '+';
        }
    }

    /* top border */
    putchar('+');
    for (int i = 0; i < RADAR_WIDTH; i++) putchar('-');
    printf("+\n");
    for (int r = 0; r < RADAR_HEIGHT; r++) {
        printf("|%s|\n", grid[r]);
    }
    putchar('+');
    for (int i = 0; i < RADAR_WIDTH; i++) putchar('-');
    printf("+\n");

    /* legend */
    printf("\n  ID  CALLSIGN   X   Y   ALT   FUEL  STATE        E\n");
    printf("  --------------------------------------------------\n");
    for (int i = 0; i < g_contact_count; i++) {
        Contact *c = &g_contacts[i];
        printf("  %-3d %-10s %3d %3d %5d  %3d%%  %-12s %s\n",
               c->id, c->callsign, c->x, c->y, c->alt, c->fuel, c->state,
               c->emergency ? "!!" : "  ");
    }
    fflush(stdout);
}

int main(void) {
    struct sigaction sa = {0};
    sa.sa_handler = on_int;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); return 1;
    }

    send_msg(fd, "HELLO RADAR");
    char reply[MSG_MAX];
    if (recv_msg(fd, reply, sizeof(reply)) <= 0) {
        fprintf(stderr, "no reply\n"); return 1;
    }

    char buf[MSG_MAX];
    while (!g_stop) {
        ssize_t n = recv_msg(fd, buf, sizeof(buf));
        if (n <= 0) break;
        if (!strncmp(buf, "STATE", 5)) {
            parse_state(buf);
            render();
        } else if (!strncmp(buf, "ALERT", 5)) {
            /* Alert banner - stays on screen briefly */
            printf("\n*** %s ***\n", buf);
            fflush(stdout);
            sleep(1);
        }
    }
    close(fd);
    return 0;
}

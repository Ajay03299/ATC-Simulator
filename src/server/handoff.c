#include "handoff.h"
#include "../common/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>

/*
 * Named pipe IPC.
 *
 * - handoff_init: create the FIFO on disk via mkfifo(). Idempotent:
 *   if it already exists we don't care.
 * - handoff_post: open FIFO for writing, write a line, close. The
 *   open+write+close cycle per event is intentional - it guarantees
 *   each event is a complete, atomic message from the reader's view
 *   (pipes guarantee atomicity for writes <= PIPE_BUF bytes).
 * - handoff_reader: a thread that open()s the FIFO for reading and
 *   loops forever pulling events out of it.
 */

/* We need the broadcast() function from main.c to push ALERT messages
 * to controllers. Forward declaration to avoid a circular header. */
extern void broadcast(const char *msg, int role_mask);

static pthread_t g_reader_tid;
static int       g_reader_running = 0;

void handoff_init(void) {
    /* Remove any stale FIFO from a previous run, then create fresh. */
    unlink(HANDOFF_FIFO);
    if (mkfifo(HANDOFF_FIFO, 0644) < 0) {
        perror("[handoff] mkfifo");
        exit(1);
    }
    printf("[handoff] FIFO ready at %s\n", HANDOFF_FIFO);
}

void handoff_destroy(void) {
    unlink(HANDOFF_FIFO);
}

int handoff_post(int aircraft_id, Role from, Role to) {
    /* O_NONBLOCK on write side: if there's no reader, we don't want
     * to hang the caller. The reader thread opens before any writes
     * happen, so in practice this never fires. */
    int fd = open(HANDOFF_FIFO, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        /* ENXIO = no reader. Drop event silently - it's a notification,
         * not a critical message. */
        if (errno == ENXIO) return 0;
        perror("[handoff] open write");
        return -1;
    }

    char line[128];
    int n = snprintf(line, sizeof(line),
                     "HANDOFF %d %s %s\n",
                     aircraft_id, role_name(from), role_name(to));
    if (write(fd, line, (size_t)n) < 0) {
        perror("[handoff] write");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static void *handoff_reader(void *arg) {
    (void)arg;
    /* Open the FIFO for reading. This blocks until a writer opens it,
     * but we immediately do a self-write below to unblock - so the
     * thread can run independently. Alternative: O_RDONLY | O_NONBLOCK. */
    int fd = open(HANDOFF_FIFO, O_RDONLY);
    if (fd < 0) {
        perror("[handoff] open read");
        return NULL;
    }

    char buf[512];
    char line[256];
    size_t line_pos = 0;

    while (g_reader_running) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[handoff] read");
            break;
        }
        if (n == 0) {
            /* All writers closed. Reopen to wait for new writes. */
            close(fd);
            fd = open(HANDOFF_FIFO, O_RDONLY);
            if (fd < 0) break;
            continue;
        }

        /* Accumulate into line buffer, emit on each newline. */
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || line_pos >= sizeof(line) - 1) {
                line[line_pos] = '\0';
                if (line_pos > 0) {
                    printf("[handoff-rx] %s\n", line);
                    /* Broadcast to all controllers + admin so every
                     * operator sees the handoff event. */
                    char alert[320];
                    snprintf(alert, sizeof(alert), "ALERT %s", line);
                    int mask = (1 << ROLE_GROUND)   |
                               (1 << ROLE_TOWER)    |
                               (1 << ROLE_APPROACH) |
                               (1 << ROLE_ADMIN);
                    broadcast(alert, mask);
                }
                line_pos = 0;
            } else {
                line[line_pos++] = c;
            }
        }
    }
    close(fd);
    return NULL;
}

void handoff_start_reader(void) {
    g_reader_running = 1;
    if (pthread_create(&g_reader_tid, NULL, handoff_reader, NULL) != 0) {
        perror("[handoff] pthread_create");
        exit(1);
    }
    pthread_detach(g_reader_tid);
    printf("[handoff] reader thread started\n");
}

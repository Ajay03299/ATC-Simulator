#include "logger.h"
#include "../common/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/file.h>    /* flock, LOCK_EX, LOCK_SH, LOCK_UN */
#include <sys/stat.h>

/*
 * All three public functions follow the same skeleton:
 *
 *   open()        - get a file descriptor
 *   flock(fd, L)  - acquire the lock, blocking if needed
 *   write/read    - do the I/O
 *   flock(fd, LOCK_UN) - release
 *   close()       - implicit release anyway, but explicit is nicer
 *
 * Per log entry we also prepend a timestamp, which makes the log
 * legible during demo and proves events really were serialized.
 */

void logger_init(void) {
    /* Make sure the logs directory exists. */
    mkdir("logs", 0755);
    /* Touch both files so the first open always succeeds. */
    FILE *f;
    f = fopen(FLIGHT_LOG,   "a"); if (f) fclose(f);
    f = fopen(INCIDENT_LOG, "a"); if (f) fclose(f);
    printf("[logger] flight log: %s\n", FLIGHT_LOG);
    printf("[logger] incident log: %s\n", INCIDENT_LOG);
}

/* Format a "YYYY-MM-DD HH:MM:SS " timestamp into buf. */
static void timestamp(char *buf, size_t bufsz) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, bufsz, "%Y-%m-%d %H:%M:%S ", &tm);
}

static void append_line(const char *path, const char *fmt, va_list ap) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { perror("[logger] open"); return; }

    /* Acquire exclusive lock - blocks if another writer holds it.
     * This is the CONCURRENCY CONTROL on the on-disk log. */
    if (flock(fd, LOCK_EX) < 0) {
        perror("[logger] flock LOCK_EX");
        close(fd);
        return;
    }

    char ts[32];
    timestamp(ts, sizeof(ts));

    char line[512];
    int n1 = snprintf(line, sizeof(line), "%s", ts);
    int n2 = vsnprintf(line + n1, sizeof(line) - n1, fmt, ap);
    int total = n1 + n2;

    /* Ensure trailing newline. */
    if (total < (int)sizeof(line) - 1 && line[total - 1] != '\n') {
        line[total++] = '\n';
    }

    if (write(fd, line, (size_t)total) < 0) {
        perror("[logger] write");
    }

    /* fsync is optional but guarantees durability across crashes.
     * Included here to show we care about data consistency. */
    fsync(fd);

    flock(fd, LOCK_UN);
    close(fd);
}

void log_flight(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    append_line(FLIGHT_LOG, fmt, ap);
    va_end(ap);
}

void log_incident(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    append_line(INCIDENT_LOG, fmt, ap);
    va_end(ap);
}

int log_read_flights(char *buf, size_t bufsz) {
    int fd = open(FLIGHT_LOG, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) { buf[0] = '\0'; return 0; }
        perror("[logger] open read"); return -1;
    }

    /* SHARED lock - multiple readers can hold this at once. Writers
     * requesting LOCK_EX will block until all shared locks release.
     * This is the read side of the reader/writer pattern. */
    if (flock(fd, LOCK_SH) < 0) {
        perror("[logger] flock LOCK_SH");
        close(fd);
        return -1;
    }

    ssize_t total = 0;
    ssize_t n;
    while ((size_t)total < bufsz - 1 &&
           (n = read(fd, buf + total, bufsz - 1 - (size_t)total)) > 0) {
        total += n;
    }
    buf[total] = '\0';

    flock(fd, LOCK_UN);
    close(fd);
    return (int)total;
}

#ifndef ATC_LOGGER_H
#define ATC_LOGGER_H

#include <stddef.h>

/*
 * Concurrent-safe file logging via flock().
 *
 * We keep two on-disk log files:
 *   logs/flights.log   - one line per aircraft state change
 *   logs/incidents.log - mayday / weather / kill events
 *
 * Both are written by many threads (one per connected client) and
 * occasionally read by the admin VIEW_LOGS command. flock() gives us
 * the classic reader/writer semantics:
 *
 *   LOCK_EX  exclusive - one writer, no readers. Used by append paths.
 *   LOCK_SH  shared    - many readers, no writer. Used by VIEW_LOGS.
 *   LOCK_UN  unlock.
 *
 * Why flock() over fcntl()/F_SETLK?
 *   flock() is simpler: whole-file locks, no byte ranges, released
 *   automatically when the fd is closed (or the process dies). For
 *   a small log file with append-only semantics, that's exactly what
 *   we want. fcntl is more powerful but overkill here.
 *
 * Correctness note: we open+lock+write+unlock+close for every line.
 * Holding the lock across a slow write would starve other writers.
 * Short critical sections keep throughput high.
 */

void logger_init(void);

/* Append a line to the flight log under LOCK_EX. fmt/... are printf-style. */
void log_flight(const char *fmt, ...);

/* Append a line to the incident log under LOCK_EX. */
void log_incident(const char *fmt, ...);

/* Read entire flight log under LOCK_SH. Returns bytes written to buf
 * (null-terminated). Returns 0 if empty, -1 on error. Multiple admins
 * can call this concurrently without blocking each other. */
int  log_read_flights(char *buf, size_t bufsz);

#endif

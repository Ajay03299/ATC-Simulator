#ifndef ATC_EMERGENCY_H
#define ATC_EMERGENCY_H

/*
 * Signal-based IPC for emergencies and weather alerts.
 *
 * This is the SECOND IPC mechanism in the project (the first is the
 * named pipe for controller handoffs). Signals are fundamentally
 * different from sockets and pipes:
 *
 *   - They're kernel-delivered, asynchronous, and interrupt the target
 *     process wherever it's currently executing.
 *   - They carry no payload (except real-time signals, which we don't
 *     need here). The signal NUMBER is the entire message.
 *   - They're received even if the target isn't reading any fd.
 *
 * We use two user-defined signals:
 *   SIGUSR1 - mayday. A pilot sends this directly to the server PID
 *             (not via the socket) to demand immediate priority. Useful
 *             when the pilot is overwhelmed and can't type the command.
 *   SIGUSR2 - weather alert trigger. An admin-side action.
 *
 * Signal handler constraints:
 *   Handlers can only call async-signal-safe functions. No printf,
 *   no malloc, no pthread_mutex_lock. The pattern is: set a flag, let
 *   the main loop do the real work.
 *
 * We write the server's PID to a file on init so pilots know where
 * to send the signal. Classic Unix daemon pattern.
 */

#define PID_FILE "/tmp/atc_server.pid"

/* Install SIGUSR1 / SIGUSR2 handlers. Write our PID to PID_FILE. */
void emergency_init(void);
void emergency_destroy(void);

/* Called from the main loop once per tick. If a signal fired since
 * the last call, handles the real work (broadcast alerts, log the
 * incident). Safe to call from any thread; uses atomics. */
void emergency_poll(void);

#endif

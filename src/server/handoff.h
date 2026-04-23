#ifndef ATC_HANDOFF_H
#define ATC_HANDOFF_H

#include "../common/types.h"

/*
 * Inter-controller handoff notifications via a named pipe (FIFO).
 *
 * When Approach hands an aircraft off to Tower (or Tower to Ground),
 * the server writes a line to a FIFO on disk. A background reader
 * thread picks it up and broadcasts it to the relevant controller
 * clients. This demonstrates IPC via named pipes - distinct from
 * Unix domain sockets and from signals.
 *
 * The FIFO lives at /tmp/atc_handoff.fifo. Format of each line:
 *
 *     HANDOFF <aircraft_id> <from_role> <to_role>\n
 *
 * Example:
 *     HANDOFF 3 APPROACH TOWER
 *
 * Why a named pipe?
 *   Named pipes are one of the classic IPC mechanisms listed in the
 *   project spec. They let unrelated processes communicate without
 *   needing a common parent (unlike unnamed pipes from pipe(2)).
 *   Here both ends live in the server process, but we still use a
 *   real FIFO on disk so the mechanism is unambiguously demonstrated.
 */

void handoff_init(void);
void handoff_destroy(void);

/* Queue a handoff event. Writes one line to the FIFO. Non-blocking. */
int  handoff_post(int aircraft_id, Role from, Role to);

/* Start the background reader thread. Events pulled from the FIFO
 * are printed to the server log and re-broadcast via the main
 * broadcast() helper in main.c. */
void handoff_start_reader(void);

#endif

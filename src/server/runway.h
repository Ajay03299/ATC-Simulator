#ifndef ATC_RUNWAY_H
#define ATC_RUNWAY_H

#include "../common/types.h"

/*
 * Runway management.
 *
 * We have NUM_RUNWAYS physical runways. Each is a critical section -
 * only one aircraft can be using it at any time (for takeoff or
 * landing). We guard each one with its own pthread_mutex_t, so two
 * different runways can be in use simultaneously - independent locks
 * mean no false contention.
 *
 * Tower acquires the mutex in runway_acquire(), operates on the
 * aircraft, and releases in runway_release(). If another controller
 * tries the same runway, they block until we release.
 */

void runway_init(void);
void runway_destroy(void);

/* Try to acquire runway [idx] for aircraft [aircraft_id].
 * Blocks if the runway is in use. Returns 0 on success, -1 on bad idx. */
int  runway_acquire(int idx, int aircraft_id);

/* Release runway [idx]. Caller must have previously acquired it. */
int  runway_release(int idx);

/* Read-only status (mutex protected): which aircraft is on this runway? */
int  runway_occupant(int idx);

#endif

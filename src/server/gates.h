#ifndef ATC_GATES_H
#define ATC_GATES_H

/*
 * Gate allocation via a POSIX named semaphore.
 *
 * We have NUM_GATES (4) parking gates. Multiple controllers may be
 * allocating gates at the same time, so we need counting synchronization:
 *   - semaphore count = number of free gates
 *   - gate_acquire() = sem_wait (decrement, block if zero)
 *   - gate_release() = sem_post (increment)
 *
 * Why a semaphore and not a mutex?
 *   A mutex is binary - locked or unlocked. A semaphore counts. We have
 *   4 identical resources; any free one is fine. The semaphore blocks
 *   only when ALL 4 are taken, which is the correct behavior.
 *
 * Why NAMED (sem_open) instead of unnamed (sem_init)?
 *   macOS does not implement unnamed POSIX semaphores - sem_init()
 *   returns ENOSYS. Named semaphores work on both macOS and Linux.
 */

void gates_init(void);
void gates_destroy(void);

/* Claim one free gate. Returns 0 on success.
 * Blocks if all gates are full. */
int  gate_acquire(void);

/* Return a gate to the pool. */
int  gate_release(void);

#endif

#include "gates.h"
#include "../common/types.h"

#include <semaphore.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * POSIX named semaphore, backed by the kernel. We use sem_open with
 * O_CREAT so the object is created the first time and reused after.
 *
 * On macOS, the semaphore shows up as a kernel object; on Linux it
 * appears under /dev/shm as "sem.<name>". Either way, the API is the
 * same.
 */

static sem_t *g_gate_sem = SEM_FAILED;

void gates_init(void) {
    /* Unlink first so we start with a clean count if a prior run left
     * a stale semaphore with the wrong value. */
    sem_unlink(GATE_SEM_NAME);

    g_gate_sem = sem_open(GATE_SEM_NAME,
                          O_CREAT | O_EXCL,
                          0644,
                          NUM_GATES);   /* initial count */
    if (g_gate_sem == SEM_FAILED) {
        perror("[gates] sem_open");
        exit(1);
    }
    printf("[gates] semaphore initialized with %d gates free\n", NUM_GATES);
}

void gates_destroy(void) {
    if (g_gate_sem != SEM_FAILED) {
        sem_close(g_gate_sem);
        sem_unlink(GATE_SEM_NAME);
        g_gate_sem = SEM_FAILED;
    }
}

int gate_acquire(void) {
    if (g_gate_sem == SEM_FAILED) return -1;

    /* sem_wait blocks until the count is > 0, then decrements it. */
    if (sem_wait(g_gate_sem) < 0) {
        perror("[gates] sem_wait");
        return -1;
    }

    /* Peek remaining count for the server log. sem_getvalue is not
     * required by POSIX to be exact, but it's fine for a human-facing
     * status line. */
    int val = -1;
    sem_getvalue(g_gate_sem, &val);
    printf("[gates] gate allocated, %d free\n", val);
    return 0;
}

int gate_release(void) {
    if (g_gate_sem == SEM_FAILED) return -1;

    if (sem_post(g_gate_sem) < 0) {
        perror("[gates] sem_post");
        return -1;
    }

    int val = -1;
    sem_getvalue(g_gate_sem, &val);
    printf("[gates] gate released, %d free\n", val);
    return 0;
}

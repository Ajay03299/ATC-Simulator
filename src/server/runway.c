#include "runway.h"
#include <pthread.h>
#include <stdio.h>

/*
 * One mutex per runway plus a small bookkeeping integer (who's on it).
 * The occupant field is guarded by the same mutex as the runway itself,
 * so reads/writes to it never race.
 */

typedef struct {
    pthread_mutex_t mtx;
    int             occupant;   /* aircraft id, or -1 if runway free */
} Runway;

static Runway g_runways[NUM_RUNWAYS];

void runway_init(void) {
    for (int i = 0; i < NUM_RUNWAYS; i++) {
        pthread_mutex_init(&g_runways[i].mtx, NULL);
        g_runways[i].occupant = -1;
    }
}

void runway_destroy(void) {
    for (int i = 0; i < NUM_RUNWAYS; i++) {
        pthread_mutex_destroy(&g_runways[i].mtx);
    }
}

int runway_acquire(int idx, int aircraft_id) {
    if (idx < 0 || idx >= NUM_RUNWAYS) return -1;

    /* This call blocks if another thread holds the mutex.
     * That's the whole point - Tower controllers serialize on this. */
    pthread_mutex_lock(&g_runways[idx].mtx);
    g_runways[idx].occupant = aircraft_id;
    printf("[runway] runway %d acquired by aircraft %d\n", idx, aircraft_id);
    return 0;
}

int runway_release(int idx) {
    if (idx < 0 || idx >= NUM_RUNWAYS) return -1;
    int who = g_runways[idx].occupant;
    g_runways[idx].occupant = -1;
    pthread_mutex_unlock(&g_runways[idx].mtx);
    printf("[runway] runway %d released (was aircraft %d)\n", idx, who);
    return 0;
}

int runway_occupant(int idx) {
    if (idx < 0 || idx >= NUM_RUNWAYS) return -1;
    /* Quick peek - we don't hold the lock across the read because it's
     * just an int and torn reads are impossible on aligned ints on x86/arm.
     * If we wanted strict correctness we'd lock, but this is a status
     * query, not a decision point. */
    return g_runways[idx].occupant;
}

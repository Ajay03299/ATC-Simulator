#include "auth.h"
#include <stdio.h>
#include <string.h>

/*
 * Permission table.
 *
 * Each row maps a command verb to a bitmask of roles that may invoke it.
 * Add a new command -> add a new row. Keep this table the single source
 * of truth for authorization decisions.
 */

#define R(role) (1 << (role))

typedef struct {
    const char *cmd;
    int         allowed_mask;
} Permission;

static const Permission TABLE[] = {
    /* Pilot-only commands: creating and moving your own aircraft. */
    { "SPAWN",         R(ROLE_PILOT) },
    { "UPDATE",        R(ROLE_PILOT) },
    { "MAYDAY",        R(ROLE_PILOT) },

    /* Approach controller hands aircraft off to Tower when on final. */
    { "VECTOR",        R(ROLE_APPROACH) | R(ROLE_ADMIN) },
    { "HANDOFF_TOWER", R(ROLE_APPROACH) | R(ROLE_ADMIN) },

    /* Tower owns the runway: clearances for takeoff and landing. */
    { "CLEAR_LAND",    R(ROLE_TOWER)    | R(ROLE_ADMIN) },
    { "CLEAR_TAKEOFF", R(ROLE_TOWER)    | R(ROLE_ADMIN) },
    { "HANDOFF_GROUND",R(ROLE_TOWER)    | R(ROLE_ADMIN) },

    /* Ground controls aircraft on taxiways and at gates. */
    { "TAXI",          R(ROLE_GROUND)   | R(ROLE_ADMIN) },
    { "PUSHBACK",      R(ROLE_GROUND)   | R(ROLE_ADMIN) },
    { "HANDOFF_TOWER_FROM_GROUND",
                       R(ROLE_GROUND)   | R(ROLE_ADMIN) },

    /* Admin-only: view full log, force-kill an aircraft, weather alert. */
    { "VIEW_LOGS",     R(ROLE_ADMIN) },
    { "KILL",          R(ROLE_ADMIN) },
    { "WEATHER",       R(ROLE_ADMIN) },

    /* Everyone can ping. */
    { "PING",          R(ROLE_PILOT)    | R(ROLE_GROUND) |
                       R(ROLE_TOWER)    | R(ROLE_APPROACH) |
                       R(ROLE_ADMIN)    | R(ROLE_RADAR) },

    { NULL, 0 }
};

int is_authorized(Role r, const char *cmd) {
    if (!cmd) return 0;
    for (const Permission *p = TABLE; p->cmd != NULL; p++) {
        if (!strcmp(p->cmd, cmd)) {
            return (p->allowed_mask & R(r)) ? 1 : 0;
        }
    }
    return 0;  /* unknown commands are denied by default */
}

const char *deny_reason(Role r, const char *cmd) {
    static char buf[128];
    /* static buffer is fine - server is single-threaded for auth checks
     * only from within the client_thread, and the string is copied
     * into send_msg before any other thread would call this. */
    snprintf(buf, sizeof(buf),
             "role %s not allowed to issue %s",
             (r == ROLE_PILOT)    ? "PILOT" :
             (r == ROLE_GROUND)   ? "GROUND" :
             (r == ROLE_TOWER)    ? "TOWER" :
             (r == ROLE_APPROACH) ? "APPROACH" :
             (r == ROLE_ADMIN)    ? "ADMIN" :
             (r == ROLE_RADAR)    ? "RADAR" : "UNKNOWN",
             cmd);
    return buf;
}

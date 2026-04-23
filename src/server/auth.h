#ifndef ATC_AUTH_H
#define ATC_AUTH_H

#include "../common/types.h"

/*
 * Role-based command authorization.
 *
 * Each command in the system has an allowed-role set. The server calls
 * is_authorized(role, command) before executing anything that mutates
 * state. This is the core of our role-based access control.
 */

/* Returns 1 if the given role is allowed to issue the given command,
 * 0 otherwise. Command is a verb like "SPAWN", "CLEAR_LAND", "TAXI". */
int is_authorized(Role r, const char *cmd);

/* Human-readable reason a command was denied (for ERR replies). */
const char *deny_reason(Role r, const char *cmd);

#endif

#ifndef ATC_PROTOCOL_H
#define ATC_PROTOCOL_H

#include "types.h"
#include <sys/types.h>

/*
 * Wire protocol: length-prefixed text messages.
 * [4 bytes: big-endian length N][N bytes: payload]
 *
 * Payload is a text command, space-separated. Examples:
 *   HELLO PILOT AI203
 *   HELLO TOWER
 *   SPAWN AI203 10000          (pilot -> server)
 *   UPDATE 5 42 15 9500 87     (pilot -> server: id x y alt fuel)
 *   MAYDAY 5                   (pilot -> server, also via SIGUSR1)
 *   CMD CLEAR_LAND 5 0         (controller -> server: id runway)
 *   CMD TAXI 5 2               (controller -> server: id gate)
 *   STATE <snapshot>           (server -> radar/clients)
 *   ALERT MAYDAY AI203         (server -> all controllers)
 *   OK / ERR <reason>          (server -> client)
 */

/* Send a length-prefixed message. Returns 0 on success, -1 on error. */
int send_msg(int fd, const char *msg);

/* Receive a length-prefixed message into buf (size bufsz).
 * Returns bytes read, 0 on clean close, -1 on error. */
ssize_t recv_msg(int fd, char *buf, size_t bufsz);

/* Parse "HELLO <ROLE> [callsign]" into a Role enum. */
Role parse_role(const char *s);
const char *role_name(Role r);
const char *state_name(FlightState s);

#endif

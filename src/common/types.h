#ifndef ATC_TYPES_H
#define ATC_TYPES_H

#include <stdint.h>
#include <time.h>

#define MAX_AIRCRAFT     32
#define MAX_CLIENTS      16
#define NUM_RUNWAYS      2
#define NUM_GATES        4
#define RADAR_WIDTH      60
#define RADAR_HEIGHT     20
#define CALLSIGN_LEN     16
#define MSG_MAX          512

#define SOCKET_PATH      "/tmp/atc.sock"
#define HANDOFF_FIFO     "/tmp/atc_handoff.fifo"
#define FLIGHT_LOG       "logs/flights.log"
#define INCIDENT_LOG     "logs/incidents.log"
#define GATE_SEM_NAME    "/atc_gate_sem"

typedef enum {
    ROLE_PILOT = 0,
    ROLE_GROUND,
    ROLE_TOWER,
    ROLE_APPROACH,
    ROLE_ADMIN,
    ROLE_RADAR,
    ROLE_UNKNOWN
} Role;

typedef enum {
    STATE_APPROACHING = 0,   // in Approach controller's airspace
    STATE_LANDING,           // cleared by Tower
    STATE_TAXIING_IN,        // on ground, heading to gate
    STATE_PARKED,            // at gate
    STATE_TAXIING_OUT,       // heading to runway
    STATE_TAKEOFF,           // cleared by Tower
    STATE_DEPARTED,          // left airspace
    STATE_EMERGENCY          // mayday declared
} FlightState;

typedef struct {
    int         id;                       // unique, assigned by server
    char        callsign[CALLSIGN_LEN];
    int         x, y;                     // radar grid position
    int         altitude;                 // feet
    int         fuel;                     // percent 0-100
    FlightState state;
    Role        controller;               // who owns it right now
    int         runway;                   // -1 if none
    int         gate;                     // -1 if none
    int         emergency;                // 1 if mayday
    time_t      last_update;
} Aircraft;

#endif

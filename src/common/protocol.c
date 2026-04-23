#include "protocol.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

static ssize_t read_all(int fd, void *buf, size_t n) {
    size_t total = 0;
    char *p = buf;
    while (total < n) {
        ssize_t r = read(fd, p + total, n - total);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)r;
    }
    return (ssize_t)total;
}

static ssize_t write_all(int fd, const void *buf, size_t n) {
    size_t total = 0;
    const char *p = buf;
    while (total < n) {
        ssize_t w = write(fd, p + total, n - total);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)w;
    }
    return (ssize_t)total;
}

int send_msg(int fd, const char *msg) {
    uint32_t len = (uint32_t)strlen(msg);
    uint32_t nlen = htonl(len);
    if (write_all(fd, &nlen, sizeof(nlen)) < 0) return -1;
    if (len > 0 && write_all(fd, msg, len) < 0) return -1;
    return 0;
}

ssize_t recv_msg(int fd, char *buf, size_t bufsz) {
    uint32_t nlen;
    ssize_t r = read_all(fd, &nlen, sizeof(nlen));
    if (r == 0) return 0;
    if (r < 0) return -1;
    uint32_t len = ntohl(nlen);
    if (len >= bufsz) return -1;   // too big
    if (len > 0) {
        r = read_all(fd, buf, len);
        if (r <= 0) return r;
    }
    buf[len] = '\0';
    return (ssize_t)len;
}

Role parse_role(const char *s) {
    if (!s) return ROLE_UNKNOWN;
    if (!strcasecmp(s, "PILOT"))    return ROLE_PILOT;
    if (!strcasecmp(s, "GROUND"))   return ROLE_GROUND;
    if (!strcasecmp(s, "TOWER"))    return ROLE_TOWER;
    if (!strcasecmp(s, "APPROACH")) return ROLE_APPROACH;
    if (!strcasecmp(s, "ADMIN"))    return ROLE_ADMIN;
    if (!strcasecmp(s, "RADAR"))    return ROLE_RADAR;
    return ROLE_UNKNOWN;
}

const char *role_name(Role r) {
    switch (r) {
        case ROLE_PILOT: return "PILOT";
        case ROLE_GROUND: return "GROUND";
        case ROLE_TOWER: return "TOWER";
        case ROLE_APPROACH: return "APPROACH";
        case ROLE_ADMIN: return "ADMIN";
        case ROLE_RADAR: return "RADAR";
        default: return "UNKNOWN";
    }
}

const char *state_name(FlightState s) {
    switch (s) {
        case STATE_APPROACHING:  return "APPROACHING";
        case STATE_LANDING:      return "LANDING";
        case STATE_TAXIING_IN:   return "TAXI_IN";
        case STATE_PARKED:       return "PARKED";
        case STATE_TAXIING_OUT:  return "TAXI_OUT";
        case STATE_TAKEOFF:      return "TAKEOFF";
        case STATE_DEPARTED:     return "DEPARTED";
        case STATE_EMERGENCY:    return "EMERGENCY";
        default: return "?";
    }
}

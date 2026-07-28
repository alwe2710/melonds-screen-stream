#ifndef FINLINK_DISCOVERY_H
#define FINLINK_DISCOVERY_H

#include <stddef.h>

/* UDP discovery beacon (docs/protocol.md, "Discovery-Beacon (UDP)"). Pure
 * parsing -- no socket I/O: the platform shell owns the UDP socket (bind to
 * FINLINK_BEACON_PORT, recvfrom in a loop, one call here per datagram),
 * same division of responsibility as finlink/handshake.h. */

#ifdef __cplusplus
extern "C" {
#endif

#define FINLINK_BEACON_PORT 6805
/* Client-side convention matching the server's send interval (protocol.md):
 * an entry not refreshed within this long is considered gone. */
#define FINLINK_BEACON_STALE_MS 6000

#define FINLINK_BEACON_EMULATOR_LEN 64
#define FINLINK_BEACON_GAME_TITLE_LEN 256
#define FINLINK_BEACON_STREAM_TYPE_LEN 32
#define FINLINK_BEACON_HOST_LEN 64

typedef struct {
    int protocol_version;
    char emulator_identifier[FINLINK_BEACON_EMULATOR_LEN];
    char game_title[FINLINK_BEACON_GAME_TITLE_LEN];
    char stream_type[FINLINK_BEACON_STREAM_TYPE_LEN];
    char host[FINLINK_BEACON_HOST_LEN];
    int handshake_port;
} finlink_beacon;

/* Parses one UDP datagram's payload. Returns 0 (failure) for anything that
 * isn't a well-formed finlink beacon -- notably including unrelated UDP
 * traffic that happens to land on the same port, which callers should
 * silently ignore rather than treat as a real error. */
int finlink_parse_beacon(const unsigned char *data, size_t size, finlink_beacon *out);

#ifdef __cplusplus
}
#endif

#endif /* FINLINK_DISCOVERY_H */

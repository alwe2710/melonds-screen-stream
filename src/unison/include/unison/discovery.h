#ifndef UNISON_DISCOVERY_H
#define UNISON_DISCOVERY_H

#include <stddef.h>

/* UDP discovery beacon (docs/protocol.md, "Discovery-Beacon (UDP)"). Pure
 * parsing -- no socket I/O: the platform shell owns the UDP socket (bind to
 * UNISON_BEACON_PORT, recvfrom in a loop, one call here per datagram),
 * same division of responsibility as unison/handshake.h. */

#ifdef __cplusplus
extern "C" {
#endif

#define UNISON_BEACON_PORT 6805
/* Client-side convention matching the server's send interval (protocol.md):
 * an entry not refreshed within this long is considered gone. */
#define UNISON_BEACON_STALE_MS 6000

#define UNISON_BEACON_EMULATOR_LEN 64
#define UNISON_BEACON_GAME_TITLE_LEN 256
#define UNISON_BEACON_STREAM_TYPE_LEN 32
#define UNISON_BEACON_HOST_LEN 64

typedef struct {
    int protocol_version;
    char emulator_identifier[UNISON_BEACON_EMULATOR_LEN];
    char game_title[UNISON_BEACON_GAME_TITLE_LEN];
    char stream_type[UNISON_BEACON_STREAM_TYPE_LEN];
    char host[UNISON_BEACON_HOST_LEN];
    int handshake_port;
} unison_beacon;

/* Parses one UDP datagram's payload. Returns 0 (failure) for anything that
 * isn't a well-formed Unison beacon -- notably including unrelated UDP
 * traffic that happens to land on the same port, which callers should
 * silently ignore rather than treat as a real error. */
int unison_parse_beacon(const unsigned char *data, size_t size, unison_beacon *out);

#ifdef __cplusplus
}
#endif

#endif /* UNISON_DISCOVERY_H */

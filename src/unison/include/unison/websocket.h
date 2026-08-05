#ifndef UNISON_WEBSOCKET_H
#define UNISON_WEBSOCKET_H

#include <stddef.h>
#include <stdint.h>

/* Client-side RFC6455 WebSocket handshake + frame (de)serialization,
 * against the server implementation in GBAStreamHost.cpp / GBAStreamLobby.cpp
 * (dolphin-gba-stream). Pure logic, no sockets: the caller owns the TCP
 * connection and is responsible for sending the bytes this module produces
 * and feeding it the bytes it receives, same division of responsibility as
 * unison/protocol.h.
 *
 * Server-specific behavior this module assumes (confirmed against source,
 * see docs/protocol.md):
 *   - Server frames are always unmasked, single (non-fragmented) binary
 *     frames. Fragmented frames (FIN=0) are treated as a protocol error.
 *   - No ping/pong, no permessage-deflate extension negotiation.
 *   - The server does not echo a close frame back; once one is
 *     sent/received, the caller should just close the TCP connection. */

#ifdef __cplusplus
extern "C" {
#endif

#define UNISON_WS_KEY_LEN 24     /* base64(16 random bytes), no NUL */
#define UNISON_WS_KEY_BUF_LEN (UNISON_WS_KEY_LEN + 1)
#define UNISON_WS_ACCEPT_LEN 28  /* base64(SHA1 digest), no NUL */

/* Largest payload unison_ws_parse_frame() will accept. Comfortably above
 * any real video/audio/input frame (largest is one uncompressed 240x160
 * RGB565 video frame, 76800 bytes) -- exists to bound how much a corrupt or
 * hostile peer can make the caller buffer before unison_ws_parse_frame()
 * gives up, which matters on memory-constrained homebrew targets. */
#define UNISON_WS_MAX_FRAME_PAYLOAD (1u << 20)

/* --- Handshake --- */

/* Base64-encodes 16 caller-supplied random bytes into a Sec-WebSocket-Key.
 * Generating the random bytes themselves is the caller's responsibility:
 * platforms differ wildly in what RNG is available (hardware TRNG down to
 * libc rand()), and it doesn't need to be cryptographically strong here --
 * RFC6455 only needs it unpredictable enough to defeat naive proxy caching. */
void unison_ws_generate_key(const uint8_t random_bytes[16], char key_out[UNISON_WS_KEY_BUF_LEN]);

/* Writes an HTTP/1.1 GET Upgrade request into out_buf. `host` should be the
 * full Host header value (include the port if non-default, e.g.
 * "192.168.1.5:6801"). Returns the number of bytes written, or 0 if
 * out_capacity was too small. */
size_t unison_ws_build_handshake_request(const char *host, const char *path,
                                           const char key[UNISON_WS_KEY_LEN], char *out_buf,
                                           size_t out_capacity);

typedef enum {
    UNISON_WS_HANDSHAKE_OK = 0,
    UNISON_WS_HANDSHAKE_INCOMPLETE = 1, /* haven't received a full header yet, call again with more data */
    UNISON_WS_HANDSHAKE_ERR = -1        /* not HTTP 101, or Sec-WebSocket-Accept doesn't match */
} unison_ws_handshake_status;

/* Validates a server handshake response against the key we sent. `data`
 * should be everything received on the socket so far. On OK, *header_len is
 * set to the response header's length (through the blank line) -- any bytes
 * after that in `data` are already WebSocket frame data and must be kept,
 * not discarded. */
unison_ws_handshake_status unison_ws_parse_handshake_response(const uint8_t *data, size_t size,
                                                                  const char key[UNISON_WS_KEY_LEN],
                                                                  size_t *header_len);

/* --- Frames --- */

typedef enum {
    UNISON_WS_OPCODE_TEXT = 0x1,   /* app-level handshake JSON, see unison/handshake.h */
    UNISON_WS_OPCODE_BINARY = 0x2, /* Video/Audio/Input, see unison/protocol.h */
    UNISON_WS_OPCODE_CLOSE = 0x8
} unison_ws_opcode;

typedef struct {
    unison_ws_opcode opcode;
    uint8_t *payload; /* points into the caller's buffer; already unmasked in place */
    size_t payload_size;
    size_t frame_size; /* total bytes this frame occupied -- how much to consume from the front */
} unison_ws_frame;

typedef enum {
    UNISON_WS_FRAME_OK = 0,
    UNISON_WS_FRAME_INCOMPLETE = 1, /* not enough data yet for a full frame */
    UNISON_WS_FRAME_ERR = -1        /* fragmented, oversized, or an unsupported opcode */
} unison_ws_frame_status;

/* Parses one frame from the front of `data`. If masked, unmasks the payload
 * in place (mutates `data`) so `out->payload` can point directly into it
 * without a copy. Server frames in this protocol are always unmasked
 * already, so that path is mainly for completeness/testing. */
unison_ws_frame_status unison_ws_parse_frame(uint8_t *data, size_t size, unison_ws_frame *out);

/* Upper bound on the bytes unison_ws_build_frame() needs for a given
 * payload size (max header + mask key + payload). */
size_t unison_ws_build_frame_max_size(size_t payload_size);

/* Builds a masked client->server frame (RFC6455 requires clients to mask
 * every frame). mask_key is 4 caller-supplied bytes, see the note on RNG
 * above. Returns the number of bytes written to out_buf, or 0 if
 * out_capacity was too small. */
size_t unison_ws_build_frame(unison_ws_opcode opcode, const uint8_t *payload, size_t payload_size,
                               const uint8_t mask_key[4], uint8_t *out_buf, size_t out_capacity);

#ifdef __cplusplus
}
#endif

#endif /* UNISON_WEBSOCKET_H */

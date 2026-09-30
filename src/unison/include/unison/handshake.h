#ifndef UNISON_HANDSHAKE_H
#define UNISON_HANDSHAKE_H

#include <stddef.h>
#include <stdint.h>

/* App-level handshake (hello / hello_ack / session_ready / handshake_error),
 * exchanged as WebSocket text frames (UNISON_WS_OPCODE_TEXT,
 * unison/websocket.h) before any Video/Audio/Input binary frame -- see
 * docs/protocol.md, "Verbindungsaufbau: Handshake". Pure message
 * (de)serialization, no socket I/O: the caller sends/receives the actual WS
 * text frames (via unison_ws_build_frame / unison_ws_parse_frame) and
 * passes this module the frame payload, same division of responsibility as
 * unison/protocol.h and unison/websocket.h.
 *
 * protocol_version compatibility is exact-match (docs/protocol.md) --
 * callers should compare unison_hello.protocol_version against their own
 * UNISON_PROTOCOL_VERSION themselves and refuse to proceed (with a clear,
 * user-visible message) on any mismatch, rather than this module enforcing
 * it, since only the caller knows how to surface that to a user. */

#ifdef __cplusplus
extern "C" {
#endif

/* Bump alongside any wire-incompatible change to the messages below --
 * mirrors GBA_STREAM_PROTOCOL_VERSION in the dolphin-gba-stream fork's
 * Core/HW/GBAStreamNetUtil.h, which must stay numerically in sync.
 *
 * 2 -> 3: added session_ready.video_port (see unison_session_ready's own
 * comment and docs/protocol.md's "Dedicated video connection") -- only
 * Cemu (WIIU_GAMEPAD) and the Android client actually use it as of this
 * bump; azahar/melonDS/dolphin-gba-stream and every other client still
 * report/expect plain version 2 until they're updated too, so exact-match
 * means the Android client can't connect to them again until then -- a
 * known, accepted tradeoff for this pass (see the plan this came from).
 *
 * 3 -> 4: session_ready.video_port changed meaning -- a UDP port instead
 * of a second TCP/WebSocket connection, carrying UNISON_MSG_VIDEO *and*
 * UNISON_MSG_AUDIO now (audio moved off the control connection too), each
 * datagram prefixed with a unison_udp_fragment_header (protocol.h) instead
 * of WebSocket framing. See docs/protocol.md's "Dedicated video/audio
 * channel (UDP)". Same Cemu/Android-only scope as the 2 -> 3 bump. */
#define UNISON_PROTOCOL_VERSION 4

#define UNISON_MAX_SLOTS 4
#define UNISON_LABEL_LEN 8
#define UNISON_STREAM_TYPE_LEN 32
#define UNISON_INPUT_ENCODING_LEN 32
#define UNISON_HOST_LEN 64
#define UNISON_HANDSHAKE_CODE_LEN 32
#define UNISON_HANDSHAKE_DETAIL_LEN 256
#define UNISON_VIDEO_MODE_LEN 16

typedef enum {
    UNISON_HANDSHAKE_OK = 0,
    UNISON_HANDSHAKE_ERR = -1 /* malformed JSON, wrong "message" discriminator, or a required field missing */
} unison_handshake_result;

typedef struct {
    int index;
    char label[UNISON_LABEL_LEN]; /* e.g. "P1" */
    int occupied;
} unison_handshake_slot;

typedef struct {
    uint32_t width;
    uint32_t height;
    double fps;
} unison_handshake_video;

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
} unison_handshake_audio;

/* Server -> client, first message on a freshly WS-upgraded connection. */
typedef struct {
    int protocol_version;
    char stream_type[UNISON_STREAM_TYPE_LEN]; /* "GC_GBA_LINK", "N3DS_BOTTOM_SCREEN", ... */
    unison_handshake_slot slots[UNISON_MAX_SLOTS];
    size_t slot_count; /* truncated to UNISON_MAX_SLOTS if the server somehow sent more */
    unison_handshake_video video;
    int has_audio; /* 0 for stream types without audio, e.g. N3DS_BOTTOM_SCREEN */
    unison_handshake_audio audio;
    char input_encoding[UNISON_INPUT_ENCODING_LEN]; /* "gba_buttons", ... */
} unison_hello;

/* Client capabilities/limits to send back as hello_ack. wants_audio=0 skips
 * audio_limits entirely in the built JSON (see docs/protocol.md: "fehlt
 * ... wenn der Client keinen Ton möchte/kann").
 *
 * video_mode is the client's requested video encoding: "tiles" (TILES
 * delta-encoding + frame dedup, see unison/video_encode.h) or "legacy"
 * (always a full, non-tiled frame -- the original, pre-dedup behavior, kept
 * as a user-selectable fallback). Left as an empty string, video_mode is
 * omitted from the built JSON entirely, same convention as wants_audio; a
 * server that doesn't recognize the field (or an old client that never sets
 * it) should default to "tiles". Always set this from a fixed literal, never
 * from user-entered text -- unison_build_hello_ack() writes it into the
 * JSON unescaped. */
typedef struct {
    int requested_slot;
    uint32_t max_width;
    uint32_t max_height;
    double max_fps;
    int wants_audio;
    uint32_t max_sample_rate;
    uint8_t max_channels;
    char video_mode[UNISON_VIDEO_MODE_LEN];
} unison_hello_ack_request;

/* Server -> client, confirms (possibly downscaled) parameters and either the
 * final slot or a redirect to reconnect elsewhere and repeat the whole
 * hello/hello_ack exchange (multi-slot stream types only, e.g. GC_GBA_LINK's
 * lobby-to-player-port hop).
 *
 * video_mode is what the server actually used/will use for this session --
 * NOT necessarily an echo of the hello_ack.video_mode the client requested,
 * since the server may not support it (falls back to whatever it actually
 * has). Empty means the server predates this field entirely (an
 * unpatched/older host), which is a *different* case from the server
 * deliberately reporting a fallback: a client should only compare this
 * against what it requested (see docs/protocol.md, "Video-mode fallback")
 * when this is non-empty. Treat an empty value as "no information, don't
 * prompt the user" -- not as "tiles was granted" -- so an old host that
 * predates this field entirely never produces a false-positive prompt,
 * regardless of what was requested. */
typedef struct {
    int slot;
    unison_handshake_video video;
    int has_audio;
    unison_handshake_audio audio;
    int has_redirect;
    char redirect_host[UNISON_HOST_LEN];
    int redirect_port;
    char video_mode[UNISON_VIDEO_MODE_LEN];
    /* Port for a second, video-only WebSocket connection (see
     * docs/protocol.md, "Dedicated video connection") -- absent
     * (has_video_port = 0) means video stays multiplexed on this same
     * connection, the only behavior any server predating this field can
     * produce. Unlike redirect (a one-time replace: this connection
     * closes, a new one repeats the whole hello/hello_ack exchange
     * elsewhere), video_port names a *second*, simultaneously open
     * connection -- no hello/hello_ack on it, just a plain WebSocket
     * upgrade, after which it carries UNISON_MSG_VIDEO frames exclusively
     * and nothing else ever flows on it in either direction. */
    int has_video_port;
    int video_port;
} unison_session_ready;

typedef struct {
    char code[UNISON_HANDSHAKE_CODE_LEN]; /* "version_mismatch", "slot_unavailable", "malformed_request" */
    char detail[UNISON_HANDSHAKE_DETAIL_LEN]; /* human-readable, safe to show directly */
} unison_handshake_error;

typedef enum {
    UNISON_HS_MSG_UNKNOWN = 0,
    UNISON_HS_MSG_HELLO,
    UNISON_HS_MSG_SESSION_READY,
    UNISON_HS_MSG_HANDSHAKE_ERROR
} unison_handshake_message_type;

/* Reads just the top-level "message" discriminator from a text frame
 * payload, so the caller knows which of the parsers below to call without
 * guessing or trying each in turn. */
unison_handshake_message_type unison_peek_handshake_message(const uint8_t *data, size_t size);

unison_handshake_result unison_parse_hello(const uint8_t *data, size_t size, unison_hello *out);

/* Writes the hello_ack JSON text into out_buf (no trailing NUL counted in
 * the return value, but one is written if it fits). Returns the number of
 * bytes written, or 0 if out_capacity was too small -- same convention as
 * unison_ws_build_handshake_request. */
size_t unison_build_hello_ack(const unison_hello_ack_request *req, char *out_buf,
                                size_t out_capacity);

unison_handshake_result unison_parse_session_ready(const uint8_t *data, size_t size,
                                                      unison_session_ready *out);

unison_handshake_result unison_parse_handshake_error(const uint8_t *data, size_t size,
                                                        unison_handshake_error *out);

/* Whether a hello.stream_type is itself the *secondary* screen of a
 * dual-screen source (an emulator's own bottom/GamePad screen, e.g.
 * N3DS_BOTTOM_SCREEN, NDS_BOTTOM_SCREEN, or a future WIIU_GAMEPAD) rather
 * than a primary display like GC_GBA_LINK. Clients with two physical
 * screens of their own (3DS, DS/DSi) should show this content on their
 * own secondary/bottom screen unconditionally -- the same way a real
 * DS/DSi/Wii U shows its second screen on fixed hardware, not a
 * user-chosen one -- overriding whatever screen the user otherwise
 * prefers for single-screen stream types. See docs/protocol.md,
 * "Stream-Typen". */
int unison_stream_type_prefers_secondary_screen(const char *stream_type);

#ifdef __cplusplus
}
#endif

#endif /* UNISON_HANDSHAKE_H */

#ifndef FINLINK_PROTOCOL_H
#define FINLINK_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* Pure message (de)serialization for the wire protocol in docs/protocol.md.
 * No I/O, no allocation: these functions only view into / write to buffers
 * the caller owns. The actual WebSocket transport (handshake, frame masking,
 * socket I/O) is platform-specific and lives in clients/<platform>/. */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FINLINK_MSG_VIDEO = 1,
    FINLINK_MSG_INPUT = 2,
    FINLINK_MSG_AUDIO = 3,
    FINLINK_MSG_TEXT_INPUT_REQUEST = 4,  /* server->client, see finlink_text_input_request */
    FINLINK_MSG_TEXT_INPUT_RESPONSE = 5, /* client->server, see finlink_text_input_response */
    FINLINK_MSG_MIC_ENABLE = 6,          /* server->client, see finlink_mic_enable */
    FINLINK_MSG_MIC_AUDIO = 7,           /* client->server, see finlink_parse_mic_audio_frame */
} finlink_msg_type;

typedef enum {
    FINLINK_OK = 0,
    FINLINK_ERR_TOO_SHORT = -1,     /* buffer/decompressed data smaller than expected */
    FINLINK_ERR_UNKNOWN_TYPE = -2,  /* leading byte isn't a known finlink_msg_type */
    FINLINK_ERR_UNKNOWN_FORMAT = -3 /* video_header.format isn't a known finlink_video_format */
} finlink_result;

/* Bit positions within the type=2 input keyBitmask, per docs/protocol.md. */
typedef enum {
    FINLINK_KEY_A = 1 << 0,
    FINLINK_KEY_B = 1 << 1,
    FINLINK_KEY_SELECT = 1 << 2,
    FINLINK_KEY_START = 1 << 3,
    FINLINK_KEY_RIGHT = 1 << 4,
    FINLINK_KEY_LEFT = 1 << 5,
    FINLINK_KEY_UP = 1 << 6,
    FINLINK_KEY_DOWN = 1 << 7,
    FINLINK_KEY_R = 1 << 8,
    FINLINK_KEY_L = 1 << 9
} finlink_key;

/* Bitmask selected per-frame by the server (whichever is cheapest for that
 * frame) describing the decompressed block's layout -- see
 * finlink_decode_video_frame(). All four combinations must be handled. */
typedef enum {
    /* Pixels are palette indices (1 byte each) preceded by a palette,
     * instead of raw u16le RGB565. */
    FINLINK_VIDEO_FORMAT_INDEXED = 1 << 0,
    /* Only the 8x8 tiles that actually changed since the last frame the
     * server sent are included, preceded by a list of which tiles those
     * are -- finlink_decode_video_frame() patches just those tiles into
     * the caller's existing framebuffer rather than overwriting all of
     * it. Unset means a full frame (every pixel included), which is also
     * always what the first frame after connecting is -- there's no
     * previous frame yet for the server to diff against, and it doubles
     * as the keyframe clients need to have painted something onto their
     * framebuffer before trusting a tile patch. */
    FINLINK_VIDEO_FORMAT_TILES = 1 << 1
} finlink_video_format;

/* Video header (type=1). compressed_data points into the caller's buffer
 * (no copy) and is a raw-deflate compressed block whose content depends on
 * `format` -- see finlink_video_format. Decompress with
 * finlink_inflate_raw() (size it with finlink_video_max_inflated_size()),
 * then decode with finlink_decode_video_frame(). */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t format; /* finlink_video_format bits, OR'd together */
    const uint8_t *compressed_data;
    size_t compressed_size;
} finlink_video_header;

/* Audio frame (type=3). samples points into the caller's buffer (no copy),
 * sample_count is the total number of s16 samples (i.e. frames * channels). */
typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    const uint8_t *samples; /* s16le, read with finlink_read_s16le() */
    size_t sample_count;
} finlink_audio_frame;

#define FINLINK_INPUT_FRAME_SIZE 3

/* Touch state for input_encoding "n3ds_touch" (docs/protocol.md,
 * N3DS_BOTTOM_SCREEN) -- a client->server type=2 message, same message
 * type as the GBA key bitmask above but a different payload shape,
 * disambiguated by hello.input_encoding rather than a separate
 * finlink_msg_type (the receiver already knows which shape to expect from
 * the handshake before any type=2 message can arrive). x/y are in the
 * bottom screen's own native pixel coordinates (0..320, 0..240) --
 * whatever screen-to-touch-coordinate mapping a touch-capable client uses
 * to get there is entirely its own concern, this is just the wire value.
 * x/y are meaningless (and should be sent as 0, ignored by the receiver)
 * whenever pressed is 0: a release carries no meaningful position, it's
 * simply "stop touching", not "touch stopped at (x,y)". */
typedef struct {
    int pressed; /* 0 = released, nonzero = touching at (x, y) */
    uint16_t x;
    uint16_t y;
} finlink_touch_state;

#define FINLINK_TOUCH_FRAME_SIZE 6

/* Combined touch + buttons + analog sticks state for input_encoding
 * "n3ds_touch_and_buttons" (docs/protocol.md) -- a client->server type=2
 * message, for stream types whose server can accept full remote control
 * input rather than only touch. Unlike "n3ds_touch" and "gba_buttons",
 * which are each exactly one shape of data, this bundles all three input
 * kinds into a single fixed frame, so the "one input_encoding == one
 * message shape" rule those two already rely on still holds -- there's no
 * second, differently-shaped type=2 message to disambiguate by size.
 *
 * touch/touch_x/touch_y: same semantics as finlink_touch_state (x/y
 * meaningless, sent as 0, whenever pressed is 0).
 *
 * buttons: a generic superset bitmask (finlink_button_bit) covering every
 * digital button any touch-capable stream type's server might accept
 * remotely -- a given server only looks at the bits its own console
 * actually has; a client with no such button to send simply never sets the
 * corresponding bit, and a server with no such button to receive safely
 * ignores it.
 *
 * left_x/left_y, right_x/right_y: analog stick state, signed range
 * -32768..32767 per axis, (0, 0) centered/at rest. left is the 3DS circle
 * pad, or on a console with two sticks (WIIU_GAMEPAD) its left stick;
 * right is always (0, 0) on a console with at most one analog stick. */
typedef struct {
    int pressed;
    uint16_t touch_x;
    uint16_t touch_y;
    uint32_t buttons;
    int16_t left_x;
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
} finlink_extended_input;

/* finlink_extended_input.buttons bit assignments -- a bit only means
 * something to a server whose console actually has that button, see
 * finlink_extended_input's own comment. Named after the button each bit
 * most directly corresponds to across consoles (e.g. FINLINK_BUTTON_X/Y
 * for a Wii U GamePad's X/Y face buttons, unused -- always clear -- for a
 * 3DS, which has no X/Y-labeled equivalent remotely reachable here). */
typedef enum {
    FINLINK_BUTTON_A = 1u << 0,
    FINLINK_BUTTON_B = 1u << 1,
    FINLINK_BUTTON_X = 1u << 2,
    FINLINK_BUTTON_Y = 1u << 3,
    FINLINK_BUTTON_L = 1u << 4,
    FINLINK_BUTTON_R = 1u << 5,
    FINLINK_BUTTON_ZL = 1u << 6,
    FINLINK_BUTTON_ZR = 1u << 7,
    FINLINK_BUTTON_SELECT = 1u << 8, /* aka Minus (Wii U) */
    FINLINK_BUTTON_START = 1u << 9,  /* aka Plus (Wii U) */
    FINLINK_BUTTON_UP = 1u << 10,
    FINLINK_BUTTON_DOWN = 1u << 11,
    FINLINK_BUTTON_LEFT = 1u << 12,
    FINLINK_BUTTON_RIGHT = 1u << 13,
    FINLINK_BUTTON_HOME = 1u << 14,
} finlink_button_bit;

#define FINLINK_EXTENDED_INPUT_FRAME_SIZE 18

/* Writes out_buf[FINLINK_EXTENDED_INPUT_FRAME_SIZE] (the caller must have
 * room for that many bytes). Returns the number of bytes written, always
 * FINLINK_EXTENDED_INPUT_FRAME_SIZE -- same convention as
 * finlink_build_touch_frame. */
size_t finlink_build_extended_input_frame(const finlink_extended_input *input,
                                           uint8_t out_buf[FINLINK_EXTENDED_INPUT_FRAME_SIZE]);

/* Only valid to call when hello.input_encoding was
 * "n3ds_touch_and_buttons" -- see finlink_extended_input's own comment. */
finlink_result finlink_parse_extended_input_frame(const uint8_t *data, size_t size,
                                                   finlink_extended_input *out);

/* Touch + buttons for input_encoding "touch_and_buttons" (docs/protocol.md)
 * -- a client->server type=2 message, for touch-capable stream types whose
 * console has no analog stick at all (currently only NDS_BOTTOM_SCREEN).
 * Same touch/buttons semantics as finlink_extended_input (touch_x/touch_y
 * meaningless when pressed is 0; buttons is the same finlink_button_bit
 * superset, a given server only looking at the bits its own console
 * actually has), just without the two always-zero stick fields
 * finlink_extended_input would otherwise carry on a console with none --
 * a distinct, smaller wire shape rather than a duplicate of that one with
 * padding. */
typedef struct {
    int pressed;
    uint16_t touch_x;
    uint16_t touch_y;
    uint32_t buttons;
} finlink_touch_and_buttons;

#define FINLINK_TOUCH_AND_BUTTONS_FRAME_SIZE 10

/* Writes out_buf[FINLINK_TOUCH_AND_BUTTONS_FRAME_SIZE] (the caller must
 * have room for that many bytes). Returns the number of bytes written,
 * always FINLINK_TOUCH_AND_BUTTONS_FRAME_SIZE -- same convention as
 * finlink_build_touch_frame. */
size_t finlink_build_touch_and_buttons_frame(const finlink_touch_and_buttons *input,
                                              uint8_t out_buf[FINLINK_TOUCH_AND_BUTTONS_FRAME_SIZE]);

/* Only valid to call when hello.input_encoding was "touch_and_buttons" --
 * see finlink_touch_and_buttons's own comment. */
finlink_result finlink_parse_touch_and_buttons_frame(const uint8_t *data, size_t size,
                                                      finlink_touch_and_buttons *out);

/* Reads the leading type byte of a server->client message without consuming
 * the rest. `size` must be >= 1. */
finlink_result finlink_peek_type(const uint8_t *data, size_t size, finlink_msg_type *out_type);

/* Parses a type=1 message. `data` must start at the type byte. */
finlink_result finlink_parse_video_header(const uint8_t *data, size_t size, finlink_video_header *out);

/* Upper bound on finlink_inflate_raw()'s output size for a video message of
 * this width/height, across all four finlink_video_format combinations --
 * use this to size the buffer passed to finlink_inflate_raw(), not
 * width*height*2 (only exactly right for a full non-indexed frame; a
 * worst-case TILES frame needs slightly more, for the per-tile index
 * list). */
size_t finlink_video_max_inflated_size(uint32_t width, uint32_t height);

/* Decodes the finlink_inflate_raw() output of a video message's
 * compressed_data (format-dependent, see finlink_video_format) into
 * framebuffer_rgb565: width*height u16le RGB565 pixels, row-major.
 *
 * framebuffer_rgb565 is the caller's PERSISTENT framebuffer, not a
 * scratch/output-only buffer -- it must be preserved between calls for
 * the same stream (must have room for width*height*2 bytes, same
 * width/height every call). If format has FINLINK_VIDEO_FORMAT_TILES set,
 * only the pixels belonging to the changed tiles are overwritten; every
 * other pixel keeps whatever finlink_decode_video_frame() last wrote
 * there. Without that bit, the whole framebuffer is overwritten (this is
 * always true for the first frame after connecting, which the caller
 * should treat as the point its framebuffer becomes valid to display --
 * see docs/protocol.md). */
finlink_result finlink_decode_video_frame(uint8_t format, const uint8_t *inflated, size_t inflated_size,
                                           uint32_t width, uint32_t height, uint8_t *framebuffer_rgb565,
                                           size_t framebuffer_capacity);

/* Parses a type=3 message. `data` must start at the type byte. */
finlink_result finlink_parse_audio_frame(const uint8_t *data, size_t size, finlink_audio_frame *out);

/* Writes a type=2 message into out_buf (must have room for
 * FINLINK_INPUT_FRAME_SIZE bytes). Returns the number of bytes written. */
size_t finlink_build_input_frame(uint16_t key_bitmask, uint8_t out_buf[FINLINK_INPUT_FRAME_SIZE]);

/* Writes a type=2 "n3ds_touch"-encoding message into out_buf (must have
 * room for FINLINK_TOUCH_FRAME_SIZE bytes). Returns the number of bytes
 * written. */
size_t finlink_build_touch_frame(const finlink_touch_state *touch, uint8_t out_buf[FINLINK_TOUCH_FRAME_SIZE]);

/* Parses a type=2 "n3ds_touch"-encoding message. `data` must start at the
 * type byte. Only valid to call when hello.input_encoding was
 * "n3ds_touch" -- see finlink_touch_state's own comment. */
finlink_result finlink_parse_touch_frame(const uint8_t *data, size_t size, finlink_touch_state *out);

/* Text input request (type=4), server->client -- sent when the emulated
 * console wants to show its own on-screen software keyboard (e.g. Cemu's
 * swkbd, 3DS's swkbd applet), whose local rendering the video stream never
 * captures (it's drawn as a host-side UI overlay on top of the emulated
 * framebuffer, not part of it). Prompts the client to show its own native
 * text input UI instead of leaving the remote user with no way to type at
 * all. `text` points into the caller's buffer (no copy); empty if there's
 * no initial/pre-filled text to show. */
typedef struct {
    uint32_t max_length; /* in characters; 0 = no limit enforced by the server */
    const char *text;    /* utf8, initial/pre-filled text */
    size_t text_len;      /* in bytes */
} finlink_text_input_request;

/* Header only (type + max_length + text_len); the text itself follows and
 * is variable-length, see finlink_text_input_request_max_size(). */
#define FINLINK_TEXT_INPUT_REQUEST_HEADER_SIZE 9

size_t finlink_text_input_request_max_size(size_t text_len);

/* Writes into out_buf (sized with finlink_text_input_request_max_size()).
 * Returns the number of bytes written, or 0 if out_capacity was too small. */
size_t finlink_build_text_input_request(const finlink_text_input_request *req, uint8_t *out_buf,
                                         size_t out_capacity);

/* Parses a type=4 message. `data` must start at the type byte. */
finlink_result finlink_parse_text_input_request(const uint8_t *data, size_t size,
                                                 finlink_text_input_request *out);

/* Text input response (type=5), client->server -- the user's typed result.
 * confirmed=0 means the user cancelled (dismissed the client's text input
 * UI without submitting); text/text_len are meaningless in that case and
 * the server should leave whatever text was already there unchanged. */
typedef struct {
    int confirmed;
    const char *text; /* utf8 */
    size_t text_len;   /* in bytes */
} finlink_text_input_response;

/* Header only (type + confirmed + text_len); the text itself follows and is
 * variable-length, see finlink_text_input_response_max_size(). */
#define FINLINK_TEXT_INPUT_RESPONSE_HEADER_SIZE 6

size_t finlink_text_input_response_max_size(size_t text_len);

/* Writes into out_buf (sized with finlink_text_input_response_max_size()).
 * Returns the number of bytes written, or 0 if out_capacity was too small. */
size_t finlink_build_text_input_response(const finlink_text_input_response *resp, uint8_t *out_buf,
                                          size_t out_capacity);

/* Parses a type=5 message. `data` must start at the type byte. */
finlink_result finlink_parse_text_input_response(const uint8_t *data, size_t size,
                                                  finlink_text_input_response *out);

/* Mic input enable (type=6), server->client -- tells the client whether the
 * emulated console currently wants microphone input, and at what sample
 * rate to capture it at. Mirrors real hardware: the physical mic (Wii U
 * GamePad, 3DS) is only actually active while a game has powered it on and
 * is sampling, not continuously just because a stream is connected -- so
 * the client should only capture and upload its own microphone while the
 * most recent message had enabled=1, stopping as soon as one with
 * enabled=0 arrives. A level signal (safe to resend the same value
 * idempotently), not an edge/toggle. sample_rate is meaningless when
 * enabled=0. */
typedef struct {
    int enabled;
    uint32_t sample_rate;
} finlink_mic_enable;

#define FINLINK_MIC_ENABLE_FRAME_SIZE 6

/* Writes out_buf[FINLINK_MIC_ENABLE_FRAME_SIZE] (the caller must have room
 * for that many bytes). Returns the number of bytes written, always
 * FINLINK_MIC_ENABLE_FRAME_SIZE. */
size_t finlink_build_mic_enable_frame(const finlink_mic_enable *enable,
                                      uint8_t out_buf[FINLINK_MIC_ENABLE_FRAME_SIZE]);

/* Parses a type=6 message. `data` must start at the type byte. */
finlink_result finlink_parse_mic_enable_frame(const uint8_t *data, size_t size,
                                               finlink_mic_enable *out);

/* Mic input audio (type=7), client->server -- raw PCM samples captured from
 * the client's own microphone, sent while a FINLINK_MSG_MIC_ENABLE(1) is in
 * effect. Always mono (consoles with a mic input, Wii U GamePad and 3DS,
 * only ever take a single channel). Same wire shape as finlink_audio_frame
 * (type=3, which is always server->client console/speaker audio -- this is
 * the reverse direction, reusing the identical struct since nothing about
 * the layout differs). No build helper here since, like FINLINK_MSG_AUDIO,
 * there's currently exactly one implementation producing this message
 * (the Android client) and it hand-builds it the same way that
 * implementation hand-builds everything else it sends. */
finlink_result finlink_parse_mic_audio_frame(const uint8_t *data, size_t size,
                                              finlink_audio_frame *out);

#ifdef __cplusplus
}
#endif

#endif /* FINLINK_PROTOCOL_H */

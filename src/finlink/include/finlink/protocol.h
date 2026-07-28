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

#ifdef __cplusplus
}
#endif

#endif /* FINLINK_PROTOCOL_H */

#ifndef FINLINK_INFLATE_H
#define FINLINK_INFLATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FINLINK_INFLATE_OK = 0,
    FINLINK_INFLATE_ERR = -1 /* corrupt input, or out_capacity too small */
} finlink_inflate_status;

/* Decompresses a raw-deflate buffer (no zlib/gzip header, matching the
 * video payload in docs/protocol.md) fully into out_buf.
 *
 * out_capacity must be >= the expected decompressed size (width * height * 2
 * for a video frame). On success *out_size holds the actual decompressed
 * size, which should equal width * height * 2 for a well-formed frame. */
finlink_inflate_status finlink_inflate_raw(const uint8_t *src, size_t src_size,
                                            uint8_t *out_buf, size_t out_capacity,
                                            size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif /* FINLINK_INFLATE_H */

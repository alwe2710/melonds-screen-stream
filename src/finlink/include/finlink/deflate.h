#ifndef FINLINK_DEFLATE_H
#define FINLINK_DEFLATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FINLINK_DEFLATE_OK = 0,
    FINLINK_DEFLATE_ERR = -1 /* out_capacity too small for the compressed output */
} finlink_deflate_status;

/* Upper bound on finlink_deflate_raw()'s output size for a given input
 * size -- use this to size out_buf when compression isn't guaranteed to
 * shrink the input (e.g. already-noisy/high-entropy pixel data). Standard
 * "stored block" deflate worst case: every input byte plus 5 bytes of
 * block-header overhead per ~65535-byte block, plus a small fixed margin. */
size_t finlink_deflate_max_size(size_t src_size);

/* Compresses src into a raw-deflate stream (no zlib/gzip header, matching
 * what finlink_inflate_raw() expects and what the video payload in
 * docs/protocol.md requires) written to out_buf.
 *
 * out_buf must have room for at least finlink_deflate_max_size(src_size)
 * bytes. On success *out_size holds the actual compressed size. Intended
 * for a server encoding an outgoing video frame -- no existing finlink
 * client needs this (they only ever decompress), see finlink/inflate.h for
 * that side. */
finlink_deflate_status finlink_deflate_raw(const uint8_t *src, size_t src_size, uint8_t *out_buf,
                                            size_t out_capacity, size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif /* FINLINK_DEFLATE_H */

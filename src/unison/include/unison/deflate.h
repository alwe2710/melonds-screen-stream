#ifndef UNISON_DEFLATE_H
#define UNISON_DEFLATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UNISON_DEFLATE_OK = 0,
    UNISON_DEFLATE_ERR = -1 /* out_capacity too small for the compressed output */
} unison_deflate_status;

/* Upper bound on unison_deflate_raw()'s output size for a given input
 * size -- use this to size out_buf when compression isn't guaranteed to
 * shrink the input (e.g. already-noisy/high-entropy pixel data). Standard
 * "stored block" deflate worst case: every input byte plus 5 bytes of
 * block-header overhead per ~65535-byte block, plus a small fixed margin. */
size_t unison_deflate_max_size(size_t src_size);

/* Compresses src into a raw-deflate stream (no zlib/gzip header, matching
 * what unison_inflate_raw() expects and what the video payload in
 * docs/protocol.md requires) written to out_buf.
 *
 * out_buf must have room for at least unison_deflate_max_size(src_size)
 * bytes. On success *out_size holds the actual compressed size. Intended
 * for a server encoding an outgoing video frame -- no existing Unison
 * client needs this (they only ever decompress), see unison/inflate.h for
 * that side. */
unison_deflate_status unison_deflate_raw(const uint8_t *src, size_t src_size, uint8_t *out_buf,
                                            size_t out_capacity, size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif /* UNISON_DEFLATE_H */

#include "finlink/deflate.h"

#include "miniz_tdef.h"

size_t finlink_deflate_max_size(size_t src_size) {
    /* Worst case ("stored"/uncompressed) deflate blocks: up to 65535 bytes
     * of source per block, 5 bytes of block-header overhead
     * (1 byte type + 2 bytes LEN + 2 bytes ~LEN) per block, plus a small
     * fixed margin for the final block. No zlib header/Adler-32 trailer in
     * raw mode (TDEFL_WRITE_ZLIB_HEADER unset below), so nothing to add
     * for those. */
    return src_size + 5 * (src_size / 65535 + 1) + 16;
}

finlink_deflate_status finlink_deflate_raw(const uint8_t *src, size_t src_size, uint8_t *out_buf,
                                            size_t out_capacity, size_t *out_size) {
    /* No TDEFL_WRITE_ZLIB_HEADER: raw deflate output, matching what
     * finlink_inflate_raw() (and every client's decoder) expects -- see
     * docs/protocol.md's video payload format. Default probe depth
     * (TDEFL_DEFAULT_MAX_PROBES, see miniz_tdef.h) rather than
     * TDEFL_HUFFMAN_ONLY/greedy-parsing flags: this runs on a server with
     * real CPU budget to spend, unlike finlink_inflate_raw()'s
     * memory-constrained homebrew decoders, so there's no reason to trade
     * compression ratio away here. */
    /* tdefl_compress_mem_to_mem() itself uses 0 as its only failure
     * sentinel (out_capacity too small, or a null out_buf) -- ambiguous in
     * principle with "compressed to genuinely zero bytes", but that never
     * happens for real deflate output (even empty input still emits a
     * final-block marker), and every real caller here is compressing an
     * actual video frame, never zero bytes. */
    size_t written = tdefl_compress_mem_to_mem(out_buf, out_capacity, src, src_size, 0);
    if (written == 0) {
        return FINLINK_DEFLATE_ERR;
    }

    *out_size = written;
    return FINLINK_DEFLATE_OK;
}

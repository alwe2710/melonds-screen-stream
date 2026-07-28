#include "finlink/inflate.h"

#include "miniz_tinfl.h"

finlink_inflate_status finlink_inflate_raw(const uint8_t *src, size_t src_size,
                                            uint8_t *out_buf, size_t out_capacity,
                                            size_t *out_size) {
    /* Whole compressed frame is already in memory (the WebSocket layer
     * delivers complete messages), and out_buf is sized for the full frame
     * rather than a sliding 32KB window, so USING_NON_WRAPPING_OUTPUT_BUF
     * applies. No zlib header per the wire protocol, hence no
     * TINFL_FLAG_PARSE_ZLIB_HEADER. */
    size_t written = tinfl_decompress_mem_to_mem(
        out_buf, out_capacity, src, src_size, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);

    if (written == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        return FINLINK_INFLATE_ERR;
    }

    *out_size = written;
    return FINLINK_INFLATE_OK;
}

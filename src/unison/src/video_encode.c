#include "unison/video_encode.h"

#include "unison/deflate.h"
#include "unison/endian.h"
#include "unison/protocol.h"

#include <string.h>

static size_t tiles_per_row(uint32_t width) {
    return ((size_t)width + 7) / 8;
}

static size_t tiles_per_col(uint32_t height) {
    return ((size_t)height + 7) / 8;
}

size_t unison_video_encode_max_size(uint32_t width, uint32_t height) {
    return unison_deflate_max_size(unison_video_encode_scratch_size(width, height));
}

size_t unison_video_encode_scratch_size(uint32_t width, uint32_t height) {
    size_t max_tile_count = tiles_per_row(width) * tiles_per_col(height);
    /* u16 tile_count header + up to max_tile_count u16 indices + up to
     * max_tile_count full padded 8x8 (64px * 2 bytes) tiles -- see the
     * header comment on why this can exceed width*height*2. */
    return 2 + max_tile_count * 2 + max_tile_count * 64 * 2;
}

/* True if any in-bounds pixel of the 8x8 tile at (tile_col, tile_row)
 * differs between current_rgb565 and previous_rgb565 -- out-of-bounds
 * pixels (width/height not a multiple of 8) never participate, same as
 * unison_decode_video_frame() never writing them. */
static int tile_changed(const uint8_t *current_rgb565, const uint8_t *previous_rgb565, uint32_t width,
                         uint32_t height, size_t tile_col, size_t tile_row) {
    size_t origin_x = tile_col * 8;
    size_t origin_y = tile_row * 8;
    size_t rows = (origin_y + 8 <= height) ? 8 : (height - origin_y);
    size_t cols = (origin_x + 8 <= width) ? 8 : (width - origin_x);

    for (size_t ty = 0; ty < rows; ty++) {
        size_t row_offset = ((origin_y + ty) * (size_t)width + origin_x) * 2;
        if (memcmp(current_rgb565 + row_offset, previous_rgb565 + row_offset, cols * 2) != 0) {
            return 1;
        }
    }
    return 0;
}

/* Writes one tile's 64 pixels (row-major, per docs/protocol.md) into
 * dst -- out-of-bounds positions (edge tiles) are written as 0, never read
 * back by unison_decode_video_frame() since it bounds-checks px/py before
 * writing to the framebuffer, but the fixed 64-pixel-per-tile layout still
 * needs *something* in that slot to keep every later tile's offset correct. */
static void write_tile_pixels(uint8_t *dst, const uint8_t *current_rgb565, uint32_t width, uint32_t height,
                               size_t tile_col, size_t tile_row) {
    size_t origin_x = tile_col * 8;
    size_t origin_y = tile_row * 8;

    for (size_t ty = 0; ty < 8; ty++) {
        size_t py = origin_y + ty;
        for (size_t tx = 0; tx < 8; tx++) {
            size_t px = origin_x + tx;
            size_t dst_offset = (ty * 8 + tx) * 2;
            if (py < height && px < width) {
                size_t src_offset = (py * (size_t)width + px) * 2;
                memcpy(dst + dst_offset, current_rgb565 + src_offset, 2);
            } else {
                dst[dst_offset] = 0;
                dst[dst_offset + 1] = 0;
            }
        }
    }
}

static unison_encode_status encode_full_frame(const uint8_t *current_rgb565, uint32_t width, uint32_t height,
                                                 uint8_t *out_buf, size_t out_capacity, size_t *out_size,
                                                 uint8_t *out_format) {
    size_t pixel_bytes = (size_t)width * (size_t)height * 2;
    if (unison_deflate_raw(current_rgb565, pixel_bytes, out_buf, out_capacity, out_size) != UNISON_DEFLATE_OK) {
        return UNISON_ENCODE_ERR;
    }
    *out_format = 0;
    return UNISON_ENCODE_OK;
}

unison_encode_status unison_encode_video_frame(const uint8_t *current_rgb565,
                                                   const uint8_t *previous_rgb565, uint32_t width,
                                                   uint32_t height, uint8_t *scratch_buf,
                                                   size_t scratch_capacity, uint8_t *out_buf,
                                                   size_t out_capacity, size_t *out_size,
                                                   uint8_t *out_format) {
    if (previous_rgb565 == NULL) {
        return encode_full_frame(current_rgb565, width, height, out_buf, out_capacity, out_size, out_format);
    }

    size_t row_stride = tiles_per_row(width);
    size_t col_count = tiles_per_col(height);
    size_t max_tile_count = row_stride * col_count;

    /* scratch layout while counting/collecting changed tiles: the u16le
     * index list starts right after the (not-yet-known) u16le tile_count,
     * i.e. at scratch_buf + 2 -- filled in once the final count is known,
     * same "write payload first, patch the count header last" approach
     * unison_deflate_raw()'s caller-owns-buffers style already implies. */
    if (scratch_capacity < 2) {
        return UNISON_ENCODE_ERR;
    }
    uint8_t *index_list = scratch_buf + 2;
    size_t indices_capacity = scratch_capacity - 2;
    if (indices_capacity < max_tile_count * 2) {
        return UNISON_ENCODE_ERR;
    }

    uint16_t tile_count = 0;
    for (size_t tile_row = 0; tile_row < col_count; tile_row++) {
        for (size_t tile_col = 0; tile_col < row_stride; tile_col++) {
            if (tile_changed(current_rgb565, previous_rgb565, width, height, tile_col, tile_row)) {
                uint16_t tile_index = (uint16_t)(tile_row * row_stride + tile_col);
                unison_write_u16le(index_list + (size_t)tile_count * 2, tile_index);
                tile_count++;
            }
        }
    }

    if (tile_count == 0) {
        return UNISON_ENCODE_UNCHANGED;
    }

    size_t pixel_data_offset = 2 + (size_t)tile_count * 2;
    size_t pixel_data_bytes = (size_t)tile_count * 64 * 2;
    if (scratch_capacity < pixel_data_offset + pixel_data_bytes) {
        return UNISON_ENCODE_ERR;
    }
    unison_write_u16le(scratch_buf, tile_count);

    /* Second pass, independently re-deciding "changed?" per tile (cheap --
     * a handful of memcmp calls) rather than reading back the index list
     * from the first pass: avoids the two passes needing to agree on
     * position-in-list bookkeeping, since both just walk tiles in the same
     * deterministic raster order and only the destination offset
     * (written_tiles) differs from pass to pass. */
    uint8_t *pixel_data = scratch_buf + pixel_data_offset;
    size_t written_tiles = 0;
    for (size_t tile_row = 0; tile_row < col_count; tile_row++) {
        for (size_t tile_col = 0; tile_col < row_stride; tile_col++) {
            if (!tile_changed(current_rgb565, previous_rgb565, width, height, tile_col, tile_row)) {
                continue;
            }
            write_tile_pixels(pixel_data + written_tiles * 64 * 2, current_rgb565, width, height, tile_col,
                               tile_row);
            written_tiles++;
        }
    }

    if (unison_deflate_raw(scratch_buf, pixel_data_offset + pixel_data_bytes, out_buf, out_capacity, out_size) !=
        UNISON_DEFLATE_OK) {
        return UNISON_ENCODE_ERR;
    }
    *out_format = UNISON_VIDEO_FORMAT_TILES;
    return UNISON_ENCODE_OK;
}

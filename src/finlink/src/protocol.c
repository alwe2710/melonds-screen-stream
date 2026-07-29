#include "finlink/protocol.h"
#include "finlink/endian.h"

#include <stdbool.h>
#include <string.h>

#define VIDEO_HEADER_SIZE 10 /* type(1) + width(4) + height(4) + format(1) */
#define AUDIO_HEADER_SIZE 6  /* type(1) + sampleRate(4) + channels(1) */

finlink_result finlink_peek_type(const uint8_t *data, size_t size, finlink_msg_type *out_type) {
    if (size < 1) {
        return FINLINK_ERR_TOO_SHORT;
    }

    switch (data[0]) {
        case FINLINK_MSG_VIDEO:
        case FINLINK_MSG_INPUT:
        case FINLINK_MSG_AUDIO:
        case FINLINK_MSG_TEXT_INPUT_REQUEST:
        case FINLINK_MSG_TEXT_INPUT_RESPONSE:
        case FINLINK_MSG_MIC_ENABLE:
        case FINLINK_MSG_MIC_AUDIO:
            *out_type = (finlink_msg_type)data[0];
            return FINLINK_OK;
        default:
            return FINLINK_ERR_UNKNOWN_TYPE;
    }
}

finlink_result finlink_parse_video_header(const uint8_t *data, size_t size, finlink_video_header *out) {
    if (size < VIDEO_HEADER_SIZE) {
        return FINLINK_ERR_TOO_SHORT;
    }
    if (data[0] != FINLINK_MSG_VIDEO) {
        return FINLINK_ERR_UNKNOWN_TYPE;
    }

    out->width = finlink_read_u32le(data + 1);
    out->height = finlink_read_u32le(data + 5);
    out->format = data[9];
    out->compressed_data = data + VIDEO_HEADER_SIZE;
    out->compressed_size = size - VIDEO_HEADER_SIZE;
    return FINLINK_OK;
}

static size_t tiles_per_row(uint32_t width) {
    return ((size_t)width + 7) / 8;
}

static size_t tiles_per_col(uint32_t height) {
    return ((size_t)height + 7) / 8;
}

size_t finlink_video_max_inflated_size(uint32_t width, uint32_t height) {
    size_t max_tile_count = tiles_per_row(width) * tiles_per_col(height);
    /* Worst case across all four format combinations: a full-image frame
     * (width*height*2, the non-indexed pixel data alone), plus the
     * largest possible header overhead (TILES' u16 count + up to
     * max_tile_count u16 indices, INDEXED's u16 count + up to 256 u16
     * palette entries) -- generous rather than tight, this only sizes a
     * scratch buffer. */
    return (size_t)width * (size_t)height * 2 + 4 + max_tile_count * 2 + 512;
}

/* Reads one pixel (a palette index if `indexed`, otherwise raw RGB565)
 * from `pixel_data` at `pixel_offset`, resolving through `palette` if
 * needed. Returns false (leaving *out_color unset) if the data doesn't
 * support the read -- caller propagates that as FINLINK_ERR_TOO_SHORT. */
static bool read_pixel(bool indexed, const uint8_t *pixel_data, size_t pixel_offset, const uint8_t *palette,
                        uint16_t palette_count, uint16_t *out_color) {
    if (indexed) {
        uint8_t idx = pixel_data[pixel_offset];
        if (idx >= palette_count) {
            return false; /* corrupt: index outside the palette it came with */
        }
        *out_color = finlink_read_u16le(palette + (size_t)idx * 2);
    } else {
        *out_color = finlink_read_u16le(pixel_data + pixel_offset * 2);
    }
    return true;
}

finlink_result finlink_decode_video_frame(uint8_t format, const uint8_t *inflated, size_t inflated_size,
                                           uint32_t width, uint32_t height, uint8_t *framebuffer_rgb565,
                                           size_t framebuffer_capacity) {
    if (format & ~(uint8_t)(FINLINK_VIDEO_FORMAT_INDEXED | FINLINK_VIDEO_FORMAT_TILES)) {
        return FINLINK_ERR_UNKNOWN_FORMAT;
    }

    bool indexed = (format & FINLINK_VIDEO_FORMAT_INDEXED) != 0;
    bool tiled = (format & FINLINK_VIDEO_FORMAT_TILES) != 0;

    size_t pixel_count = (size_t)width * (size_t)height;
    size_t needed = pixel_count * 2;
    if (framebuffer_capacity < needed) {
        return FINLINK_ERR_TOO_SHORT;
    }

    size_t offset = 0;

    uint16_t tile_count = 0;
    const uint8_t *tile_indices = NULL;
    if (tiled) {
        if (inflated_size < offset + 2) {
            return FINLINK_ERR_TOO_SHORT;
        }
        tile_count = finlink_read_u16le(inflated + offset);
        offset += 2;

        size_t tile_list_bytes = (size_t)tile_count * 2;
        if (inflated_size < offset + tile_list_bytes) {
            return FINLINK_ERR_TOO_SHORT;
        }
        tile_indices = inflated + offset;
        offset += tile_list_bytes;
    }

    uint16_t palette_count = 0;
    const uint8_t *palette = NULL;
    if (indexed) {
        if (inflated_size < offset + 2) {
            return FINLINK_ERR_TOO_SHORT;
        }
        palette_count = finlink_read_u16le(inflated + offset); /* 1-256 per docs/protocol.md */
        offset += 2;
        if (palette_count == 0) {
            return FINLINK_ERR_TOO_SHORT;
        }

        size_t palette_bytes = (size_t)palette_count * 2;
        if (inflated_size < offset + palette_bytes) {
            return FINLINK_ERR_TOO_SHORT;
        }
        palette = inflated + offset;
        offset += palette_bytes;
    }

    const uint8_t *pixel_data = inflated + offset;
    size_t pixel_data_available = inflated_size - offset;
    size_t bytes_per_pixel_in = indexed ? 1 : 2;

    if (!tiled) {
        if (pixel_data_available < pixel_count * bytes_per_pixel_in) {
            return FINLINK_ERR_TOO_SHORT;
        }
        for (size_t i = 0; i < pixel_count; i++) {
            uint16_t color;
            if (!read_pixel(indexed, pixel_data, i, palette, palette_count, &color)) {
                return FINLINK_ERR_TOO_SHORT;
            }
            finlink_write_u16le(framebuffer_rgb565 + i * 2, color);
        }
        return FINLINK_OK;
    }

    /* Tiled: only patch the pixels belonging to the listed tiles: every
     * other pixel in framebuffer_rgb565 is left exactly as it was. */
    if (pixel_data_available < (size_t)tile_count * 64 * bytes_per_pixel_in) {
        return FINLINK_ERR_TOO_SHORT;
    }

    size_t row_stride = tiles_per_row(width);
    size_t max_tile_count = row_stride * tiles_per_col(height);

    for (uint16_t t = 0; t < tile_count; t++) {
        uint16_t tile_index = finlink_read_u16le(tile_indices + (size_t)t * 2);
        if (tile_index >= max_tile_count) {
            return FINLINK_ERR_TOO_SHORT; /* corrupt: tile outside the frame */
        }

        size_t tile_col = (size_t)tile_index % row_stride;
        size_t tile_row = (size_t)tile_index / row_stride;
        size_t origin_x = tile_col * 8;
        size_t origin_y = tile_row * 8;

        for (size_t ty = 0; ty < 8; ty++) {
            size_t py = origin_y + ty;
            if (py >= height) {
                continue; /* width/height not a multiple of 8: partial edge tile */
            }
            for (size_t tx = 0; tx < 8; tx++) {
                size_t px = origin_x + tx;
                if (px >= width) {
                    continue;
                }

                size_t src_pixel = (size_t)t * 64 + ty * 8 + tx;
                uint16_t color;
                if (!read_pixel(indexed, pixel_data, src_pixel, palette, palette_count, &color)) {
                    return FINLINK_ERR_TOO_SHORT;
                }

                size_t dst_pixel = py * (size_t)width + px;
                finlink_write_u16le(framebuffer_rgb565 + dst_pixel * 2, color);
            }
        }
    }
    return FINLINK_OK;
}

finlink_result finlink_parse_audio_frame(const uint8_t *data, size_t size, finlink_audio_frame *out) {
    if (size < AUDIO_HEADER_SIZE) {
        return FINLINK_ERR_TOO_SHORT;
    }
    if (data[0] != FINLINK_MSG_AUDIO) {
        return FINLINK_ERR_UNKNOWN_TYPE;
    }

    out->sample_rate = finlink_read_u32le(data + 1);
    out->channels = data[5];
    out->samples = data + AUDIO_HEADER_SIZE;
    out->sample_count = (size - AUDIO_HEADER_SIZE) / sizeof(int16_t);
    return FINLINK_OK;
}

size_t finlink_build_input_frame(uint16_t key_bitmask, uint8_t out_buf[FINLINK_INPUT_FRAME_SIZE]) {
    out_buf[0] = FINLINK_MSG_INPUT;
    finlink_write_u16le(out_buf + 1, key_bitmask);
    return FINLINK_INPUT_FRAME_SIZE;
}

size_t finlink_build_touch_frame(const finlink_touch_state *touch, uint8_t out_buf[FINLINK_TOUCH_FRAME_SIZE]) {
    out_buf[0] = FINLINK_MSG_INPUT;
    out_buf[1] = touch->pressed ? 1 : 0;
    finlink_write_u16le(out_buf + 2, touch->pressed ? touch->x : 0);
    finlink_write_u16le(out_buf + 4, touch->pressed ? touch->y : 0);
    return FINLINK_TOUCH_FRAME_SIZE;
}

finlink_result finlink_parse_touch_frame(const uint8_t *data, size_t size, finlink_touch_state *out) {
    if (size < FINLINK_TOUCH_FRAME_SIZE) {
        return FINLINK_ERR_TOO_SHORT;
    }
    if (data[0] != FINLINK_MSG_INPUT) {
        return FINLINK_ERR_UNKNOWN_TYPE;
    }
    out->pressed = data[1] != 0;
    out->x = finlink_read_u16le(data + 2);
    out->y = finlink_read_u16le(data + 4);
    return FINLINK_OK;
}

size_t finlink_build_extended_input_frame(const finlink_extended_input *input,
                                           uint8_t out_buf[FINLINK_EXTENDED_INPUT_FRAME_SIZE]) {
    out_buf[0] = FINLINK_MSG_INPUT;
    out_buf[1] = input->pressed ? 1 : 0;
    finlink_write_u16le(out_buf + 2, input->pressed ? input->touch_x : 0);
    finlink_write_u16le(out_buf + 4, input->pressed ? input->touch_y : 0);
    finlink_write_u32le(out_buf + 6, input->buttons);
    finlink_write_u16le(out_buf + 10, (uint16_t)input->left_x);
    finlink_write_u16le(out_buf + 12, (uint16_t)input->left_y);
    finlink_write_u16le(out_buf + 14, (uint16_t)input->right_x);
    finlink_write_u16le(out_buf + 16, (uint16_t)input->right_y);
    return FINLINK_EXTENDED_INPUT_FRAME_SIZE;
}

finlink_result finlink_parse_extended_input_frame(const uint8_t *data, size_t size,
                                                   finlink_extended_input *out) {
    if (size < FINLINK_EXTENDED_INPUT_FRAME_SIZE) {
        return FINLINK_ERR_TOO_SHORT;
    }
    if (data[0] != FINLINK_MSG_INPUT) {
        return FINLINK_ERR_UNKNOWN_TYPE;
    }
    out->pressed = data[1] != 0;
    out->touch_x = finlink_read_u16le(data + 2);
    out->touch_y = finlink_read_u16le(data + 4);
    out->buttons = finlink_read_u32le(data + 6);
    out->left_x = finlink_read_s16le(data + 10);
    out->left_y = finlink_read_s16le(data + 12);
    out->right_x = finlink_read_s16le(data + 14);
    out->right_y = finlink_read_s16le(data + 16);
    return FINLINK_OK;
}

size_t finlink_build_touch_and_buttons_frame(const finlink_touch_and_buttons *input,
                                              uint8_t out_buf[FINLINK_TOUCH_AND_BUTTONS_FRAME_SIZE]) {
    out_buf[0] = FINLINK_MSG_INPUT;
    out_buf[1] = input->pressed ? 1 : 0;
    finlink_write_u16le(out_buf + 2, input->pressed ? input->touch_x : 0);
    finlink_write_u16le(out_buf + 4, input->pressed ? input->touch_y : 0);
    finlink_write_u32le(out_buf + 6, input->buttons);
    return FINLINK_TOUCH_AND_BUTTONS_FRAME_SIZE;
}

finlink_result finlink_parse_touch_and_buttons_frame(const uint8_t *data, size_t size,
                                                      finlink_touch_and_buttons *out) {
    if (size < FINLINK_TOUCH_AND_BUTTONS_FRAME_SIZE) {
        return FINLINK_ERR_TOO_SHORT;
    }
    if (data[0] != FINLINK_MSG_INPUT) {
        return FINLINK_ERR_UNKNOWN_TYPE;
    }
    out->pressed = data[1] != 0;
    out->touch_x = finlink_read_u16le(data + 2);
    out->touch_y = finlink_read_u16le(data + 4);
    out->buttons = finlink_read_u32le(data + 6);
    return FINLINK_OK;
}

size_t finlink_text_input_request_max_size(size_t text_len) {
    return FINLINK_TEXT_INPUT_REQUEST_HEADER_SIZE + text_len;
}

size_t finlink_build_text_input_request(const finlink_text_input_request *req, uint8_t *out_buf,
                                         size_t out_capacity) {
    const size_t needed = FINLINK_TEXT_INPUT_REQUEST_HEADER_SIZE + req->text_len;
    if (out_capacity < needed) {
        return 0;
    }
    out_buf[0] = FINLINK_MSG_TEXT_INPUT_REQUEST;
    finlink_write_u32le(out_buf + 1, req->max_length);
    finlink_write_u32le(out_buf + 5, (uint32_t)req->text_len);
    if (req->text_len > 0) {
        memcpy(out_buf + FINLINK_TEXT_INPUT_REQUEST_HEADER_SIZE, req->text, req->text_len);
    }
    return needed;
}

finlink_result finlink_parse_text_input_request(const uint8_t *data, size_t size,
                                                  finlink_text_input_request *out) {
    if (size < FINLINK_TEXT_INPUT_REQUEST_HEADER_SIZE) {
        return FINLINK_ERR_TOO_SHORT;
    }
    if (data[0] != FINLINK_MSG_TEXT_INPUT_REQUEST) {
        return FINLINK_ERR_UNKNOWN_TYPE;
    }
    out->max_length = finlink_read_u32le(data + 1);
    const size_t text_len = finlink_read_u32le(data + 5);
    if (size < FINLINK_TEXT_INPUT_REQUEST_HEADER_SIZE + text_len) {
        return FINLINK_ERR_TOO_SHORT;
    }
    out->text = (const char *)(data + FINLINK_TEXT_INPUT_REQUEST_HEADER_SIZE);
    out->text_len = text_len;
    return FINLINK_OK;
}

size_t finlink_text_input_response_max_size(size_t text_len) {
    return FINLINK_TEXT_INPUT_RESPONSE_HEADER_SIZE + text_len;
}

size_t finlink_build_text_input_response(const finlink_text_input_response *resp, uint8_t *out_buf,
                                          size_t out_capacity) {
    const size_t needed = FINLINK_TEXT_INPUT_RESPONSE_HEADER_SIZE + resp->text_len;
    if (out_capacity < needed) {
        return 0;
    }
    out_buf[0] = FINLINK_MSG_TEXT_INPUT_RESPONSE;
    out_buf[1] = resp->confirmed ? 1 : 0;
    finlink_write_u32le(out_buf + 2, (uint32_t)resp->text_len);
    if (resp->text_len > 0) {
        memcpy(out_buf + FINLINK_TEXT_INPUT_RESPONSE_HEADER_SIZE, resp->text, resp->text_len);
    }
    return needed;
}

finlink_result finlink_parse_text_input_response(const uint8_t *data, size_t size,
                                                   finlink_text_input_response *out) {
    if (size < FINLINK_TEXT_INPUT_RESPONSE_HEADER_SIZE) {
        return FINLINK_ERR_TOO_SHORT;
    }
    if (data[0] != FINLINK_MSG_TEXT_INPUT_RESPONSE) {
        return FINLINK_ERR_UNKNOWN_TYPE;
    }
    out->confirmed = data[1] != 0;
    const size_t text_len = finlink_read_u32le(data + 2);
    if (size < FINLINK_TEXT_INPUT_RESPONSE_HEADER_SIZE + text_len) {
        return FINLINK_ERR_TOO_SHORT;
    }
    out->text = (const char *)(data + FINLINK_TEXT_INPUT_RESPONSE_HEADER_SIZE);
    out->text_len = text_len;
    return FINLINK_OK;
}

size_t finlink_build_mic_enable_frame(const finlink_mic_enable *enable,
                                      uint8_t out_buf[FINLINK_MIC_ENABLE_FRAME_SIZE]) {
    out_buf[0] = FINLINK_MSG_MIC_ENABLE;
    out_buf[1] = enable->enabled ? 1 : 0;
    finlink_write_u32le(out_buf + 2, enable->sample_rate);
    return FINLINK_MIC_ENABLE_FRAME_SIZE;
}

finlink_result finlink_parse_mic_enable_frame(const uint8_t *data, size_t size,
                                               finlink_mic_enable *out) {
    if (size < FINLINK_MIC_ENABLE_FRAME_SIZE) {
        return FINLINK_ERR_TOO_SHORT;
    }
    if (data[0] != FINLINK_MSG_MIC_ENABLE) {
        return FINLINK_ERR_UNKNOWN_TYPE;
    }
    out->enabled = data[1] != 0;
    out->sample_rate = finlink_read_u32le(data + 2);
    return FINLINK_OK;
}

finlink_result finlink_parse_mic_audio_frame(const uint8_t *data, size_t size, finlink_audio_frame *out) {
    if (size < AUDIO_HEADER_SIZE) {
        return FINLINK_ERR_TOO_SHORT;
    }
    if (data[0] != FINLINK_MSG_MIC_AUDIO) {
        return FINLINK_ERR_UNKNOWN_TYPE;
    }

    out->sample_rate = finlink_read_u32le(data + 1);
    out->channels = data[5];
    out->samples = data + AUDIO_HEADER_SIZE;
    out->sample_count = (size - AUDIO_HEADER_SIZE) / sizeof(int16_t);
    return FINLINK_OK;
}

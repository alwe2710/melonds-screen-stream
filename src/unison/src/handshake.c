#include "unison/handshake.h"

#include <stdio.h>
#include <string.h>

#include "unison/json.h"

static unison_json_span whole_object(const uint8_t *data, size_t size) {
    unison_json_span span;
    span.found = 1;
    span.start = 0;
    span.end = size;
    (void)data;
    return span;
}

static void get_string_field(const char *text, size_t obj_start, size_t obj_end, const char *key,
                              char *out, size_t out_cap) {
    const unison_json_span span = unison_json_find_member(text, obj_start, obj_end, key);
    if (unison_json_get_string(text, span, out, out_cap) == (size_t)-1) {
        out[0] = '\0';
    }
}

static unison_handshake_video parse_video(const char *text, size_t obj_start, size_t obj_end) {
    unison_handshake_video video;
    memset(&video, 0, sizeof(video));
    const unison_json_span video_span = unison_json_find_member(text, obj_start, obj_end, "video");
    if (!video_span.found) {
        return video;
    }
    video.width = (uint32_t)unison_json_get_number(
        text, unison_json_find_member(text, video_span.start, video_span.end, "width"));
    video.height = (uint32_t)unison_json_get_number(
        text, unison_json_find_member(text, video_span.start, video_span.end, "height"));
    video.fps = unison_json_get_number(
        text, unison_json_find_member(text, video_span.start, video_span.end, "fps"));
    return video;
}

/* Shared by unison_parse_hello and unison_parse_session_ready: both carry
 * an optional top-level "audio": { sample_rate, channels } object. */
static int parse_audio(const char *text, size_t obj_start, size_t obj_end,
                        unison_handshake_audio *out) {
    const unison_json_span audio_span = unison_json_find_member(text, obj_start, obj_end, "audio");
    if (!audio_span.found) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    out->sample_rate = (uint32_t)unison_json_get_number(
        text, unison_json_find_member(text, audio_span.start, audio_span.end, "sample_rate"));
    out->channels = (uint8_t)unison_json_get_number(
        text, unison_json_find_member(text, audio_span.start, audio_span.end, "channels"));
    return 1;
}

unison_handshake_message_type unison_peek_handshake_message(const uint8_t *data, size_t size) {
    const char *text = (const char *)data;
    const unison_json_span obj = whole_object(data, size);
    char message[16];
    if (unison_json_get_string(text, unison_json_find_member(text, obj.start, obj.end, "message"),
                                 message, sizeof(message)) == (size_t)-1) {
        return UNISON_HS_MSG_UNKNOWN;
    }
    if (strcmp(message, "hello") == 0) {
        return UNISON_HS_MSG_HELLO;
    }
    if (strcmp(message, "session_ready") == 0) {
        return UNISON_HS_MSG_SESSION_READY;
    }
    if (strcmp(message, "handshake_error") == 0) {
        return UNISON_HS_MSG_HANDSHAKE_ERROR;
    }
    return UNISON_HS_MSG_UNKNOWN;
}

unison_handshake_result unison_parse_hello(const uint8_t *data, size_t size, unison_hello *out) {
    const char *text = (const char *)data;
    const unison_json_span obj = whole_object(data, size);

    if (unison_peek_handshake_message(data, size) != UNISON_HS_MSG_HELLO) {
        return UNISON_HANDSHAKE_ERR;
    }
    memset(out, 0, sizeof(*out));

    out->protocol_version = (int)unison_json_get_number(
        text, unison_json_find_member(text, obj.start, obj.end, "protocol_version"));
    get_string_field(text, obj.start, obj.end, "stream_type", out->stream_type,
                      sizeof(out->stream_type));
    get_string_field(text, obj.start, obj.end, "input_encoding", out->input_encoding,
                      sizeof(out->input_encoding));
    out->video = parse_video(text, obj.start, obj.end);
    out->has_audio = parse_audio(text, obj.start, obj.end, &out->audio);

    const unison_json_span slots_span = unison_json_find_member(text, obj.start, obj.end, "slots");
    if (slots_span.found) {
        unison_json_span elem = {0, 0, 0};
        while (out->slot_count < UNISON_MAX_SLOTS) {
            elem = unison_json_array_next(text, slots_span.start, slots_span.end, elem);
            if (!elem.found) {
                break;
            }
            unison_handshake_slot *slot = &out->slots[out->slot_count];
            slot->index = (int)unison_json_get_number(
                text, unison_json_find_member(text, elem.start, elem.end, "index"));
            get_string_field(text, elem.start, elem.end, "label", slot->label, sizeof(slot->label));
            slot->occupied = unison_json_get_bool(
                text, unison_json_find_member(text, elem.start, elem.end, "occupied"));
            out->slot_count++;
        }
    }

    return UNISON_HANDSHAKE_OK;
}

size_t unison_build_hello_ack(const unison_hello_ack_request *req, char *out_buf,
                                size_t out_capacity) {
    int n;
    if (req->wants_audio) {
        n = snprintf(out_buf, out_capacity,
                     "{\"message\":\"hello_ack\",\"protocol_version\":%d,\"requested_slot\":%d,"
                     "\"video_limits\":{\"max_width\":%u,\"max_height\":%u,\"max_fps\":%.4f,"
                     "\"max_bitrate_kbps\":null},"
                     "\"audio_limits\":{\"max_sample_rate\":%u,\"max_channels\":%u}}",
                     UNISON_PROTOCOL_VERSION, req->requested_slot, (unsigned)req->max_width,
                     (unsigned)req->max_height, req->max_fps, (unsigned)req->max_sample_rate,
                     (unsigned)req->max_channels);
    } else {
        n = snprintf(out_buf, out_capacity,
                     "{\"message\":\"hello_ack\",\"protocol_version\":%d,\"requested_slot\":%d,"
                     "\"video_limits\":{\"max_width\":%u,\"max_height\":%u,\"max_fps\":%.4f,"
                     "\"max_bitrate_kbps\":null}}",
                     UNISON_PROTOCOL_VERSION, req->requested_slot, (unsigned)req->max_width,
                     (unsigned)req->max_height, req->max_fps);
    }
    if (n < 0 || (size_t)n >= out_capacity) {
        return 0;
    }
    return (size_t)n;
}

unison_handshake_result unison_parse_session_ready(const uint8_t *data, size_t size,
                                                      unison_session_ready *out) {
    const char *text = (const char *)data;
    const unison_json_span obj = whole_object(data, size);

    if (unison_peek_handshake_message(data, size) != UNISON_HS_MSG_SESSION_READY) {
        return UNISON_HANDSHAKE_ERR;
    }
    memset(out, 0, sizeof(*out));

    out->slot =
        (int)unison_json_get_number(text, unison_json_find_member(text, obj.start, obj.end, "slot"));
    out->video = parse_video(text, obj.start, obj.end);
    out->has_audio = parse_audio(text, obj.start, obj.end, &out->audio);

    const unison_json_span redirect_span =
        unison_json_find_member(text, obj.start, obj.end, "redirect");
    if (redirect_span.found) {
        out->has_redirect = 1;
        get_string_field(text, redirect_span.start, redirect_span.end, "host", out->redirect_host,
                          sizeof(out->redirect_host));
        out->redirect_port = (int)unison_json_get_number(
            text, unison_json_find_member(text, redirect_span.start, redirect_span.end, "port"));
    }

    return UNISON_HANDSHAKE_OK;
}

unison_handshake_result unison_parse_handshake_error(const uint8_t *data, size_t size,
                                                        unison_handshake_error *out) {
    const char *text = (const char *)data;
    const unison_json_span obj = whole_object(data, size);

    if (unison_peek_handshake_message(data, size) != UNISON_HS_MSG_HANDSHAKE_ERROR) {
        return UNISON_HANDSHAKE_ERR;
    }
    memset(out, 0, sizeof(*out));
    get_string_field(text, obj.start, obj.end, "code", out->code, sizeof(out->code));
    get_string_field(text, obj.start, obj.end, "detail", out->detail, sizeof(out->detail));
    return UNISON_HANDSHAKE_OK;
}

int unison_stream_type_prefers_secondary_screen(const char *stream_type) {
    return strcmp(stream_type, "N3DS_BOTTOM_SCREEN") == 0 ||
           strcmp(stream_type, "NDS_BOTTOM_SCREEN") == 0 ||
           strcmp(stream_type, "WIIU_GAMEPAD") == 0;
}

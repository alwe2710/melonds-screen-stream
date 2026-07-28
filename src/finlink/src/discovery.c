#include "finlink/discovery.h"

#include <string.h>

#include "finlink/json.h"

static void get_string_field(const char *text, size_t obj_start, size_t obj_end, const char *key,
                              char *out, size_t out_cap) {
    const finlink_json_span span = finlink_json_find_member(text, obj_start, obj_end, key);
    if (finlink_json_get_string(text, span, out, out_cap) == (size_t)-1) {
        out[0] = '\0';
    }
}

int finlink_parse_beacon(const unsigned char *data, size_t size, finlink_beacon *out) {
    const char *text = (const char *)data;
    const finlink_json_span obj = {1, 0, size};

    /* `type` marker (docs/protocol.md) lets a stray UDP packet on the same
     * port -- something else's broadcast traffic, not a finlink beacon at
     * all -- be recognized and ignored cheaply before trusting any other
     * field. */
    char type[32];
    if (finlink_json_get_string(text, finlink_json_find_member(text, obj.start, obj.end, "type"),
                                 type, sizeof(type)) == (size_t)-1 ||
        strcmp(type, "finlink_beacon") != 0) {
        return 0;
    }

    memset(out, 0, sizeof(*out));
    out->protocol_version = (int)finlink_json_get_number(
        text, finlink_json_find_member(text, obj.start, obj.end, "protocol_version"));
    get_string_field(text, obj.start, obj.end, "emulator_identifier", out->emulator_identifier,
                      sizeof(out->emulator_identifier));
    get_string_field(text, obj.start, obj.end, "game_title", out->game_title,
                      sizeof(out->game_title));
    get_string_field(text, obj.start, obj.end, "stream_type", out->stream_type,
                      sizeof(out->stream_type));
    get_string_field(text, obj.start, obj.end, "host", out->host, sizeof(out->host));
    out->handshake_port = (int)finlink_json_get_number(
        text, finlink_json_find_member(text, obj.start, obj.end, "handshake_port"));

    return 1;
}

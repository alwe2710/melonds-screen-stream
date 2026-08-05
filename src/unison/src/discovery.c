#include "unison/discovery.h"

#include <string.h>

#include "unison/json.h"

static void get_string_field(const char *text, size_t obj_start, size_t obj_end, const char *key,
                              char *out, size_t out_cap) {
    const unison_json_span span = unison_json_find_member(text, obj_start, obj_end, key);
    if (unison_json_get_string(text, span, out, out_cap) == (size_t)-1) {
        out[0] = '\0';
    }
}

int unison_parse_beacon(const unsigned char *data, size_t size, unison_beacon *out) {
    const char *text = (const char *)data;
    const unison_json_span obj = {1, 0, size};

    /* `type` marker (docs/protocol.md) lets a stray UDP packet on the same
     * port -- something else's broadcast traffic, not a Unison beacon at
     * all -- be recognized and ignored cheaply before trusting any other
     * field. */
    char type[32];
    if (unison_json_get_string(text, unison_json_find_member(text, obj.start, obj.end, "type"),
                                 type, sizeof(type)) == (size_t)-1 ||
        strcmp(type, "unison_beacon") != 0) {
        return 0;
    }

    memset(out, 0, sizeof(*out));
    out->protocol_version = (int)unison_json_get_number(
        text, unison_json_find_member(text, obj.start, obj.end, "protocol_version"));
    get_string_field(text, obj.start, obj.end, "emulator_identifier", out->emulator_identifier,
                      sizeof(out->emulator_identifier));
    get_string_field(text, obj.start, obj.end, "game_title", out->game_title,
                      sizeof(out->game_title));
    get_string_field(text, obj.start, obj.end, "stream_type", out->stream_type,
                      sizeof(out->stream_type));
    get_string_field(text, obj.start, obj.end, "host", out->host, sizeof(out->host));
    out->handshake_port = (int)unison_json_get_number(
        text, unison_json_find_member(text, obj.start, obj.end, "handshake_port"));

    return 1;
}

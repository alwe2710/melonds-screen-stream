#ifndef FINLINK_JSON_H
#define FINLINK_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal JSON reading helpers, purpose-built for finlink/handshake.h and
 * finlink/discovery.h's fixed message shapes (docs/protocol.md) -- not a
 * general-purpose parser: no DOM, no dynamic allocation, no writer (building
 * the one JSON shape this client ever sends, hello_ack, is done directly
 * with snprintf-style formatting in handshake.c instead). Every function
 * here takes a `(text, start, end)` span rather than a NUL-terminated
 * string, so callers can point directly into an unmodified WebSocket frame
 * payload or UDP datagram without copying it first. */

typedef struct {
    int found;
    size_t start; /* value span: text[start..end), exclusive of surrounding whitespace */
    size_t end;
} finlink_json_span;

/* Scans a JSON object's members (text[obj_start] must be '{', after skipping
 * leading whitespace) for `key` at the top level -- does not recurse into
 * nested objects/arrays looking for it. .found=0 if the key isn't present,
 * obj_start/obj_end don't bound a well-formed object, or a member's own
 * value is malformed. */
finlink_json_span finlink_json_find_member(const char *text, size_t obj_start, size_t obj_end,
                                            const char *key);

/* Decodes a JSON string span (as returned by finlink_json_find_member, must
 * start with '"') into out_buf, unescaping \", \\, \/, \n, \r, \t, \b, \f and
 * \uXXXX (encoded as UTF-8; an unpaired/invalid \uXXXX becomes '?', not a
 * hard failure -- this is display text, e.g. a game title, not something to
 * reject a whole connection over). Returns the decoded length (excluding the
 * NUL terminator, which is always written on success), or (size_t)-1 if
 * out_capacity was too small or the span isn't a JSON string at all. */
size_t finlink_json_get_string(const char *text, finlink_json_span span, char *out_buf,
                                size_t out_capacity);

/* Parses a JSON number span into a double. 0.0 if the span isn't a number
 * (not found, or too long to fit the internal conversion buffer -- every
 * number this protocol sends, ports/rates/small fps values, is far short of
 * that). */
double finlink_json_get_number(const char *text, finlink_json_span span);

/* JSON `true`/`false` span -> 1/0. Defaults to 0 (not just for `false`, but
 * also not-found or malformed) -- callers treat this the same as an absent
 * optional flag. */
int finlink_json_get_bool(const char *text, finlink_json_span span);

/* Iterates a JSON array's elements (text[arr_start] must be '[', after
 * skipping leading whitespace). Pass a zeroed/`.found=0` span as `previous`
 * to get the first element; pass the previously-returned span to advance.
 * .found=0 once past the last element or if arr_start/arr_end don't bound a
 * well-formed array. */
finlink_json_span finlink_json_array_next(const char *text, size_t arr_start, size_t arr_end,
                                           finlink_json_span previous);

#ifdef __cplusplus
}
#endif

#endif /* FINLINK_JSON_H */

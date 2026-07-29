#include "finlink/json.h"

#include <stdlib.h>
#include <string.h>

static size_t skip_ws(const char *text, size_t pos, size_t end) {
    while (pos < end &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
        pos++;
    }
    return pos;
}

/* text[pos] must be '"'. Returns the position right after the closing quote,
 * or `end` if the string runs off the end of the given span unterminated. */
static size_t skip_string(const char *text, size_t pos, size_t end) {
    pos++; /* opening quote */
    while (pos < end) {
        if (text[pos] == '"') {
            return pos + 1;
        }
        if (text[pos] == '\\' && pos + 1 < end) {
            pos += 2;
            continue;
        }
        pos++;
    }
    return end;
}

/* Returns the position right after one JSON value starting at (whitespace-
 * skipped) `pos`: a string, object, array, or a run of "primitive" chars
 * (number/true/false/null) up to the next structural/whitespace character.
 * Malformed input (mismatched braces, unterminated string) just runs to
 * `end` rather than looping forever -- every caller treats that the same as
 * "value not found", not a crash. */
static size_t skip_value(const char *text, size_t pos, size_t end) {
    pos = skip_ws(text, pos, end);
    if (pos >= end) {
        return pos;
    }
    const char c = text[pos];
    if (c == '"') {
        return skip_string(text, pos, end);
    }
    if (c == '{' || c == '[') {
        const char close = (c == '{') ? '}' : ']';
        int depth = 1;
        pos++;
        while (pos < end && depth > 0) {
            const char d = text[pos];
            if (d == '"') {
                pos = skip_string(text, pos, end);
                continue;
            }
            if (d == c) {
                depth++;
            } else if (d == close) {
                depth--;
            }
            pos++;
        }
        return pos;
    }
    while (pos < end) {
        const char d = text[pos];
        if (d == ',' || d == '}' || d == ']' || d == ' ' || d == '\t' || d == '\n' || d == '\r') {
            break;
        }
        pos++;
    }
    return pos;
}

finlink_json_span finlink_json_find_member(const char *text, size_t obj_start, size_t obj_end,
                                            const char *key) {
    finlink_json_span result = {0, 0, 0};
    size_t pos = skip_ws(text, obj_start, obj_end);
    if (pos >= obj_end || text[pos] != '{') {
        return result;
    }
    pos++;
    const size_t key_len = strlen(key);

    for (;;) {
        pos = skip_ws(text, pos, obj_end);
        if (pos >= obj_end || text[pos] == '}') {
            break;
        }
        if (text[pos] != '"') {
            break; /* malformed: expected a member name */
        }
        const size_t key_start = pos + 1;
        const size_t key_str_end = skip_string(text, pos, obj_end); /* right after closing quote */
        const size_t key_str_len = (key_str_end >= key_start + 1) ? (key_str_end - 1 - key_start) : 0;
        pos = skip_ws(text, key_str_end, obj_end);
        if (pos >= obj_end || text[pos] != ':') {
            break; /* malformed: expected ':' */
        }
        pos = skip_ws(text, pos + 1, obj_end);

        const size_t value_start = pos;
        const size_t value_end = skip_value(text, pos, obj_end);

        if (key_str_len == key_len && memcmp(text + key_start, key, key_len) == 0) {
            result.found = 1;
            result.start = value_start;
            result.end = value_end;
            return result;
        }

        pos = skip_ws(text, value_end, obj_end);
        if (pos < obj_end && text[pos] == ',') {
            pos++;
            continue;
        }
        break; /* '}' (done, not found) or malformed -- either way, stop */
    }
    return result;
}

static void append_byte(char *out_buf, size_t out_capacity, size_t *out_len, char byte) {
    if (*out_len + 1 <= out_capacity) {
        out_buf[*out_len] = byte;
    }
    (*out_len)++;
}

static unsigned int hex_digit(char h) {
    if (h >= '0' && h <= '9') {
        return (unsigned int)(h - '0');
    }
    if (h >= 'a' && h <= 'f') {
        return (unsigned int)(h - 'a' + 10);
    }
    if (h >= 'A' && h <= 'F') {
        return (unsigned int)(h - 'A' + 10);
    }
    return 0xFFu; /* sentinel: not a hex digit */
}

size_t finlink_json_get_string(const char *text, finlink_json_span span, char *out_buf,
                                size_t out_capacity) {
    if (!span.found || span.end <= span.start || text[span.start] != '"') {
        return (size_t)-1;
    }
    size_t pos = span.start + 1;
    const size_t end = span.end;
    size_t out_len = 0;

    while (pos < end) {
        const char c = text[pos];
        if (c == '"') {
            break;
        }
        if (c == '\\' && pos + 1 < end) {
            const char e = text[pos + 1];
            if (e == 'u') {
                unsigned int cp = 0;
                int hex_ok = (pos + 6 <= end);
                if (hex_ok) {
                    for (int i = 0; i < 4; i++) {
                        const unsigned int digit = hex_digit(text[pos + 2 + (size_t)i]);
                        if (digit == 0xFFu) {
                            hex_ok = 0;
                            break;
                        }
                        cp = (cp << 4) | digit;
                    }
                }
                if (!hex_ok) {
                    /* Malformed \u (bad hex digits, or not enough room left
                     * in the string) -- give up on the rest of the string,
                     * same as any other unrecoverable parse error here. */
                    pos = end;
                    append_byte(out_buf, out_capacity, &out_len, '?');
                    continue;
                }
                pos += 6;

                /* High surrogate (U+D800..U+DBFF): per RFC 8259, only valid
                 * when immediately followed by a low surrogate
                 * (U+DC00..U+DFFF) \u escape -- together they encode one
                 * codepoint above the BMP (U+10000..U+10FFFF). Combine the
                 * pair and consume both escapes. An unpaired surrogate
                 * (either half on its own, or a high surrogate not actually
                 * followed by a valid low one) isn't a valid Unicode scalar
                 * value by itself -- unlike a bad hex digit, this doesn't
                 * abandon the rest of the string, just substitutes '?' for
                 * this one escape and keeps going. */
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    unsigned int low = 0;
                    int pair_ok = (pos + 6 <= end) && text[pos] == '\\' && text[pos + 1] == 'u';
                    if (pair_ok) {
                        for (int i = 0; i < 4; i++) {
                            const unsigned int digit = hex_digit(text[pos + 2 + (size_t)i]);
                            if (digit == 0xFFu) {
                                pair_ok = 0;
                                break;
                            }
                            low = (low << 4) | digit;
                        }
                    }
                    if (pair_ok && low >= 0xDC00u && low <= 0xDFFFu) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                        pos += 6;
                    } else {
                        cp = (unsigned int)'?';
                    }
                } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                    cp = (unsigned int)'?'; /* unpaired low surrogate */
                }

                if (cp < 0x80u) {
                    append_byte(out_buf, out_capacity, &out_len, (char)cp);
                } else if (cp < 0x800u) {
                    append_byte(out_buf, out_capacity, &out_len, (char)(0xC0u | (cp >> 6)));
                    append_byte(out_buf, out_capacity, &out_len, (char)(0x80u | (cp & 0x3Fu)));
                } else if (cp < 0x10000u) {
                    append_byte(out_buf, out_capacity, &out_len, (char)(0xE0u | (cp >> 12)));
                    append_byte(out_buf, out_capacity, &out_len, (char)(0x80u | ((cp >> 6) & 0x3Fu)));
                    append_byte(out_buf, out_capacity, &out_len, (char)(0x80u | (cp & 0x3Fu)));
                } else {
                    append_byte(out_buf, out_capacity, &out_len, (char)(0xF0u | (cp >> 18)));
                    append_byte(out_buf, out_capacity, &out_len, (char)(0x80u | ((cp >> 12) & 0x3Fu)));
                    append_byte(out_buf, out_capacity, &out_len, (char)(0x80u | ((cp >> 6) & 0x3Fu)));
                    append_byte(out_buf, out_capacity, &out_len, (char)(0x80u | (cp & 0x3Fu)));
                }
                continue;
            }

            char decoded;
            switch (e) {
                case '"': decoded = '"'; break;
                case '\\': decoded = '\\'; break;
                case '/': decoded = '/'; break;
                case 'n': decoded = '\n'; break;
                case 'r': decoded = '\r'; break;
                case 't': decoded = '\t'; break;
                case 'b': decoded = '\b'; break;
                case 'f': decoded = '\f'; break;
                default: decoded = e; break;
            }
            append_byte(out_buf, out_capacity, &out_len, decoded);
            pos += 2;
            continue;
        }
        append_byte(out_buf, out_capacity, &out_len, c);
        pos++;
    }

    if (out_len + 1 > out_capacity) {
        return (size_t)-1;
    }
    out_buf[out_len] = '\0';
    return out_len;
}

double finlink_json_get_number(const char *text, finlink_json_span span) {
    if (!span.found || span.end <= span.start) {
        return 0.0;
    }
    char buf[64];
    const size_t len = span.end - span.start;
    if (len >= sizeof(buf)) {
        return 0.0;
    }
    memcpy(buf, text + span.start, len);
    buf[len] = '\0';
    return strtod(buf, NULL);
}

int finlink_json_get_bool(const char *text, finlink_json_span span) {
    if (!span.found) {
        return 0;
    }
    const size_t len = span.end - span.start;
    return len == 4 && memcmp(text + span.start, "true", 4) == 0;
}

finlink_json_span finlink_json_array_next(const char *text, size_t arr_start, size_t arr_end,
                                           finlink_json_span previous) {
    finlink_json_span result = {0, 0, 0};
    size_t pos = skip_ws(text, arr_start, arr_end);
    if (pos >= arr_end || text[pos] != '[') {
        return result;
    }
    pos++;

    if (previous.found) {
        pos = skip_ws(text, previous.end, arr_end);
        if (pos < arr_end && text[pos] == ',') {
            pos++;
        }
    }
    pos = skip_ws(text, pos, arr_end);
    if (pos >= arr_end || text[pos] == ']') {
        return result;
    }

    result.found = 1;
    result.start = pos;
    result.end = skip_value(text, pos, arr_end);
    return result;
}

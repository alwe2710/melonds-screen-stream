#include "unison/websocket.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "teeny-sha1.h"

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" /* RFC6455 magic, 36 chars */

static const char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Writes exactly 4*ceil(len/3) chars + a NUL terminator. out must have room
 * for that many bytes; callers here only ever pass fixed 16- or 20-byte
 * inputs (Sec-WebSocket-Key / SHA1 digest), never used as a general-purpose
 * encoder. */
static void base64_encode(const uint8_t *data, size_t len, char *out) {
    size_t i = 0;
    size_t o = 0;

    for (; i + 3 <= len; i += 3) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[o++] = kBase64Table[(n >> 18) & 0x3F];
        out[o++] = kBase64Table[(n >> 12) & 0x3F];
        out[o++] = kBase64Table[(n >> 6) & 0x3F];
        out[o++] = kBase64Table[n & 0x3F];
    }

    const size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)data[i] << 16;
        out[o++] = kBase64Table[(n >> 18) & 0x3F];
        out[o++] = kBase64Table[(n >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[o++] = kBase64Table[(n >> 18) & 0x3F];
        out[o++] = kBase64Table[(n >> 12) & 0x3F];
        out[o++] = kBase64Table[(n >> 6) & 0x3F];
        out[o++] = '=';
    }

    out[o] = '\0';
}

static const uint8_t *find_bytes(const uint8_t *hay, size_t hay_len, const char *needle) {
    const size_t needle_len = strlen(needle);
    if (needle_len == 0 || hay_len < needle_len) {
        return NULL;
    }
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

static int ci_starts_with(const uint8_t *data, size_t size, const char *prefix) {
    const size_t n = strlen(prefix);
    if (size < n) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower(data[i]) != tolower((unsigned char)prefix[i])) {
            return 0;
        }
    }
    return 1;
}

void unison_ws_generate_key(const uint8_t random_bytes[16], char key_out[UNISON_WS_KEY_BUF_LEN]) {
    base64_encode(random_bytes, 16, key_out);
}

size_t unison_ws_build_handshake_request(const char *host, const char *path,
                                           const char key[UNISON_WS_KEY_LEN], char *out_buf,
                                           size_t out_capacity) {
    const int n = snprintf(out_buf, out_capacity,
                            "GET %s HTTP/1.1\r\n"
                            "Host: %s\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Key: %.*s\r\n"
                            "Sec-WebSocket-Version: 13\r\n"
                            "\r\n",
                            path, host, (int)UNISON_WS_KEY_LEN, key);
    if (n < 0 || (size_t)n >= out_capacity) {
        return 0;
    }
    return (size_t)n;
}

unison_ws_handshake_status unison_ws_parse_handshake_response(const uint8_t *data, size_t size,
                                                                  const char key[UNISON_WS_KEY_LEN],
                                                                  size_t *header_len) {
    const uint8_t *terminator = find_bytes(data, size, "\r\n\r\n");
    if (!terminator) {
        return UNISON_WS_HANDSHAKE_INCOMPLETE;
    }
    const size_t headers_len = (size_t)(terminator - data) + 4;
    *header_len = headers_len;

    const uint8_t *first_line_end = find_bytes(data, headers_len, "\r\n");
    if (!first_line_end) {
        return UNISON_WS_HANDSHAKE_ERR;
    }
    if (!find_bytes(data, (size_t)(first_line_end - data), " 101 ")) {
        return UNISON_WS_HANDSHAKE_ERR;
    }

    /* terminator points at the first byte of the blank line's "\r\n\r\n"
     * match, which is itself the *last header line's own* line-terminating
     * "\r\n" -- so the header block for line-splitting purposes extends two
     * bytes further, through that "\r\n", with the remaining "\r\n" being
     * the blank line itself. */
    const uint8_t *headers_end = terminator + 2;
    const uint8_t *line = first_line_end + 2;
    const uint8_t *accept_value = NULL;
    size_t accept_value_len = 0;

    while (line < headers_end) {
        const uint8_t *line_end = find_bytes(line, (size_t)(headers_end - line), "\r\n");
        if (!line_end) {
            break;
        }
        const size_t line_len = (size_t)(line_end - line);
        if (ci_starts_with(line, line_len, "sec-websocket-accept:")) {
            const size_t prefix_len = strlen("sec-websocket-accept:");
            const uint8_t *v = line + prefix_len;
            size_t v_len = line_len - prefix_len;
            while (v_len > 0 && *v == ' ') {
                v++;
                v_len--;
            }
            accept_value = v;
            accept_value_len = v_len;
            break;
        }
        line = line_end + 2;
    }

    if (!accept_value || accept_value_len != UNISON_WS_ACCEPT_LEN) {
        return UNISON_WS_HANDSHAKE_ERR;
    }

    uint8_t concat[UNISON_WS_KEY_LEN + 36];
    memcpy(concat, key, UNISON_WS_KEY_LEN);
    memcpy(concat + UNISON_WS_KEY_LEN, WS_GUID, 36);

    uint8_t digest[20];
    sha1digest(digest, NULL, concat, sizeof(concat));

    char expected[UNISON_WS_ACCEPT_LEN + 1];
    base64_encode(digest, sizeof(digest), expected);

    if (memcmp(accept_value, expected, UNISON_WS_ACCEPT_LEN) != 0) {
        return UNISON_WS_HANDSHAKE_ERR;
    }

    return UNISON_WS_HANDSHAKE_OK;
}

unison_ws_frame_status unison_ws_parse_frame(uint8_t *data, size_t size, unison_ws_frame *out) {
    if (size < 2) {
        return UNISON_WS_FRAME_INCOMPLETE;
    }

    const uint8_t b0 = data[0];
    const uint8_t b1 = data[1];
    const int fin = (b0 & 0x80) != 0;
    const uint8_t opcode = b0 & 0x0F;
    const int masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7F;
    size_t pos = 2;

    if (len == 126) {
        if (size < 4) {
            return UNISON_WS_FRAME_INCOMPLETE;
        }
        len = ((uint64_t)data[2] << 8) | data[3];
        pos = 4;
    } else if (len == 127) {
        if (size < 10) {
            return UNISON_WS_FRAME_INCOMPLETE;
        }
        len = 0;
        for (int i = 0; i < 8; i++) {
            len = (len << 8) | data[2 + i];
        }
        pos = 10;
    }

    if (len > UNISON_WS_MAX_FRAME_PAYLOAD) {
        return UNISON_WS_FRAME_ERR;
    }

    uint8_t mask_key[4] = {0};
    if (masked) {
        if (size < pos + 4) {
            return UNISON_WS_FRAME_INCOMPLETE;
        }
        memcpy(mask_key, data + pos, 4);
        pos += 4;
    }

    if (size < pos + (size_t)len) {
        return UNISON_WS_FRAME_INCOMPLETE;
    }

    if (!fin) {
        /* Fragmented frames are not supported on either side of this
         * protocol; treat one as a protocol error rather than silently
         * misinterpreting a partial message as complete. */
        return UNISON_WS_FRAME_ERR;
    }
    if (opcode != UNISON_WS_OPCODE_TEXT && opcode != UNISON_WS_OPCODE_BINARY &&
        opcode != UNISON_WS_OPCODE_CLOSE) {
        return UNISON_WS_FRAME_ERR;
    }

    if (masked) {
        for (uint64_t i = 0; i < len; i++) {
            data[pos + i] ^= mask_key[i % 4];
        }
    }

    out->opcode = (unison_ws_opcode)opcode;
    out->payload = data + pos;
    out->payload_size = (size_t)len;
    out->frame_size = pos + (size_t)len;
    return UNISON_WS_FRAME_OK;
}

size_t unison_ws_build_frame_max_size(size_t payload_size) {
    return 10 /* max header incl. length field */ + 4 /* mask key */ + payload_size;
}

size_t unison_ws_build_frame(unison_ws_opcode opcode, const uint8_t *payload, size_t payload_size,
                               const uint8_t mask_key[4], uint8_t *out_buf, size_t out_capacity) {
    uint8_t header[14];
    size_t header_len;

    header[0] = 0x80 | (uint8_t)opcode; /* FIN=1 */

    if (payload_size < 126) {
        header[1] = 0x80 | (uint8_t)payload_size; /* MASK=1 */
        header_len = 2;
    } else if (payload_size <= 0xFFFF) {
        header[1] = 0x80 | 126;
        header[2] = (uint8_t)((payload_size >> 8) & 0xFF);
        header[3] = (uint8_t)(payload_size & 0xFF);
        header_len = 4;
    } else {
        header[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (uint8_t)(((uint64_t)payload_size >> (56 - 8 * i)) & 0xFF);
        }
        header_len = 10;
    }

    memcpy(header + header_len, mask_key, 4);
    header_len += 4;

    const size_t total = header_len + payload_size;
    if (total > out_capacity) {
        return 0;
    }

    memcpy(out_buf, header, header_len);
    for (size_t i = 0; i < payload_size; i++) {
        out_buf[header_len + i] = payload[i] ^ mask_key[i % 4];
    }
    return total;
}

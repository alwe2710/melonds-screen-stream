#ifndef FINLINK_ENDIAN_H
#define FINLINK_ENDIAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Portable little-endian reads/writes. The wire protocol (docs/protocol.md) is
 * fixed little-endian regardless of host byte order. */

static inline uint16_t finlink_read_u16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t finlink_read_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int16_t finlink_read_s16le(const uint8_t *p) {
    return (int16_t)finlink_read_u16le(p);
}

static inline void finlink_write_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void finlink_write_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

#ifdef __cplusplus
}
#endif

#endif /* FINLINK_ENDIAN_H */

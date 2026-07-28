#ifndef TEENY_SHA1_H
#define TEENY_SHA1_H

/* Declaration for teeny-sha1.c (https://github.com/CTrabant/teeny-sha1, MIT).
 * Upstream ships the .c only and documents copying this declaration in by
 * hand; kept as a separate header here instead so it can be #included
 * normally. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* digest must be >= 20 bytes if non-NULL; hexdigest must be >= 41 bytes if
 * non-NULL. At least one of the two must be supplied. Returns 0 on success. */
int sha1digest(uint8_t *digest, char *hexdigest, const uint8_t *data, size_t databytes);

#ifdef __cplusplus
}
#endif

#endif /* TEENY_SHA1_H */

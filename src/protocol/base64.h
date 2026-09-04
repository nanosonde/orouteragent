/* orouteragent - base64 encode/decode */
#ifndef ORA_BASE64_H
#define ORA_BASE64_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Standard base64 (RFC 4648, with padding). */
size_t ora_base64_encoded_len(size_t n);
/* Writes exactly ora_base64_encoded_len(n)+1 bytes (incl. NUL). Returns
 * bytes written excluding NUL, or 0 on bad args. */
size_t ora_base64_encode(char *out, size_t outsz, const void *src, size_t n);

/* Decode; returns number of bytes, or -1 on malformed input / too small
 * out buffer. */
ssize_t ora_base64_decode(uint8_t *out, size_t outsz, const char *src, size_t n);

#endif
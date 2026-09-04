/* orouteragent - base64 implementation (RFC 4648) */
#include "base64.h"

#include <stdint.h>

static const char enc_tab[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t ora_base64_encoded_len(size_t n)
{
    if (n > (SIZE_MAX - 2) / 4 * 3)
        return 0; /* would overflow the 4/3 expansion */
    return ((n + 2) / 3) * 4;
}

size_t ora_base64_encode(char *out, size_t outsz, const void *src, size_t n)
{
    const uint8_t *p = src;
    size_t olen = ora_base64_encoded_len(n);
    size_t i = 0, o = 0;

    if (!out || !src)
        return 0;
    if (n > 0 && olen == 0) /* length overflow */
        return 0;
    if (outsz < olen + 1)
        return 0;

    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        out[o++] = enc_tab[(v >> 18) & 63];
        out[o++] = enc_tab[(v >> 12) & 63];
        out[o++] = enc_tab[(v >> 6) & 63];
        out[o++] = enc_tab[v & 63];
        i += 3;
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)p[i] << 16;
        out[o++] = enc_tab[(v >> 18) & 63];
        out[o++] = enc_tab[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8);
        out[o++] = enc_tab[(v >> 18) & 63];
        out[o++] = enc_tab[(v >> 12) & 63];
        out[o++] = enc_tab[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

ssize_t ora_base64_decode(uint8_t *out, size_t outsz, const char *src, size_t n)
{
    size_t o = 0;
    uint32_t acc = 0;
    int nbits = 0;
    int seen_pad = 0;
    size_t i;

    if (!out || !src)
        return -1;
    /* strict: length must be a multiple of 4 */
    if (n % 4)
        return -1;

    for (i = 0; i < n; i++) {
        char c = src[i];
        if (c == '=') {
            seen_pad++;
            /* only trailing '=' allowed */
            if (i + 1 < n && src[i + 1] != '=')
                return -1;
            continue;
        }
        if (seen_pad)
            return -1; /* data after padding */
        int v = b64val(c);
        if (v < 0)
            return -1;
        acc = (acc << 6) | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            if (o >= outsz)
                return -1;
            out[o++] = (uint8_t)(acc >> nbits);
        }
    }
    /* leftover bits must be zero */
    if (nbits >= 6)
        return -1; /* non-canonical: 1 or 5 leftover bits etc. */
    return (ssize_t)o;
}
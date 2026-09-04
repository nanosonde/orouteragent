/* orouteragent - bounds-checked buffer implementation */
#include "buf.h"

#include <stdarg.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ora_buf_init(struct ora_buf *b)
{
    b->data = NULL;
    b->len = b->cap = 0;
    b->oom = false;
}

void ora_buf_free(struct ora_buf *b)
{
    free(b->data);
    ora_buf_init(b);
}

bool ora_buf_reserve(struct ora_buf *b, size_t extra)
{
    if (b->oom)
        return false;
    size_t need = b->len + extra;
    if (need < b->len) { /* overflow */
        b->oom = true;
        return false;
    }
    if (need <= b->cap)
        return true;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < need) {
        size_t next = ncap * 2;
        if (next < ncap) { /* overflow */
            b->oom = true;
            return false;
        }
        ncap = next;
    }
    uint8_t *nd = realloc(b->data, ncap);
    if (!nd) {
        b->oom = true;
        return false;
    }
    b->data = nd;
    b->cap = ncap;
    return true;
}

void ora_buf_reset(struct ora_buf *b)
{
    b->len = 0;
    b->oom = false;
}

bool ora_buf_append(struct ora_buf *b, const void *src, size_t n)
{
    if (b->oom)
        return false;
    if (n == 0)
        return true;
    if (!src)
        return false;
    if (!ora_buf_reserve(b, n))
        return false;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return true;
}

bool ora_buf_append_str(struct ora_buf *b, const char *s)
{
    return s ? ora_buf_append(b, s, strlen(s)) : false;
}

bool ora_buf_append_byte(struct ora_buf *b, uint8_t c)
{
    if (!ora_buf_reserve(b, 1))
        return false;
    b->data[b->len++] = c;
    return true;
}

bool ora_buf_printf(struct ora_buf *b, const char *fmt, ...)
{
    va_list ap;
    size_t avail;
    size_t need;
    int n;

    if (b->oom)
        return false;

    if (!ora_buf_reserve(b, 64)) /* scratch for small prints */
        return false;
    for (;;) {
        avail = b->cap - b->len;
        if (avail > INT_MAX)
            avail = INT_MAX;
        va_start(ap, fmt);
        n = vsnprintf((char *)b->data + b->len, avail, fmt, ap);
        va_end(ap);
        if (n < 0) {
            b->oom = true;
            return false;
        }
        need = (size_t)n;
        if (need < avail) { /* fits incl. NUL */
            b->len += need;
            return true;
        }
        if (!ora_buf_reserve(b, need + 1))
            return false;
    }
}

bool ora_buf_be32(struct ora_buf *b, uint32_t v)
{
    uint8_t t[4] = {
        (uint8_t)(v >> 24), (uint8_t)(v >> 16),
        (uint8_t)(v >> 8), (uint8_t)v
    };
    return ora_buf_append(b, t, sizeof(t));
}

bool ora_buf_be16(struct ora_buf *b, uint16_t v)
{
    uint8_t t[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    return ora_buf_append(b, t, sizeof(t));
}

bool ora_buf_pad(struct ora_buf *b, size_t n)
{
    static const uint8_t zeros[16] = {0};
    while (n > 0) {
        size_t chunk = n < sizeof(zeros) ? n : sizeof(zeros);
        if (!ora_buf_append(b, zeros, chunk))
            return false;
        n -= chunk;
    }
    return true;
}
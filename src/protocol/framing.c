/* orouteragent - framing implementation */
#include "framing.h"

#include <string.h>

bool ora_frame_encode(struct ora_buf *out, const void *payload, size_t len)
{
    if (!out || !payload)
        return false;
    if (len > 0xFFFFFFFFull)
        return false;
    if (!ora_buf_be32(out, (uint32_t)len))
        return false;
    return ora_buf_append(out, payload, len);
}

void ora_frame_reader_init(struct ora_frame_reader *r, size_t max_frame)
{
    ora_buf_init(&r->in);
    r->declared = 0;
    r->have_declared = false;
    r->max_frame = max_frame ? max_frame : ORA_MAX_FRAME_PAYLOAD;
}

void ora_frame_reader_free(struct ora_frame_reader *r)
{
    ora_buf_free(&r->in);
}

bool ora_frame_reader_feed(struct ora_frame_reader *r, const void *data, size_t n)
{
    return ora_buf_append(&r->in, data, n);
}

int ora_frame_reader_next(struct ora_frame_reader *r,
                          const uint8_t **payload, size_t *len)
{
    while (!r->have_declared) {
        if (r->in.len < 4)
            return 0;
        r->declared = ((uint32_t)r->in.data[0] << 24) |
                      ((uint32_t)r->in.data[1] << 16) |
                      ((uint32_t)r->in.data[2] << 8) |
                       (uint32_t)r->in.data[3];
        if (r->declared > r->max_frame) {
            /* protocol violation: refuse and force reconnect */
            return -1;
        }
        r->have_declared = true;
        memmove(r->in.data, r->in.data + 4, r->in.len - 4);
        r->in.len -= 4;
    }
    if (r->in.len < r->declared)
        return 0;
    *payload = r->in.data;
    *len = r->declared;
    return 1;
}

/* After a successful ora_frame_reader_next(), drop the returned frame. */
void ora_frame_reader_consume(struct ora_frame_reader *r)
{
    if (!r->have_declared)
        return;
    memmove(r->in.data, r->in.data + r->declared, r->in.len - r->declared);
    r->in.len -= r->declared;
    r->have_declared = false;
    r->declared = 0;
}

ssize_t ora_frame_decode_datagram(const void *data, size_t n,
                                  const uint8_t **payload, size_t *len)
{
    const uint8_t *p = data;
    uint32_t d;

    if (!data || n < 4)
        return -1;
    /* cast before shifting: uint8_t promotes to int, so an unguarded
     * <<24 of a byte >= 0x80 overflows a signed int */
    d = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    if (d > ORA_MAX_FRAME_PAYLOAD)
        return -1;
    if (n - 4 < d)
        return -1;
    *payload = p + 4;
    *len = d;
    return (ssize_t)d;
}
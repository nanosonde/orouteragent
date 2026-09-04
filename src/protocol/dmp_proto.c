/* orouteragent - DMP protobuf codec implementation */
#include "dmp_proto.h"

#include <string.h>

/* ---- primitives ---- */

static bool put_tag(struct ora_buf *b, uint32_t field, uint32_t wire)
{
    uint64_t tag = ((uint64_t)field << 3) | wire;

    for (;;) {
        uint8_t byte = (uint8_t)(tag & 0x7F);

        tag >>= 7;
        if (tag)
            byte |= 0x80;
        if (!ora_buf_append_byte(b, byte))
            return false;
        if (!tag)
            return true;
    }
}

static bool put_raw_varint(struct ora_buf *b, uint64_t value)
{
    for (;;) {
        uint8_t byte = (uint8_t)(value & 0x7F);

        value >>= 7;
        if (value)
            byte |= 0x80;
        if (!ora_buf_append_byte(b, byte))
            return false;
        if (!value)
            return true;
    }
}

bool ora_pb_put_varint(struct ora_buf *b, uint32_t field, uint64_t value)
{
    return put_tag(b, field, 0) && put_raw_varint(b, value);
}

bool ora_pb_put_bytes(struct ora_buf *b, uint32_t field,
                      const void *data, size_t len)
{
    if (!put_tag(b, field, 2) || !put_raw_varint(b, len))
        return false;
    return len ? ora_buf_append(b, data, len) : true;
}

bool ora_pb_put_fixed64(struct ora_buf *b, uint32_t field, uint64_t value)
{
    uint8_t t[8];
    int i;

    if (!put_tag(b, field, 1))
        return false;
    for (i = 0; i < 8; i++)
        t[i] = (uint8_t)(value >> (8 * i)); /* protobuf fixed64 is LE */
    return ora_buf_append(b, t, sizeof(t));
}

bool ora_pb_get_varint(const uint8_t *buf, size_t len, size_t *off, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;

    while (*off < len) {
        uint8_t byte = buf[(*off)++];

        v |= (uint64_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) {
            *out = v;
            return true;
        }
        shift += 7;
        if (shift > 63)
            return false;
    }
    return false;
}

/* Skip a field of @wire type. */
static bool skip_field(const uint8_t *buf, size_t len, size_t *off, uint32_t wire)
{
    uint64_t v;

    switch (wire) {
    case 0:
        return ora_pb_get_varint(buf, len, off, &v);
    case 1:
        if (len - *off < 8)
            return false;
        *off += 8;
        return true;
    case 2:
        if (!ora_pb_get_varint(buf, len, off, &v))
            return false;
        if (v > len - *off)
            return false;
        *off += (size_t)v;
        return true;
    case 5:
        if (len - *off < 4)
            return false;
        *off += 4;
        return true;
    default:
        return false;
    }
}

/* ---- header ---- */

static bool encode_header(struct ora_buf *b, const struct ora_dmp_header *h)
{
    if (!ora_pb_put_bytes(b, 1, h->mac, sizeof(h->mac)))
        return false;
    if (h->token[0] && !ora_pb_put_bytes(b, 2, h->token, strlen(h->token)))
        return false;
    if (h->path[0] && !ora_pb_put_bytes(b, 3, h->path, strlen(h->path)))
        return false;
    if (!ora_pb_put_varint(b, 4, (uint64_t)h->version))
        return false;
    if (!ora_pb_put_varint(b, 5, (uint64_t)h->msg_type))
        return false;
    if (!ora_pb_put_varint(b, 6, (uint64_t)h->seq))
        return false;
    if (!ora_pb_put_varint(b, 7, (uint64_t)h->dev_type))
        return false;
    if (!ora_pb_put_varint(b, 8, (uint64_t)h->error_code))
        return false;
    if (!ora_pb_put_varint(b, 9, (uint64_t)h->need_reply))
        return false;
    if (!ora_pb_put_fixed64(b, 10, h->epoch_ms))
        return false;
    return ora_pb_put_varint(b, 11, (uint64_t)h->content_type);
}

static bool decode_header(const uint8_t *buf, size_t len, struct ora_dmp_header *h)
{
    size_t off = 0;

    memset(h, 0, sizeof(*h));
    while (off < len) {
        uint64_t tag, v;
        uint32_t field, wire;

        if (!ora_pb_get_varint(buf, len, &off, &tag))
            return false;
        field = (uint32_t)(tag >> 3);
        wire = (uint32_t)(tag & 7);

        if (wire == 2) {
            uint64_t n;
            size_t start;

            if (!ora_pb_get_varint(buf, len, &off, &n))
                return false;
            if (n > len - off)
                return false;
            start = off;
            off += (size_t)n;
            switch (field) {
            case 1:
                if (n == sizeof(h->mac))
                    memcpy(h->mac, buf + start, sizeof(h->mac));
                break;
            case 2:
                if (n < sizeof(h->token)) {
                    memcpy(h->token, buf + start, (size_t)n);
                    h->token[n] = '\0';
                }
                break;
            case 3:
                if (n < sizeof(h->path)) {
                    memcpy(h->path, buf + start, (size_t)n);
                    h->path[n] = '\0';
                }
                break;
            default:
                break;
            }
            continue;
        }
        if (wire == 1) {
            uint64_t fv = 0;
            int i;

            if (len - off < 8)
                return false;
            for (i = 0; i < 8; i++)
                fv |= (uint64_t)buf[off + i] << (8 * i);
            off += 8;
            if (field == 10)
                h->epoch_ms = fv;
            continue;
        }
        if (wire != 0) {
            if (!skip_field(buf, len, &off, wire))
                return false;
            continue;
        }
        if (!ora_pb_get_varint(buf, len, &off, &v))
            return false;
        switch (field) {
        case 4: h->version = (int64_t)v; break;
        case 5: h->msg_type = (int64_t)v; break;
        case 6: h->seq = (int64_t)v; break;
        case 7: h->dev_type = (int64_t)v; break;
        case 8: h->error_code = (int64_t)v; break;
        case 9: h->need_reply = (int64_t)v; break;
        case 11: h->content_type = (int64_t)v; break;
        default: break;
        }
    }
    return true;
}

/* ---- message ---- */

bool ora_dmp_encode(struct ora_buf *out, const struct ora_dmp_header *hdr,
                    const void *data, size_t data_len)
{
    struct ora_buf h;
    bool ok = false;

    ora_buf_init(&h);
    if (!encode_header(&h, hdr))
        goto out;
    if (!ora_pb_put_bytes(out, 1, h.data, h.len))
        goto out;
    if (data && data_len && !ora_pb_put_bytes(out, 2, data, data_len))
        goto out;
    ok = true;
out:
    ora_buf_free(&h);
    return ok;
}

bool ora_dmp_decode(const uint8_t *payload, size_t len,
                    struct ora_dmp_header *hdr,
                    const uint8_t **data, size_t *data_len)
{
    size_t off = 0;
    bool have_header = false;

    if (data)
        *data = NULL;
    if (data_len)
        *data_len = 0;
    if (!payload)
        return false;

    while (off < len) {
        uint64_t tag, n;
        uint32_t field, wire;
        size_t start;

        if (!ora_pb_get_varint(payload, len, &off, &tag))
            return false;
        field = (uint32_t)(tag >> 3);
        wire = (uint32_t)(tag & 7);
        if (wire != 2) {
            if (!skip_field(payload, len, &off, wire))
                return false;
            continue;
        }
        if (!ora_pb_get_varint(payload, len, &off, &n))
            return false;
        if (n > len - off)
            return false;
        start = off;
        off += (size_t)n;

        if (field == 1) {
            if (!decode_header(payload + start, (size_t)n, hdr))
                return false;
            have_header = true;
        } else if (field == 2) {
            if (data)
                *data = payload + start;
            if (data_len)
                *data_len = (size_t)n;
        }
    }
    return have_header;
}
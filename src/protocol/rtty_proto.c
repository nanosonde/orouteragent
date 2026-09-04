/* orouteragent - RTTY wire protocol implementation */
#include "rtty_proto.h"

#include <string.h>

bool ora_rtty_is_v1(int msg_type)
{
    switch (msg_type) {
    case ORA_RTTY_REGISTER:
    case ORA_RTTY_LOGIN:
    case ORA_RTTY_LOGOUT:
    case ORA_RTTY_TERMDATA:
    case ORA_RTTY_WINSIZE:
    case ORA_RTTY_CMD:
    case ORA_RTTY_HEARTBEAT:
    case ORA_RTTY_ACK:
    case ORA_RTTY_DISCONNECT_EXCEPTION:
    case ORA_RTTY_DISCONNECT_NORMALLY:
        return true;
    default:
        return false;
    }
}

bool ora_rtty_pack(struct ora_buf *out, int msg_type,
                   const void *payload, size_t len)
{
    if (!out)
        return false;
    if (!ora_buf_append_byte(out, (uint8_t)msg_type))
        return false;
    if (ora_rtty_is_v1(msg_type)) {
        if (len > 0xFFFF)
            return false;
        if (!ora_buf_be16(out, (uint16_t)len))
            return false;
    } else {
        if (len > 0xFFFFFFFFu)
            return false;
        if (!ora_buf_be32(out, (uint32_t)len))
            return false;
    }
    return len ? ora_buf_append(out, payload, len) : true;
}

bool ora_rtty_pack_register(struct ora_buf *out, int version, const char *devid,
                            const char *description, const char *token)
{
    struct ora_buf p;
    bool ok = false;

    if (!devid || !description || !token)
        return false;

    ora_buf_init(&p);
    if (!ora_buf_append_byte(&p, (uint8_t)version))
        goto out;
    if (!ora_buf_append_str(&p, devid) || !ora_buf_append_byte(&p, 0))
        goto out;
    if (!ora_buf_append_str(&p, description) || !ora_buf_append_byte(&p, 0))
        goto out;
    if (!ora_buf_append_str(&p, token) || !ora_buf_append_byte(&p, 0))
        goto out;
    ok = ora_rtty_pack(out, ORA_RTTY_REGISTER, p.data, p.len);
out:
    ora_buf_free(&p);
    return ok;
}

static bool append_sid(struct ora_buf *p, const char *sid)
{
    size_t n = sid ? strlen(sid) : 0;

    if (n != ORA_RTTY_SID_LEN)
        return false;
    return ora_buf_append(p, sid, ORA_RTTY_SID_LEN);
}

bool ora_rtty_pack_login_response(struct ora_buf *out, const char *sid, int code)
{
    struct ora_buf p;
    bool ok = false;

    ora_buf_init(&p);
    if (!append_sid(&p, sid))
        goto out;
    if (!ora_buf_append_byte(&p, (uint8_t)code))
        goto out;
    ok = ora_rtty_pack(out, ORA_RTTY_LOGIN, p.data, p.len);
out:
    ora_buf_free(&p);
    return ok;
}

bool ora_rtty_pack_termdata(struct ora_buf *out, const char *sid,
                            const void *data, size_t len)
{
    struct ora_buf p;
    bool ok = false;

    ora_buf_init(&p);
    if (!append_sid(&p, sid))
        goto out;
    if (len && !ora_buf_append(&p, data, len))
        goto out;
    ok = ora_rtty_pack(out, ORA_RTTY_TERMDATA, p.data, p.len);
out:
    ora_buf_free(&p);
    return ok;
}

bool ora_rtty_pack_heartbeat(struct ora_buf *out, uint32_t uptime)
{
    uint8_t p[4] = {
        (uint8_t)(uptime >> 24), (uint8_t)(uptime >> 16),
        (uint8_t)(uptime >> 8), (uint8_t)uptime
    };

    return ora_rtty_pack(out, ORA_RTTY_HEARTBEAT, p, sizeof(p));
}

bool ora_rtty_pack_tcpdata(struct ora_buf *out, uint8_t tunnel_id,
                           const uint8_t req_id[16], const void *data, size_t len)
{
    struct ora_buf p;
    bool ok = false;

    ora_buf_init(&p);
    if (!ora_buf_append_byte(&p, tunnel_id))
        goto out;
    if (!ora_buf_append(&p, req_id, 16))
        goto out;
    if (len && !ora_buf_append(&p, data, len))
        goto out;
    ok = ora_rtty_pack(out, ORA_RTTY_TCPDATA, p.data, p.len);
out:
    ora_buf_free(&p);
    return ok;
}

/* ---- reader ---- */

void ora_rtty_reader_init(struct ora_rtty_reader *r)
{
    ora_buf_init(&r->in);
    r->type = -1;
    r->declared = 0;
    r->have_header = false;
}

void ora_rtty_reader_free(struct ora_rtty_reader *r)
{
    ora_buf_free(&r->in);
}

bool ora_rtty_reader_feed(struct ora_rtty_reader *r, const void *data, size_t n)
{
    return ora_buf_append(&r->in, data, n);
}

int ora_rtty_reader_next(struct ora_rtty_reader *r, int *type,
                         const uint8_t **payload, size_t *len)
{
    if (!r->have_header) {
        size_t hdr;
        int t;

        if (r->in.len < 1)
            return 0;
        t = r->in.data[0];
        hdr = ora_rtty_is_v1(t) ? 3 : 5;
        if (r->in.len < hdr)
            return 0;
        if (hdr == 3) {
            r->declared = ((uint32_t)r->in.data[1] << 8) | r->in.data[2];
        } else {
            r->declared = ((uint32_t)r->in.data[1] << 24) |
                          ((uint32_t)r->in.data[2] << 16) |
                          ((uint32_t)r->in.data[3] << 8) |
                           (uint32_t)r->in.data[4];
        }
        if (r->declared > ORA_RTTY_MAX_PAYLOAD)
            return -1;
        r->type = t;
        r->have_header = true;
        memmove(r->in.data, r->in.data + hdr, r->in.len - hdr);
        r->in.len -= hdr;
    }
    if (r->in.len < r->declared)
        return 0;
    *type = r->type;
    *payload = r->in.data;
    *len = r->declared;
    return 1;
}

void ora_rtty_reader_consume(struct ora_rtty_reader *r)
{
    if (!r->have_header)
        return;
    memmove(r->in.data, r->in.data + r->declared, r->in.len - r->declared);
    r->in.len -= r->declared;
    r->have_header = false;
    r->declared = 0;
    r->type = -1;
}

bool ora_rtty_parse_register_response(const uint8_t *payload, size_t len,
                                      int *err, char *msg, size_t msgsz)
{
    size_t n;

    if (!payload || len < 1)
        return false;
    if (err)
        *err = payload[0];
    if (msg && msgsz) {
        n = len - 1;
        if (n >= msgsz)
            n = msgsz - 1;
        memcpy(msg, payload + 1, n);
        msg[n] = '\0';
    }
    return true;
}

bool ora_rtty_parse_sid(const uint8_t *payload, size_t len, char *sid, size_t sidsz)
{
    if (!payload || len < ORA_RTTY_SID_LEN || sidsz < ORA_RTTY_SID_LEN + 1)
        return false;
    memcpy(sid, payload, ORA_RTTY_SID_LEN);
    sid[ORA_RTTY_SID_LEN] = '\0';
    return true;
}
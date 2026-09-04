/* orouteragent - ECSP JSON message envelope codec implementation */
#include "message.h"

#include "../util.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static uint64_t g_seq;

void ora_msg_seq_set(uint64_t start)
{
    g_seq = start;
}

uint64_t ora_msg_seq_next(void)
{
    return ++g_seq;
}

void ora_header_init(struct ora_header *h, const char *mac, int64_t type,
                     const char *dest)
{
    memset(h, 0, sizeof(*h));
    snprintf(h->mac, sizeof(h->mac), "%s", mac ? mac : "");
    h->type = type;
    snprintf(h->device, sizeof(h->device), "%s", ORA_DEVICE_TYPE_GATEWAY);
    snprintf(h->version, sizeof(h->version), "%s", ORA_PROTOCOL_VERSION);
    h->ver_cap = ORA_PROTOCOL_VER_CAP;
    h->timestamp = (int64_t)ora_now_ms();
    h->seq = (int64_t)ora_msg_seq_next();
    h->error = 0;
    if (dest && *dest) {
        snprintf(h->dest, sizeof(h->dest), "%s", dest);
        h->have_dest = true;
    }
}

json_object *ora_header_to_json(const struct ora_header *h)
{
    json_object *o = json_object_new_object();
    if (!o)
        return NULL;

    json_object *v;
    if ((v = json_object_new_string(h->mac))) json_object_object_add(o, "mac", v);
    if ((v = json_object_new_int64(h->type))) json_object_object_add(o, "type", v);
    if ((v = json_object_new_string(h->device))) json_object_object_add(o, "device", v);
    if ((v = json_object_new_string(h->version))) json_object_object_add(o, "version", v);
    if ((v = json_object_new_int64(h->ver_cap))) json_object_object_add(o, "verCap", v);
    if ((v = json_object_new_int64(h->timestamp))) json_object_object_add(o, "timestamp", v);
    if ((v = json_object_new_int64(h->seq))) json_object_object_add(o, "seq", v);
    if ((v = json_object_new_int64(h->error))) json_object_object_add(o, "error", v);
    if (h->have_dest && (v = json_object_new_string(h->dest)))
        json_object_object_add(o, "dest", v);
    return o;
}

bool ora_msg_encode(struct ora_buf *out, const struct ora_header *h,
                    const json_object *body)
{
    json_object *root = NULL;
    json_object *hdr = NULL;
    const char *s;
    size_t len;
    bool ok = false;

    if (!out || !h)
        return false;

    /* ECSP envelope: {"header": {...}, "body": {...}} */
    root = json_object_new_object();
    hdr = ora_header_to_json(h);
    if (!root || !hdr)
        goto out;
    json_object_object_add(root, "header", hdr);
    hdr = NULL;

    json_object_object_add(root, "body",
        body ? json_object_get((json_object *)body) : json_object_new_object());

    s = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    if (!s)
        goto out;
    len = strlen(s);
    if (!ora_buf_append(out, s, len))
        goto out;
    ok = true;
out:
    if (hdr)
        json_object_put(hdr);
    json_object_put(root);
    return ok;
}

bool ora_msg_decode(struct ora_message *msg, const uint8_t *payload, size_t len)
{
    json_object *root = NULL;
    json_object *hdr;
    json_object *o;
    json_tokener *tok = json_tokener_new();
    bool ok = false;

    memset(msg, 0, sizeof(*msg));

    if (!payload || !tok)
        goto out;

    root = json_tokener_parse_ex(tok, (const char *)payload, (int)len);
    if (!root || json_tokener_get_error(tok) != json_tokener_success)
        goto out;
    if (!json_object_is_type(root, json_type_object))
        goto out;

    hdr = ora_json_get_obj(root, "header");
    if (!hdr)
        goto out;

    {
        const char *s;

        if ((s = ora_json_get_str(hdr, "mac", NULL)))
            snprintf(msg->hdr.mac, sizeof(msg->hdr.mac), "%s", s);
        if ((o = json_object_object_get(hdr, "type")))
            msg->hdr.type = json_object_get_int64(o);
        if ((s = ora_json_get_str(hdr, "device", NULL)))
            snprintf(msg->hdr.device, sizeof(msg->hdr.device), "%s", s);
        if ((s = ora_json_get_str(hdr, "version", NULL)))
            snprintf(msg->hdr.version, sizeof(msg->hdr.version), "%s", s);
        if ((o = json_object_object_get(hdr, "verCap")))
            msg->hdr.ver_cap = json_object_get_int64(o);
        if ((o = json_object_object_get(hdr, "timestamp")))
            msg->hdr.timestamp = json_object_get_int64(o);
        if ((o = json_object_object_get(hdr, "seq")))
            msg->hdr.seq = json_object_get_int64(o);
        if ((o = json_object_object_get(hdr, "error")))
            msg->hdr.error = json_object_get_int64(o);
        if ((s = ora_json_get_str(hdr, "dest", NULL)) && *s) {
            snprintf(msg->hdr.dest, sizeof(msg->hdr.dest), "%s", s);
            msg->hdr.have_dest = true;
        }
    }

    o = json_object_object_get(root, "body");
    if (o)
        msg->body = json_object_get(o);
    ok = true;
out:
    if (root)
        json_object_put(root);
    if (tok)
        json_tokener_free(tok);
    return ok;
}

void ora_msg_free(struct ora_message *msg)
{
    if (msg->body) {
        json_object_put(msg->body);
        msg->body = NULL;
    }
}

const char *ora_json_get_str(json_object *o, const char *key, const char *dflt)
{
    json_object *v = o ? json_object_object_get(o, key) : NULL;
    if (!v)
        return dflt;
    return json_object_get_string(v) ? json_object_get_string(v) : dflt;
}

int64_t ora_json_get_int(json_object *o, const char *key, int64_t dflt)
{
    json_object *v = o ? json_object_object_get(o, key) : NULL;
    return v ? json_object_get_int64(v) : dflt;
}

bool ora_json_get_bool(json_object *o, const char *key, bool dflt)
{
    json_object *v = o ? json_object_object_get(o, key) : NULL;
    if (!v)
        return dflt;
    if (json_object_is_type(v, json_type_boolean))
        return json_object_get_boolean(v);
    if (json_object_is_type(v, json_type_int)) {
        return json_object_get_int(v) != 0;
    }
    return dflt;
}

json_object *ora_json_get_obj(json_object *o, const char *key)
{
    json_object *v = o ? json_object_object_get(o, key) : NULL;
    return (v && json_object_is_type(v, json_type_object)) ? v : NULL;
}
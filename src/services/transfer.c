/* orouteragent - file-transfer channel implementation */
#include "transfer.h"
#include "capture.h"
#include "tls.h"
#include "../protocol/constants.h"
#include "../protocol/framing.h"
#include "../protocol/message.h"
#include "../system_info.h"
#include "../util.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static struct {
    pthread_mutex_t lock;
    pthread_t thread;
    bool thread_valid;
    bool running;
    volatile bool stop;
    struct ora_tls tls;
    struct ora_config cfg;
} g_tr = { .lock = PTHREAD_MUTEX_INITIALIZER };

static bool send_msg(struct ora_tls *t, const struct ora_header *hdr,
                     json_object *body)
{
    struct ora_buf json, frame;
    bool ok;

    ora_buf_init(&json);
    ora_buf_init(&frame);
    ok = ora_msg_encode(&json, hdr, body) &&
         ora_frame_encode(&frame, json.data, json.len) &&
         ora_tls_write(t, frame.data, frame.len) == (int)frame.len;
    ora_buf_free(&json);
    ora_buf_free(&frame);
    return ok;
}

/* PRE_CONNECT_INFO carrying the transfer token; the controller replies
 * with PRE_CONNECT_INFO_RESPONSE{errCode:0}. No verify/negotiation. */
static bool handshake(struct ora_tls *t, const struct ora_config *cfg,
                      const char *token)
{
    struct ora_header hdr;
    struct ora_frame_reader rd;
    json_object *body;
    char ipbuf[INET_ADDRSTRLEN];
    bool ok = false;
    uint64_t deadline;

    ora_sys_device_ip(cfg->controller[0] ? cfg->controller : NULL,
                      ipbuf, sizeof(ipbuf));

    body = json_object_new_object();
    {
        json_object *di = json_object_new_object();
        json_object *misc = json_object_new_object();
        char up[64];

        ora_format_uptime(ora_uptime_s(), up, sizeof(up));
        json_object_object_add(di, "ip", json_object_new_string(ipbuf));
        json_object_object_add(di, "model", json_object_new_string(cfg->profile->model));
        json_object_object_add(di, "modelVer", json_object_new_string(cfg->profile->model_ver));
        json_object_object_add(di, "fwVer", json_object_new_string(cfg->fw_version));
        json_object_object_add(di, "cerVer", json_object_new_string("1.0"));
        json_object_object_add(di, "hwVer", json_object_new_string(cfg->hw_version));
        json_object_object_add(di, "time", json_object_new_string(up));
        json_object_object_add(di, "wireless",
                               json_object_new_int(cfg->profile->wireless));
        json_object_object_add(body, "deviceInfo", di);

        json_object_object_add(misc, "portNum",
                               json_object_new_int(cfg->profile->port_num));
        json_object_object_add(misc, "customizeRegion", json_object_new_int(0));
        json_object_object_add(body, "deviceMisc", misc);
    }
    json_object_object_add(body, "token", json_object_new_string(token));

    ora_header_init(&hdr, cfg->mac, ORA_MSG_PRE_CONNECT_INFO, NULL);
    if (!send_msg(t, &hdr, body)) {
        json_object_put(body);
        return false;
    }
    json_object_put(body);

    ora_frame_reader_init(&rd, ORA_MAX_FRAME_PAYLOAD);
    deadline = ora_now_ms() + 5000;
    while (ora_now_ms() < deadline) {
        uint8_t chunk[4096];
        const uint8_t *payload;
        size_t plen;
        struct ora_message msg;
        int n = ora_tls_read(t, chunk, sizeof(chunk), 1000);

        if (n < 0)
            break;
        if (n == 0)
            continue;
        if (!ora_frame_reader_feed(&rd, chunk, (size_t)n))
            break;
        if (ora_frame_reader_next(&rd, &payload, &plen) != 1)
            continue;
        if (ora_msg_decode(&msg, payload, plen)) {
            int64_t err = msg.hdr.error;
            int64_t berr = ora_json_get_int(msg.body, "errCode", 0);

            ok = err == 0 && berr == 0;
            if (!ok)
                ora_log(ORA_LOG_WARN,
                        "transfer: pre-connect rejected (error=%" PRId64
                        " errCode=%" PRId64 ")", err, berr);
            ora_msg_free(&msg);
        }
        ora_frame_reader_consume(&rd);
        break;
    }
    ora_frame_reader_free(&rd);
    return ok;
}

/* The controller may also request partitions on this channel. */
static void *transfer_thread(void *arg)
{
    struct ora_frame_reader rd;

    (void)arg;
    ora_frame_reader_init(&rd, ORA_MAX_FRAME_PAYLOAD);

    while (!g_tr.stop) {
        uint8_t chunk[8192];
        int n;

        if (!ora_tls_wait_readable(&g_tr.tls, 200))
            continue;
        n = ora_tls_read(&g_tr.tls, chunk, sizeof(chunk), 500);
        if (n < 0)
            break;
        if (n > 0 && !ora_frame_reader_feed(&rd, chunk, (size_t)n))
            break;

        for (;;) {
            const uint8_t *payload;
            size_t plen;
            struct ora_message msg;
            int rv = ora_frame_reader_next(&rd, &payload, &plen);

            if (rv == 0)
                break;
            if (rv < 0)
                goto out;
            if (ora_msg_decode(&msg, payload, plen)) {
                if (msg.hdr.type == ORA_MSG_FILE_TRANSFER_REQUEST_V2)
                    ora_capture_handle_transfer_request(msg.body, msg.hdr.seq);
                ora_msg_free(&msg);
            }
            ora_frame_reader_consume(&rd);
        }
    }
out:
    ora_frame_reader_free(&rd);
    ora_log(ORA_LOG_INFO, "transfer: channel closed");

    pthread_mutex_lock(&g_tr.lock);
    ora_tls_close(&g_tr.tls);
    ora_tls_free(&g_tr.tls);
    g_tr.running = false;
    pthread_mutex_unlock(&g_tr.lock);
    return NULL;
}

bool ora_transfer_open(const struct ora_config *cfg, json_object *tc)
{
    const char *token = ora_json_get_str(tc, "token", "");
    int port = (int)ora_json_get_int(tc, "port", ORA_TRANSFER_TCP_PORT);
    bool ok = false;

    pthread_mutex_lock(&g_tr.lock);
    if (g_tr.running) {
        pthread_mutex_unlock(&g_tr.lock);
        return true;
    }
    if (g_tr.thread_valid) {
        pthread_join(g_tr.thread, NULL);
        g_tr.thread_valid = false;
    }
    g_tr.cfg = *cfg;
    g_tr.stop = false;

    if (!ora_tls_init(&g_tr.tls))
        goto out;
    if (!ora_tls_connect(&g_tr.tls, cfg->controller, port,
                         ORA_CONNECT_TIMEOUT_S * 1000, cfg->verify_tls)) {
        ora_tls_free(&g_tr.tls);
        goto out;
    }
    /* the channel must be up before the SET_RESPONSE for transferChannel */
    if (!handshake(&g_tr.tls, cfg, token)) {
        ora_tls_close(&g_tr.tls);
        ora_tls_free(&g_tr.tls);
        goto out;
    }
    ora_log(ORA_LOG_INFO, "transfer: channel established (%s:%d)",
            cfg->controller, port);

    if (ora_thread_create(&g_tr.thread, transfer_thread, NULL) != 0) {
        ora_tls_close(&g_tr.tls);
        ora_tls_free(&g_tr.tls);
        goto out;
    }
    g_tr.thread_valid = true;
    g_tr.running = true;
    ok = true;
out:
    pthread_mutex_unlock(&g_tr.lock);
    return ok;
}

void ora_transfer_stop(void)
{
    pthread_t th;
    bool join = false;

    pthread_mutex_lock(&g_tr.lock);
    g_tr.stop = true;
    if (g_tr.thread_valid) {
        th = g_tr.thread;
        g_tr.thread_valid = false;
        join = true;
    }
    pthread_mutex_unlock(&g_tr.lock);

    if (join)
        pthread_join(th, NULL);
}

bool ora_transfer_running(void)
{
    bool r;

    pthread_mutex_lock(&g_tr.lock);
    r = g_tr.running;
    pthread_mutex_unlock(&g_tr.lock);
    return r;
}
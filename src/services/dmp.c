/* orouteragent - DMP (Network Check) service implementation */
#include "dmp.h"
#include "tls.h"
#include "../protocol/constants.h"
#include "../protocol/dmp_proto.h"
#include "../protocol/framing.h"
#include "../protocol/message.h"
#include "../util.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define ORA_DMP_PING_COUNT 4
#define ORA_DMP_PROBE_TIMEOUT_S 20
/* probe output buffer: heap, not thread stack */
#define ORA_DMP_OUT_SIZE 16384

static struct {
    pthread_t thread;
    pthread_mutex_t lock;
    bool running;
    volatile bool stop;
    struct ora_config cfg;
    struct ora_dmp_settings set;
    int64_t seq;
} g_dmp = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* ---- probe execution ---- */

/* Reject anything that is not a plain host/IP: the target string comes
 * from the controller and is passed to an exec'd binary. */
static bool valid_target(const char *s)
{
    size_t i, n;

    if (!s || !*s)
        return false;
    n = strlen(s);
    if (n > 253)
        return false;
    for (i = 0; i < n; i++) {
        char c = s[i];

        if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != ':' && c != '_')
            return false;
    }
    return s[0] != '-';
}

/* Run @argv (no shell) and capture stdout. Returns false on spawn error. */
static bool run_capture(char *const argv[], char *out, size_t outsz, int timeout_s)
{
    int fds[2];
    pid_t pid;
    size_t used = 0;
    uint64_t deadline;
    bool ok = false;

    out[0] = '\0';
    if (pipe(fds) != 0)
        return false;

    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        if (fds[1] > STDERR_FILENO)
            close(fds[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);

    deadline = ora_now_ms() + (uint64_t)timeout_s * 1000;
    for (;;) {
        struct pollfd pfd = { .fd = fds[0], .events = POLLIN, .revents = 0 };
        int64_t remain = (int64_t)deadline - (int64_t)ora_now_ms();
        ssize_t n;

        if (remain <= 0)
            break;
        if (poll(&pfd, 1, (int)remain) <= 0)
            break;
        n = read(fds[0], out + used, outsz - used - 1);
        if (n <= 0) {
            ok = true; /* EOF: the probe finished */
            break;
        }
        used += (size_t)n;
        out[used] = '\0';
        if (used + 1 >= outsz) {
            ok = true;
            break;
        }
    }
    close(fds[0]);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return ok;
}

/* Parse busybox ping output into the controller's ping result shape. */
static json_object *ping_probe(const char *target)
{
    char count[8];
    char *argv[] = { (char *)"/bin/ping", (char *)"-c", count,
                     (char *)"-w", (char *)"10", (char *)target, NULL };
    json_object *res = json_object_new_object();
    json_object *rtts = json_object_new_array();
    char *out;
    const char *p;
    int sent = ORA_DMP_PING_COUNT, received = 0;
    double min_rtt = 0, max_rtt = 0, sum_rtt = 0;
    int n_rtt = 0;

    snprintf(count, sizeof(count), "%d", ORA_DMP_PING_COUNT);

    out = malloc(ORA_DMP_OUT_SIZE);
    if (out && run_capture(argv, out, ORA_DMP_OUT_SIZE, ORA_DMP_PROBE_TIMEOUT_S)) {
        for (p = out; (p = strstr(p, "time=")) != NULL; ) {
            double rtt = strtod(p + 5, NULL);

            p += 5;
            if (rtt <= 0)
                continue;
            json_object_array_add(rtts, json_object_new_double(rtt));
            if (n_rtt == 0 || rtt < min_rtt)
                min_rtt = rtt;
            if (rtt > max_rtt)
                max_rtt = rtt;
            sum_rtt += rtt;
            n_rtt++;
        }
        received = n_rtt;
        {
            const char *stat = strstr(out, "packets transmitted");

            if (stat) {
                int tx = 0, rx = 0;

                /* busybox: "4 packets transmitted, 4 packets received, .." */
                while (stat > out && *(stat - 1) != '\n')
                    stat--;
                if (sscanf(stat, "%d packets transmitted, %d packets received",
                           &tx, &rx) == 2) {
                    sent = tx;
                    received = rx;
                }
            }
        }
    }

    json_object_object_add(res, "target", json_object_new_string(target));
    json_object_object_add(res, "ip", json_object_new_string(target));
    json_object_object_add(res, "packetsSent", json_object_new_int(sent));
    json_object_object_add(res, "packetsReceived", json_object_new_int(received));
    json_object_object_add(res, "packetsLost",
                           json_object_new_int(sent > received ? sent - received : 0));
    json_object_object_add(res, "lossRate",
                           json_object_new_int(sent ? (sent - received) * 100 / sent : 100));
    json_object_object_add(res, "minRtt", json_object_new_double(min_rtt));
    json_object_object_add(res, "maxRtt", json_object_new_double(max_rtt));
    json_object_object_add(res, "avgRtt",
                           json_object_new_double(n_rtt ? sum_rtt / n_rtt : 0));
    json_object_object_add(res, "rtts", rtts);
    json_object_object_add(res, "status",
                           json_object_new_string(received > 0 ? "success" : "failed"));
    free(out);
    return res;
}

/* Parse busybox traceroute output into the controller's hop list. */
static json_object *traceroute_probe(const char *target)
{
    char *argv[] = { (char *)"/usr/bin/traceroute", (char *)"-n",
                     (char *)"-q", (char *)"1", (char *)"-w", (char *)"2",
                     (char *)target, NULL };
    json_object *res = json_object_new_object();
    json_object *hops = json_object_new_array();
    char *out;
    char *line, *save;
    bool ran;

    out = malloc(ORA_DMP_OUT_SIZE);
    if (!out) {
        json_object_object_add(res, "target", json_object_new_string(target));
        json_object_object_add(res, "hops", hops);
        json_object_object_add(res, "status", json_object_new_string("failed"));
        return res;
    }

    ran = run_capture(argv, out, ORA_DMP_OUT_SIZE, ORA_DMP_PROBE_TIMEOUT_S);
    if (!ran) {
        argv[0] = (char *)"/bin/traceroute";
        ran = run_capture(argv, out, ORA_DMP_OUT_SIZE, ORA_DMP_PROBE_TIMEOUT_S);
    }

    if (ran) {
        for (line = strtok_r(out, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            int hop = 0;
            char ip[64] = {0};
            double rtt = 0;
            json_object *e, *rtts;

            if (sscanf(line, " %d %63s %lf", &hop, ip, &rtt) < 2)
                continue;
            if (hop <= 0)
                continue;

            e = json_object_new_object();
            rtts = json_object_new_array();
            json_object_object_add(e, "hop", json_object_new_int(hop));
            if (!strcmp(ip, "*")) {
                json_object_object_add(e, "ip", json_object_new_string(""));
                json_object_object_add(e, "status", json_object_new_string("timeout"));
            } else {
                json_object_object_add(e, "ip", json_object_new_string(ip));
                json_object_object_add(e, "status", json_object_new_string("success"));
                if (rtt > 0)
                    json_object_array_add(rtts, json_object_new_double(rtt));
            }
            json_object_object_add(e, "rtts", rtts);
            json_object_array_add(hops, e);
        }
    }

    json_object_object_add(res, "target", json_object_new_string(target));
    json_object_object_add(res, "hops", hops);
    json_object_object_add(res, "status",
        json_object_new_string(json_object_array_length(hops) ? "success" : "failed"));
    free(out);
    return res;
}

/* The probe target may arrive as JSON or as a bare string. */
static void extract_target(const uint8_t *data, size_t len, char *out, size_t outsz)
{
    json_object *root;
    const char *t = NULL;
    char tmp[512];
    size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;

    out[0] = '\0';
    if (!data || !len)
        return;
    memcpy(tmp, data, n);
    tmp[n] = '\0';

    root = json_tokener_parse(tmp);
    if (root) {
        t = ora_json_get_str(root, "target", NULL);
        if (!t)
            t = ora_json_get_str(root, "host", NULL);
        if (!t)
            t = ora_json_get_str(root, "ip", NULL);
        if (t)
            snprintf(out, outsz, "%s", t);
        json_object_put(root);
        return;
    }
    /* bare string payload */
    n = strlen(tmp);
    if (n >= outsz)
        n = outsz - 1;
    memcpy(out, tmp, n);
    out[n] = '\0';
}

/* ---- channel ---- */

static void mac_raw(const char *mac_str, uint8_t out[6])
{
    int i, v;

    memset(out, 0, 6);
    for (i = 0; i < 6; i++) {
        if (sscanf(mac_str + i * 3, "%2x", &v) != 1)
            return;
        out[i] = (uint8_t)v;
    }
}

static void fill_header(struct ora_dmp_header *h, const struct ora_config *cfg,
                        const struct ora_dmp_settings *set, int64_t msg_type,
                        const char *path)
{
    memset(h, 0, sizeof(*h));
    mac_raw(cfg->mac, h->mac);
    snprintf(h->token, sizeof(h->token), "%s", set->token);
    snprintf(h->path, sizeof(h->path), "%s", path ? path : "/");
    h->version = 1;
    h->msg_type = msg_type;
    h->seq = ++g_dmp.seq;
    h->dev_type = 0;
    h->error_code = 0;
    h->need_reply = 0;
    h->epoch_ms = ora_now_ms();
    h->content_type = 0;
}

static bool send_message(struct ora_tls *tls, const struct ora_dmp_header *hdr,
                         const void *data, size_t len)
{
    struct ora_buf pb, frame;
    bool ok = false;

    ora_buf_init(&pb);
    ora_buf_init(&frame);
    if (!ora_dmp_encode(&pb, hdr, data, len))
        goto out;
    if (!ora_frame_encode(&frame, pb.data, pb.len))
        goto out;
    ok = ora_tls_write(tls, frame.data, frame.len) == (int)frame.len;
out:
    ora_buf_free(&pb);
    ora_buf_free(&frame);
    return ok;
}

static void handle_probe(struct ora_tls *tls, const struct ora_config *cfg,
                         const struct ora_dmp_settings *set,
                         const struct ora_dmp_header *req,
                         const uint8_t *data, size_t len)
{
    json_object *result = NULL;
    struct ora_dmp_header hdr;
    char target[256];
    const char *s;

    extract_target(data, len, target, sizeof(target));

    if (strstr(req->path, "ping")) {
        if (!valid_target(target))
            snprintf(target, sizeof(target), "8.8.8.8");
        ora_log(ORA_LOG_INFO, "dmp: ping %s", target);
        result = ping_probe(target);
    } else if (strstr(req->path, "traceroute")) {
        if (!valid_target(target))
            snprintf(target, sizeof(target), "8.8.8.8");
        ora_log(ORA_LOG_INFO, "dmp: traceroute %s", target);
        result = traceroute_probe(target);
    } else {
        return; /* keepalive or unknown probe */
    }

    fill_header(&hdr, cfg, set, ORA_DMP_MSG_JSON_COMPONENT_LIST, req->path);
    hdr.seq = req->seq;
    s = json_object_to_json_string_ext(result, JSON_C_TO_STRING_PLAIN);
    if (s)
        send_message(tls, &hdr, s, strlen(s));
    json_object_put(result);
}

static void serve(struct ora_tls *tls, const struct ora_config *cfg,
                  const struct ora_dmp_settings *set)
{
    struct ora_frame_reader rd;
    uint64_t last_hb = 0;

    ora_frame_reader_init(&rd, ORA_MAX_FRAME_PAYLOAD);

    while (!g_dmp.stop) {
        uint64_t now = ora_now_ms();

        if (now - last_hb >= (uint64_t)ORA_HEARTBEAT_INTERVAL_S * 1000) {
            struct ora_dmp_header hdr;

            fill_header(&hdr, cfg, set, ORA_DMP_MSG_EMPTY, set->path);
            if (!send_message(tls, &hdr, NULL, 0))
                break;
            last_hb = now;
        }

        if (!ora_tls_wait_readable(tls, 200))
            continue;
        {
            uint8_t chunk[8192];
            int n = ora_tls_read(tls, chunk, sizeof(chunk), 500);

            if (n < 0)
                break;
            if (n > 0 && !ora_frame_reader_feed(&rd, chunk, (size_t)n))
                break;
        }
        for (;;) {
            const uint8_t *payload;
            size_t plen;
            int rv = ora_frame_reader_next(&rd, &payload, &plen);
            struct ora_dmp_header req;
            const uint8_t *data;
            size_t dlen;

            if (rv == 0)
                break;
            if (rv < 0) {
                ora_log(ORA_LOG_WARN, "dmp: oversized frame; reconnecting");
                goto out;
            }
            if (ora_dmp_decode(payload, plen, &req, &data, &dlen))
                handle_probe(tls, cfg, set, &req, data, dlen);
            ora_frame_reader_consume(&rd);
        }
    }
out:
    ora_frame_reader_free(&rd);
}

static void *dmp_thread(void *arg)
{
    (void)arg;

    while (!g_dmp.stop) {
        struct ora_tls tls;
        struct ora_dmp_settings set;
        struct ora_config cfg;
        struct ora_dmp_header hdr;

        pthread_mutex_lock(&g_dmp.lock);
        set = g_dmp.set;
        cfg = g_dmp.cfg;
        pthread_mutex_unlock(&g_dmp.lock);

        if (!ora_tls_init(&tls)) {
            sleep(ORA_RECONNECT_DELAY_S);
            continue;
        }
        if (ora_tls_connect(&tls, set.host, set.port,
                            ORA_CONNECT_TIMEOUT_S * 1000, cfg.verify_tls)) {
            fill_header(&hdr, &cfg, &set, ORA_DMP_MSG_EMPTY, set.path);
            if (send_message(&tls, &hdr, NULL, 0)) {
                ora_log(ORA_LOG_INFO, "dmp: registered (path %s)", set.path);
                serve(&tls, &cfg, &set);
            }
            ora_tls_close(&tls);
        }
        ora_tls_free(&tls);

        if (!g_dmp.stop)
            sleep(ORA_RECONNECT_DELAY_S);
    }
    return NULL;
}

bool ora_dmp_start(const struct ora_config *cfg,
                   const struct ora_dmp_settings *set)
{
    bool started = false;

    pthread_mutex_lock(&g_dmp.lock);
    g_dmp.cfg = *cfg;
    g_dmp.set = *set;
    if (g_dmp.set.port <= 0)
        g_dmp.set.port = ORA_DEVICE_MONITOR_PORT;
    if (!g_dmp.set.path[0])
        snprintf(g_dmp.set.path, sizeof(g_dmp.set.path), "/");
    if (!g_dmp.running) {
        g_dmp.stop = false;
        if (ora_thread_create(&g_dmp.thread, dmp_thread, NULL) == 0) {
            g_dmp.running = true;
            started = true;
            ora_log(ORA_LOG_INFO, "dmp: service started (%s:%d)",
                    g_dmp.set.host, g_dmp.set.port);
        } else {
            ora_log(ORA_LOG_ERR, "dmp: cannot start thread");
        }
    } else {
        started = true;
    }
    pthread_mutex_unlock(&g_dmp.lock);
    return started;
}

void ora_dmp_stop(void)
{
    pthread_t th;

    pthread_mutex_lock(&g_dmp.lock);
    if (!g_dmp.running) {
        pthread_mutex_unlock(&g_dmp.lock);
        return;
    }
    g_dmp.stop = true;
    th = g_dmp.thread;
    g_dmp.running = false;
    pthread_mutex_unlock(&g_dmp.lock);

    pthread_join(th, NULL);
    ora_log(ORA_LOG_INFO, "dmp: service stopped");
}

bool ora_dmp_running(void)
{
    bool r;

    pthread_mutex_lock(&g_dmp.lock);
    r = g_dmp.running;
    pthread_mutex_unlock(&g_dmp.lock);
    return r;
}
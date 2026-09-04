/* orouteragent - packet capture service implementation */
#include "capture.h"
#include "manage.h"
#include "transfer.h"
#include "../protocol/base64.h"
#include "../protocol/constants.h"
#include "../protocol/message.h"
#include "../protocol/pcap.h"
#include "../util.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The controller announces capture files with this notify type. */
#define ORA_CAPTURE_FILE_NOTIFY_TYPE 1

static struct {
    pthread_mutex_t lock;
    pthread_t thread;
    bool thread_valid;      /* thread created and not yet joined */
    bool running;
    volatile bool stop;
    struct ora_config cfg;
    struct ora_capture_opts opts;
    char nid[64];
    char file_name[128];
    char file_path[256];
} g_cap = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* Build the capture file name from the device MAC and the capture
 * session id. The nid comes from the controller, so it is reduced to a
 * safe charset: anything else (notably '/' and "..") would let a peer
 * steer this root-owned write outside the capture directory. */
static void build_file_name(const char *mac, const char *nid,
                            char *name, size_t namesz,
                            char *path, size_t pathsz)
{
    char safe_mac[32];
    char safe_nid[64];
    size_t i, j = 0;

    for (i = 0; mac[i] && j < sizeof(safe_mac) - 1; i++) {
        if (mac[i] == '-' || mac[i] == ':')
            continue;
        if (isalnum((unsigned char)mac[i]))
            safe_mac[j++] = mac[i];
    }
    safe_mac[j] = '\0';

    j = 0;
    for (i = 0; nid && nid[i] && j < sizeof(safe_nid) - 1; i++) {
        char c = nid[i];

        if (isalnum((unsigned char)c) || c == '-' || c == '_')
            safe_nid[j++] = c;
    }
    safe_nid[j] = '\0';

    snprintf(name, namesz, "%s_%s.pcap", safe_mac,
             safe_nid[0] ? safe_nid : "capture");
    snprintf(path, pathsz, "%s/%s", ORA_CAPTURE_DIR, name);
}

/* Announce the finished capture so the controller creates its
 * reassembly cache for this cmdId. */
static bool announce_file(void)
{
    json_object *content = json_object_new_object();
    json_object *infos = json_object_new_array();
    json_object *info = json_object_new_object();
    char md5[33] = {0};
    char path[256], name[128], nid[64];
    ssize_t size;
    int fd;
    bool ok;

    pthread_mutex_lock(&g_cap.lock);
    snprintf(path, sizeof(path), "%s", g_cap.file_path);
    snprintf(name, sizeof(name), "%s", g_cap.file_name);
    snprintf(nid, sizeof(nid), "%s", g_cap.nid);
    pthread_mutex_unlock(&g_cap.lock);

    /* Size and digest come from one open fd, so the file cannot be
     * swapped between measuring it and hashing it. */
    fd = ora_pcap_open_read(path);
    size = ora_pcap_fd_size(fd);
    if (fd >= 0 && size >= 0)
        ora_pcap_fd_md5(fd, md5, sizeof(md5));
    if (fd >= 0)
        close(fd);
    if (size < 0)
        size = 0;

    json_object_object_add(info, "fileName", json_object_new_string(name));
    json_object_object_add(info, "filePath", json_object_new_string(path));
    json_object_object_add(info, "fileSize", json_object_new_int64((int64_t)size));
    json_object_object_add(info, "md5", json_object_new_string(md5));
    json_object_array_add(infos, info);

    json_object_object_add(content, "errCode", json_object_new_int(0));
    json_object_object_add(content, "cmdId", json_object_new_string(nid));
    json_object_object_add(content, "type",
                           json_object_new_int(ORA_CAPTURE_FILE_NOTIFY_TYPE));
    json_object_object_add(content, "fileInfos", infos);

    ok = ora_manage_send_notify(ORA_NOTIFY_SUBJECT_FILE_TRANSFER, content);
    if (ok)
        ora_log(ORA_LOG_INFO, "capture: announced %s (%zd bytes, md5=%.8s)",
                name, size, md5);
    else
        ora_log(ORA_LOG_WARN, "capture: could not announce %s", name);

    json_object_put(content);
    return ok;
}

static void *capture_thread(void *arg)
{
    size_t written;

    (void)arg;

    ora_log(ORA_LOG_INFO, "capture: starting on %s for %ds (max %zu bytes)",
            g_cap.opts.ifname[0] ? g_cap.opts.ifname : "any",
            g_cap.opts.duration_s, g_cap.opts.total_size);

    written = ora_pcap_capture(g_cap.file_path, &g_cap.opts, &g_cap.stop);
    ora_log(ORA_LOG_INFO, "capture: wrote %zu bytes to %s", written, g_cap.file_path);

    if (!g_cap.stop && written)
        announce_file();

    pthread_mutex_lock(&g_cap.lock);
    g_cap.running = false;
    pthread_mutex_unlock(&g_cap.lock);
    return NULL;
}

void ora_capture_handle_set(const struct ora_config *cfg, json_object *pc)
{
    const char *op = ora_json_get_str(pc, "operation", "");
    json_object *ci;
    const char *s;

    if (!strcmp(op, "stop")) {
        ora_capture_stop();
        return;
    }
    if (strcmp(op, "start"))
        return;

    if (!ora_pcap_prepare_dir())
        return;

    pthread_mutex_lock(&g_cap.lock);
    if (g_cap.running) {
        pthread_mutex_unlock(&g_cap.lock);
        ora_log(ORA_LOG_WARN, "capture: already running; ignoring start");
        return;
    }
    /* reap the previous worker before starting another */
    if (g_cap.thread_valid) {
        pthread_join(g_cap.thread, NULL);
        g_cap.thread_valid = false;
    }

    memset(&g_cap.opts, 0, sizeof(g_cap.opts));
    g_cap.cfg = *cfg;
    g_cap.stop = false;

    s = ora_json_get_str(pc, "nid", "");
    snprintf(g_cap.nid, sizeof(g_cap.nid), "%s", s);

    ci = ora_json_get_obj(pc, "captureInfo");
    g_cap.opts.duration_s = (int)ora_json_get_int(ci, "duration", 10);
    g_cap.opts.total_size = (size_t)ora_json_get_int(ci, "totalSize", 2 * 1024 * 1024);
    g_cap.opts.vlan_id = (int)ora_json_get_int(ci, "vlanId", 0);
    {
        /* captureInfo.interface is an emulated port number; map it to the
         * real interface backing that port. */
        json_object *iface = ci ? json_object_object_get(ci, "interface") : NULL;
        const char *ifname = NULL;

        if (iface && json_object_is_type(iface, json_type_string))
            ifname = json_object_get_string(iface);
        if (!ifname || !*ifname) {
            int port = (int)ora_json_get_int(ci, "interface", 1);

            ifname = ora_config_port_ifname(cfg, port ? port : 1);
        }
        snprintf(g_cap.opts.ifname, sizeof(g_cap.opts.ifname), "%s",
                 ifname ? ifname : "");
    }
    if (g_cap.opts.duration_s <= 0 || g_cap.opts.duration_s > 300)
        g_cap.opts.duration_s = 10;
    if (g_cap.opts.total_size == 0 || g_cap.opts.total_size > 16 * 1024 * 1024)
        g_cap.opts.total_size = 2 * 1024 * 1024;

    build_file_name(cfg->mac, g_cap.nid, g_cap.file_name, sizeof(g_cap.file_name),
                    g_cap.file_path, sizeof(g_cap.file_path));

    if (ora_thread_create(&g_cap.thread, capture_thread, NULL) == 0) {
        g_cap.thread_valid = true;
        g_cap.running = true;
    } else {
        ora_log(ORA_LOG_ERR, "capture: cannot start worker thread");
    }
    pthread_mutex_unlock(&g_cap.lock);
}

bool ora_capture_handle_transfer_request(json_object *req_body, int64_t seq)
{
    json_object *req = ora_json_get_obj(req_body, "fileTransfer");
    json_object *body, *ft;
    char path[256], name[128];
    int fd = -1;
    ssize_t size;
    int64_t partition, start, end;
    size_t stop, len;
    uint8_t *chunk = NULL;
    char *b64 = NULL;
    bool ok = false;

    if (!req)
        req = req_body;

    /* A new capture may start while a transfer for the previous one is
     * still in flight, so work from a snapshot. */
    pthread_mutex_lock(&g_cap.lock);
    snprintf(path, sizeof(path), "%s", g_cap.file_path);
    snprintf(name, sizeof(name), "%s", g_cap.file_name);
    pthread_mutex_unlock(&g_cap.lock);

    if (!path[0])
        return false;

    /* Serve from one fd: the size that bounds the read is the size of
     * the file actually being read. */
    fd = ora_pcap_open_read(path);
    if (fd < 0)
        return false;
    size = ora_pcap_fd_size(fd);
    if (size <= 0)
        goto out;

    partition = ora_json_get_int(req, "partition", 0);
    start = ora_json_get_int(req, "startIndex", -1);
    if (start < 0) {
        if (partition < 0 || partition > INT64_MAX / ORA_TRANSFER_PARTITION_BYTES)
            goto out;
        start = partition * ORA_TRANSFER_PARTITION_BYTES;
    }
    if (start < 0 || start >= size)
        goto out;

    /* endIndex is inclusive and omitted for all but the last partition */
    end = ora_json_get_int(req, "endIndex", -1);
    stop = (end < 0 || end >= size) ? (size_t)size : (size_t)end + 1;
    if (stop <= (size_t)start)
        goto out;
    len = stop - (size_t)start;
    if (len > ORA_TRANSFER_PARTITION_BYTES)
        len = ORA_TRANSFER_PARTITION_BYTES;

    chunk = malloc(len);
    if (!chunk)
        goto out;
    if (lseek(fd, (off_t)start, SEEK_SET) == (off_t)-1)
        goto out;
    {
        size_t got = 0;

        while (got < len) {
            ssize_t n = read(fd, chunk + got, len - got);

            if (n <= 0)
                goto out;
            got += (size_t)n;
        }
    }

    b64 = malloc(ora_base64_encoded_len(len) + 1);
    if (!b64 || !ora_base64_encode(b64, ora_base64_encoded_len(len) + 1, chunk, len))
        goto out;

    body = json_object_new_object();
    ft = json_object_new_object();
    json_object_object_add(ft, "errCode", json_object_new_int(0));
    json_object_object_add(ft, "fileName",
        json_object_new_string(ora_json_get_str(req, "fileName", name)));
    json_object_object_add(ft, "fileType", json_object_new_string("pcap"));
    json_object_object_add(ft, "compression", json_object_new_string("none"));
    json_object_object_add(ft, "data", json_object_new_string(b64));
    json_object_object_add(ft, "partition", json_object_new_int64(partition));
    json_object_object_add(body, "fileTransfer", ft);

    ok = ora_manage_send_file_transfer(body, seq);
    ora_log(ORA_LOG_INFO, "capture: served partition %" PRId64 " (%zu bytes) -> %s",
            partition, len, ok ? "ok" : "failed");
    json_object_put(body);
out:
    free(b64);
    free(chunk);
    if (fd >= 0)
        close(fd);
    return ok;
}

void ora_capture_stop(void)
{
    pthread_t th;
    bool join = false;

    pthread_mutex_lock(&g_cap.lock);
    g_cap.stop = true;
    if (g_cap.thread_valid) {
        th = g_cap.thread;
        g_cap.thread_valid = false;
        join = true;
    }
    pthread_mutex_unlock(&g_cap.lock);

    /* joined outside the lock: the worker takes it on the way out */
    if (join)
        pthread_join(th, NULL);
    ora_transfer_stop();
}
/* orouteragent - RTTY terminal service implementation */
#include "rtty.h"
#include "tls.h"
#include "../protocol/constants.h"
#include "../protocol/rtty_proto.h"
#include "../util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define ORA_RTTY_MAX_SESSIONS 4
#define ORA_RTTY_MAX_TUNNELS 4

struct rtty_session {
    bool active;
    char sid[ORA_RTTY_SID_LEN + 1];
    int master;         /* PTY master fd */
    pid_t pid;
};

struct rtty_tunnel {
    bool active;
    uint8_t id;
    uint32_t addr;      /* host order */
    uint16_t port;
    int fd;             /* relay socket, -1 when idle */
    uint8_t req_id[16];
};

static struct {
    pthread_t thread;
    pthread_mutex_t lock;
    bool running;
    volatile bool stop;
    struct ora_config cfg;
    struct ora_rtty_settings set;
    struct rtty_session sessions[ORA_RTTY_MAX_SESSIONS];
    struct rtty_tunnel tunnels[ORA_RTTY_MAX_TUNNELS];
} g_rtty = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* ---- PTY sessions ---- */

/* Open a PTY pair and spawn a login shell on the slave side. */
static bool spawn_shell(struct rtty_session *s)
{
    int master, slave;
    char name[64];
    pid_t pid;

    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0)
        return false;
    if (grantpt(master) != 0 || unlockpt(master) != 0)
        goto fail;
    if (ptsname_r(master, name, sizeof(name)) != 0)
        goto fail;

    slave = open(name, O_RDWR | O_NOCTTY);
    if (slave < 0)
        goto fail;

    pid = fork();
    if (pid < 0) {
        close(slave);
        goto fail;
    }
    if (pid == 0) {
        /* Child: only async-signal-safe calls until exec. setenv() would
         * malloc, which can deadlock in the child of a threaded process,
         * so the environment is passed to execle instead. */
        static char *const envp[] = {
            (char *)"TERM=xterm",
            (char *)"HOME=/root",
            (char *)"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
            NULL
        };

        close(master);
        setsid();
        if (ioctl(slave, TIOCSCTTY, 0) < 0)
            _exit(127);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO)
            close(slave);
        execle("/bin/sh", "sh", "-l", (char *)NULL, envp);
        _exit(127);
    }
    close(slave);
    fcntl(master, F_SETFL, O_NONBLOCK);
    s->master = master;
    s->pid = pid;
    return true;
fail:
    close(master);
    return false;
}

static void session_close(struct rtty_session *s)
{
    if (!s->active)
        return;
    if (s->pid > 0) {
        kill(s->pid, SIGHUP);
        waitpid(s->pid, NULL, WNOHANG);
    }
    if (s->master >= 0)
        close(s->master);
    memset(s, 0, sizeof(*s));
    s->master = -1;
}

static struct rtty_session *session_find(const char *sid)
{
    int i;

    for (i = 0; i < ORA_RTTY_MAX_SESSIONS; i++)
        if (g_rtty.sessions[i].active && !strcmp(g_rtty.sessions[i].sid, sid))
            return &g_rtty.sessions[i];
    return NULL;
}

static struct rtty_session *session_alloc(const char *sid)
{
    int i;

    for (i = 0; i < ORA_RTTY_MAX_SESSIONS; i++) {
        if (g_rtty.sessions[i].active)
            continue;
        memset(&g_rtty.sessions[i], 0, sizeof(g_rtty.sessions[i]));
        snprintf(g_rtty.sessions[i].sid, sizeof(g_rtty.sessions[i].sid), "%s", sid);
        g_rtty.sessions[i].active = true;
        g_rtty.sessions[i].master = -1;
        return &g_rtty.sessions[i];
    }
    return NULL;
}

static void sessions_close_all(void)
{
    int i;

    for (i = 0; i < ORA_RTTY_MAX_SESSIONS; i++)
        session_close(&g_rtty.sessions[i]);
}

/* ---- tunnels ---- */

static struct rtty_tunnel *tunnel_find(uint8_t id)
{
    int i;

    for (i = 0; i < ORA_RTTY_MAX_TUNNELS; i++)
        if (g_rtty.tunnels[i].active && g_rtty.tunnels[i].id == id)
            return &g_rtty.tunnels[i];
    return NULL;
}

static void tunnel_close(struct rtty_tunnel *t)
{
    if (!t->active)
        return;
    if (t->fd >= 0)
        close(t->fd);
    memset(t, 0, sizeof(*t));
    t->fd = -1;
}

static void tunnels_close_all(void)
{
    int i;

    for (i = 0; i < ORA_RTTY_MAX_TUNNELS; i++)
        tunnel_close(&g_rtty.tunnels[i]);
}

/* Connect the relay socket of a tunnel on first use. */
static bool tunnel_connect(struct rtty_tunnel *t)
{
    struct sockaddr_in sa;
    int fd;

    if (t->fd >= 0)
        return true;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(t->port);
    sa.sin_addr.s_addr = htonl(t->addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return false;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    t->fd = fd;
    return true;
}

/* ---- frame handling ---- */

static bool send_frame(struct ora_tls *t, struct ora_buf *frame)
{
    bool ok = ora_tls_write(t, frame->data, frame->len) == (int)frame->len;

    ora_buf_reset(frame);
    return ok;
}

static void handle_login(struct ora_tls *tls, const uint8_t *p, size_t len)
{
    char sid[ORA_RTTY_SID_LEN + 1];
    struct rtty_session *s;
    struct ora_buf out;
    int code = ORA_RTTY_LOGIN_OK;

    if (!ora_rtty_parse_sid(p, len, sid, sizeof(sid)))
        return;

    s = session_alloc(sid);
    if (!s || !spawn_shell(s)) {
        if (s)
            session_close(s);
        code = ORA_RTTY_LOGIN_DEVICE_BUSY;
        ora_log(ORA_LOG_WARN, "rtty: cannot open terminal session");
    } else {
        ora_log(ORA_LOG_INFO, "rtty: terminal session %s opened (pid %d)",
                sid, (int)s->pid);
    }

    ora_buf_init(&out);
    if (ora_rtty_pack_login_response(&out, sid, code))
        send_frame(tls, &out);
    ora_buf_free(&out);
}

static void handle_termdata(const uint8_t *p, size_t len)
{
    char sid[ORA_RTTY_SID_LEN + 1];
    struct rtty_session *s;

    if (!ora_rtty_parse_sid(p, len, sid, sizeof(sid)))
        return;
    s = session_find(sid);
    if (!s || s->master < 0)
        return;
    if (len > ORA_RTTY_SID_LEN) {
        const uint8_t *data = p + ORA_RTTY_SID_LEN;
        size_t n = len - ORA_RTTY_SID_LEN;
        ssize_t w = write(s->master, data, n);

        if (w < 0 && errno != EAGAIN)
            ora_log(ORA_LOG_DEBUG, "rtty: pty write failed: %s", strerror(errno));
    }
}

static void handle_winsize(const uint8_t *p, size_t len)
{
    char sid[ORA_RTTY_SID_LEN + 1];
    struct rtty_session *s;
    struct winsize ws;

    if (len < ORA_RTTY_SID_LEN + 4)
        return;
    if (!ora_rtty_parse_sid(p, len, sid, sizeof(sid)))
        return;
    s = session_find(sid);
    if (!s || s->master < 0)
        return;

    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)((p[ORA_RTTY_SID_LEN] << 8) | p[ORA_RTTY_SID_LEN + 1]);
    ws.ws_row = (unsigned short)((p[ORA_RTTY_SID_LEN + 2] << 8) | p[ORA_RTTY_SID_LEN + 3]);
    if (ws.ws_col && ws.ws_row)
        ioctl(s->master, TIOCSWINSZ, &ws);
}

static void handle_logout(const uint8_t *p, size_t len)
{
    char sid[ORA_RTTY_SID_LEN + 1];
    struct rtty_session *s;

    if (!ora_rtty_parse_sid(p, len, sid, sizeof(sid)))
        return;
    s = session_find(sid);
    if (s) {
        ora_log(ORA_LOG_INFO, "rtty: terminal session %s closed", sid);
        session_close(s);
    }
}

static void handle_tunnel_add(const uint8_t *p, size_t len)
{
    struct rtty_tunnel *t = NULL;
    int i;

    if (len < 7)
        return;
    for (i = 0; i < ORA_RTTY_MAX_TUNNELS; i++) {
        if (!g_rtty.tunnels[i].active) {
            t = &g_rtty.tunnels[i];
            break;
        }
    }
    if (!t)
        return;
    memset(t, 0, sizeof(*t));
    t->active = true;
    t->fd = -1;
    t->id = p[0];
    t->addr = ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16) |
              ((uint32_t)p[3] << 8) | p[4];
    t->port = (uint16_t)((p[5] << 8) | p[6]);
    ora_log(ORA_LOG_INFO, "rtty: tunnel %u -> %u.%u.%u.%u:%u", t->id,
            (t->addr >> 24) & 0xFF, (t->addr >> 16) & 0xFF,
            (t->addr >> 8) & 0xFF, t->addr & 0xFF, t->port);
}

static void handle_tcpdata(const uint8_t *p, size_t len)
{
    struct rtty_tunnel *t;

    if (len < 17)
        return;
    t = tunnel_find(p[0]);
    if (!t)
        return;
    memcpy(t->req_id, p + 1, 16);
    if (!tunnel_connect(t)) {
        ora_log(ORA_LOG_WARN, "rtty: tunnel %u connect failed", t->id);
        return;
    }
    if (len > 17) {
        ssize_t w = write(t->fd, p + 17, len - 17);

        if (w < 0 && errno != EAGAIN)
            tunnel_close(t);
    }
}

/* ---- service loop ---- */

static bool do_register(struct ora_tls *tls, const struct ora_config *cfg,
                        const char *token)
{
    struct ora_rtty_reader rd;
    struct ora_buf out;
    char desc[96];
    bool ok = false;
    uint64_t deadline;

    snprintf(desc, sizeof(desc), "%s (gateway)", cfg->profile->model);

    ora_buf_init(&out);
    if (!ora_rtty_pack_register(&out, ORA_RTTY_MIN_VERSION, cfg->mac, desc, token) ||
        !send_frame(tls, &out)) {
        ora_buf_free(&out);
        return false;
    }
    ora_buf_free(&out);

    ora_rtty_reader_init(&rd);
    deadline = ora_now_ms() + 10000;
    while (ora_now_ms() < deadline) {
        uint8_t chunk[4096];
        int type;
        const uint8_t *payload;
        size_t plen;
        int n = ora_tls_read(tls, chunk, sizeof(chunk), 1000);

        if (n < 0)
            break;
        if (n == 0)
            continue;
        if (!ora_rtty_reader_feed(&rd, chunk, (size_t)n))
            break;
        if (ora_rtty_reader_next(&rd, &type, &payload, &plen) != 1)
            continue;
        if (type == ORA_RTTY_REGISTER) {
            int err = 1;
            char msg[128] = {0};

            ora_rtty_parse_register_response(payload, plen, &err, msg, sizeof(msg));
            ok = err == ORA_RTTY_REGISTER_OK;
            if (!ok)
                ora_log(ORA_LOG_WARN, "rtty: register rejected: %s", msg);
            else
                ora_log(ORA_LOG_INFO, "rtty: registered with controller");
        }
        ora_rtty_reader_consume(&rd);
        break;
    }
    ora_rtty_reader_free(&rd);
    return ok;
}

/* Forward PTY output and tunnel data to the controller. */
static bool pump_local(struct ora_tls *tls)
{
    struct ora_buf out;
    uint8_t buf[4096];
    bool ok = true;
    int i;

    ora_buf_init(&out);
    for (i = 0; i < ORA_RTTY_MAX_SESSIONS && ok; i++) {
        struct rtty_session *s = &g_rtty.sessions[i];
        ssize_t n;

        if (!s->active || s->master < 0)
            continue;
        n = read(s->master, buf, sizeof(buf));
        if (n > 0) {
            if (ora_rtty_pack_termdata(&out, s->sid, buf, (size_t)n))
                ok = send_frame(tls, &out);
        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            /* shell exited: tell the controller the session is gone */
            ora_log(ORA_LOG_INFO, "rtty: shell for %s exited", s->sid);
            session_close(s);
        }
    }
    for (i = 0; i < ORA_RTTY_MAX_TUNNELS && ok; i++) {
        struct rtty_tunnel *t = &g_rtty.tunnels[i];
        ssize_t n;

        if (!t->active || t->fd < 0)
            continue;
        n = read(t->fd, buf, sizeof(buf));
        if (n > 0) {
            if (ora_rtty_pack_tcpdata(&out, t->id, t->req_id, buf, (size_t)n))
                ok = send_frame(tls, &out);
        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            tunnel_close(t);
        }
    }
    ora_buf_free(&out);
    return ok;
}

static void serve(struct ora_tls *tls)
{
    struct ora_rtty_reader rd;
    uint64_t last_hb = 0;

    ora_rtty_reader_init(&rd);

    while (!g_rtty.stop) {
        uint64_t now = ora_now_ms();

        if (now - last_hb >= (uint64_t)ORA_HEARTBEAT_INTERVAL_S * 1000) {
            struct ora_buf out;

            ora_buf_init(&out);
            if (!ora_rtty_pack_heartbeat(&out, (uint32_t)ora_uptime_s()) ||
                !send_frame(tls, &out)) {
                ora_buf_free(&out);
                break;
            }
            ora_buf_free(&out);
            last_hb = now;
        }

        if (ora_tls_wait_readable(tls, 100)) {
            uint8_t chunk[8192];
            int n = ora_tls_read(tls, chunk, sizeof(chunk), 500);

            if (n < 0)
                break;
            if (n > 0 && !ora_rtty_reader_feed(&rd, chunk, (size_t)n))
                break;

            for (;;) {
                int type;
                const uint8_t *payload;
                size_t plen;
                int rv = ora_rtty_reader_next(&rd, &type, &payload, &plen);

                if (rv == 0)
                    break;
                if (rv < 0) {
                    ora_log(ORA_LOG_WARN, "rtty: oversized frame; reconnecting");
                    goto out;
                }
                switch (type) {
                case ORA_RTTY_LOGIN:
                    handle_login(tls, payload, plen);
                    break;
                case ORA_RTTY_TERMDATA:
                    handle_termdata(payload, plen);
                    break;
                case ORA_RTTY_WINSIZE:
                    handle_winsize(payload, plen);
                    break;
                case ORA_RTTY_LOGOUT:
                    handle_logout(payload, plen);
                    break;
                case ORA_RTTY_TUNNEL_ADD:
                    handle_tunnel_add(payload, plen);
                    break;
                case ORA_RTTY_TUNNEL_DELETE:
                    if (plen >= 1) {
                        struct rtty_tunnel *t = tunnel_find(payload[0]);

                        if (t)
                            tunnel_close(t);
                    }
                    break;
                case ORA_RTTY_TCPDATA:
                case ORA_RTTY_HTTPSDATA:
                    handle_tcpdata(payload, plen);
                    break;
                default:
                    break; /* ACK/CMD and unknown types are ignored */
                }
                ora_rtty_reader_consume(&rd);
            }
        }

        if (!pump_local(tls))
            break;
    }
out:
    ora_rtty_reader_free(&rd);
}

static void *rtty_thread(void *arg)
{
    (void)arg;

    while (!g_rtty.stop) {
        struct ora_tls tls;
        struct ora_rtty_settings set;
        struct ora_config cfg;

        pthread_mutex_lock(&g_rtty.lock);
        set = g_rtty.set;
        cfg = g_rtty.cfg;
        pthread_mutex_unlock(&g_rtty.lock);

        if (!ora_tls_init(&tls)) {
            sleep(ORA_RECONNECT_DELAY_S);
            continue;
        }
        if (ora_tls_connect(&tls, set.host, set.port,
                            ORA_CONNECT_TIMEOUT_S * 1000, cfg.verify_tls)) {
            if (do_register(&tls, &cfg, set.token))
                serve(&tls);
            ora_tls_close(&tls);
        }
        ora_tls_free(&tls);
        sessions_close_all();
        tunnels_close_all();

        if (!g_rtty.stop)
            sleep(ORA_RECONNECT_DELAY_S);
    }
    return NULL;
}

bool ora_rtty_start(const struct ora_config *cfg,
                    const struct ora_rtty_settings *set)
{
    bool started = false;

    pthread_mutex_lock(&g_rtty.lock);
    g_rtty.cfg = *cfg;
    g_rtty.set = *set;
    if (g_rtty.set.port <= 0)
        g_rtty.set.port = ORA_RTTY_TCP_PORT;
    if (!g_rtty.running) {
        g_rtty.stop = false;
        if (ora_thread_create(&g_rtty.thread, rtty_thread, NULL) == 0) {
            g_rtty.running = true;
            started = true;
            ora_log(ORA_LOG_INFO, "rtty: service started (%s:%d)",
                    g_rtty.set.host, g_rtty.set.port);
        } else {
            ora_log(ORA_LOG_ERR, "rtty: cannot start thread");
        }
    } else {
        started = true; /* token/settings refreshed for the next reconnect */
    }
    pthread_mutex_unlock(&g_rtty.lock);
    return started;
}

void ora_rtty_stop(void)
{
    pthread_t th;

    pthread_mutex_lock(&g_rtty.lock);
    if (!g_rtty.running) {
        pthread_mutex_unlock(&g_rtty.lock);
        return;
    }
    g_rtty.stop = true;
    th = g_rtty.thread;
    g_rtty.running = false;
    pthread_mutex_unlock(&g_rtty.lock);

    pthread_join(th, NULL);
    sessions_close_all();
    tunnels_close_all();
    ora_log(ORA_LOG_INFO, "rtty: service stopped");
}

bool ora_rtty_running(void)
{
    bool r;

    pthread_mutex_lock(&g_rtty.lock);
    r = g_rtty.running;
    pthread_mutex_unlock(&g_rtty.lock);
    return r;
}
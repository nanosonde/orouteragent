/* orouteragent - mbedTLS wrapper implementation */
#include "tls.h"
#include "../protocol/constants.h"
#include "../util.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

bool ora_tls_init(struct ora_tls *t)
{
    memset(t, 0, sizeof(*t));
    mbedtls_net_init(&t->net);
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_x509_crt_init(&t->ca);
    mbedtls_ctr_drbg_init(&t->ctr_drbg);
    mbedtls_entropy_init(&t->entropy);

    const char *pers = "orouteragent";
    if (mbedtls_ctr_drbg_seed(&t->ctr_drbg, mbedtls_entropy_func, &t->entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
        ora_log(ORA_LOG_ERR, "ctr_drbg_seed failed");
        return false;
    }
    return true;
}

static int resolve_connect(const char *host, int port, int timeout_ms)
{
    char portstr[8];
    struct addrinfo hints, *res = NULL, *ai;
    int fd = -1;
    int flags, rv;

    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;

    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        /* non-blocking connect with timeout */
        flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        rv = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rv == 0)
            goto done;
        if (errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
            int ms = timeout_ms ? timeout_ms : ORA_CONNECT_TIMEOUT_S * 1000;
            if (poll(&pfd, 1, ms) > 0) {
                int err = 0;
                socklen_t elen = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0)
                    goto done;
            }
        }
        close(fd);
        fd = -1;
    }
done:
    freeaddrinfo(res);
    if (fd >= 0) {
        flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    return fd;
}

bool ora_tls_connect(struct ora_tls *t, const char *host, int port,
                     int timeout_ms, bool verify)
{
    int rv;
    int fd = resolve_connect(host, port, timeout_ms);

    if (fd < 0) {
        ora_log(ORA_LOG_WARN, "connect %s:%d failed: %s", host, port,
                strerror(errno));
        return false;
    }
    t->net.fd = fd;
    snprintf(t->peer, sizeof(t->peer), "%s", host);
    t->port = port;

    /* Bound how long a write can block: a peer that stops reading must
     * not pin the channel lock (and with it the whole session) forever. */
    {
        struct timeval tv = { .tv_sec = ORA_CONNECT_TIMEOUT_S, .tv_usec = 0 };

        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    /* The controller presents CN=localhost; set SNI so a v6 controller
     * behind a reverse proxy still routes. */
    if (mbedtls_ssl_set_hostname(&t->ssl, ORA_TLS_SERVER_HOSTNAME) != 0)
        goto fail;

    if ((rv = mbedtls_ssl_config_defaults(&t->conf,
            MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM,
            MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
        goto fail;

    if (verify) {
        if (mbedtls_x509_crt_parse_file(&t->ca, "/etc/ssl/certs/ca-certificates.crt") != 0)
            if (mbedtls_x509_crt_parse_file(&t->ca, "/etc/ssl/cert.pem") != 0) {
                ora_log(ORA_LOG_WARN, "verify_tls=1 but no CA bundle found; connection will fail");
            }
        mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
        mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->ctr_drbg);
    mbedtls_ssl_conf_read_timeout(&t->conf, 1000);

    if (mbedtls_ssl_setup(&t->ssl, &t->conf) != 0)
        goto fail;
    mbedtls_ssl_set_bio(&t->ssl, &t->net, mbedtls_net_send, mbedtls_net_recv,
                        mbedtls_net_recv_timeout);

    while ((rv = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if (rv == MBEDTLS_ERR_SSL_WANT_READ || rv == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        ora_log(ORA_LOG_WARN, "TLS handshake with %s:%d failed: -0x%x", host, port, -rv);
        goto fail;
    }
    t->connected = true;
    return true;
fail:
    mbedtls_net_free(&t->net);
    return false;
}

bool ora_tcp_connect(struct ora_tls *t, const char *host, int port, int timeout_ms)
{
    int fd = resolve_connect(host, port, timeout_ms);

    if (fd < 0)
        return false;
    t->net.fd = fd;
    t->connected = true; /* plain TCP; ssl not used */
    return true;
}

int ora_tls_read(struct ora_tls *t, void *buf, size_t len, int timeout_ms)
{
    uint64_t deadline;
    int rv;

    if (!t->connected)
        return -1;
    deadline = ora_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 1000);
    for (;;) {
        rv = mbedtls_ssl_read(&t->ssl, buf, len);
        if (rv == MBEDTLS_ERR_SSL_WANT_READ || rv == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (ora_now_ms() >= deadline)
                return 0;
            continue;
        }
        if (rv == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
            rv == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE)
            return -1;
        if (rv == MBEDTLS_ERR_SSL_TIMEOUT)
            return 0;
        return rv; /* >0 bytes, <=0 error (0 means peer closed) */
    }
}

int ora_tls_write(struct ora_tls *t, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t off = 0;
    uint64_t deadline;

    if (!t->connected)
        return -1;
    /* A stalled peer must fail the write rather than spin here: this
     * runs with the management channel lock held. */
    deadline = ora_now_ms() + (uint64_t)ORA_CONNECT_TIMEOUT_S * 1000;
    while (off < len) {
        int rv = mbedtls_ssl_write(&t->ssl, p + off, len - off);

        if (rv == MBEDTLS_ERR_SSL_WANT_READ || rv == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (ora_now_ms() >= deadline) {
                ora_log(ORA_LOG_WARN, "TLS write to %s:%d timed out",
                        t->peer, t->port);
                return -1;
            }
            continue;
        }
        if (rv <= 0)
            return -1;
        off += (size_t)rv;
    }
    return (int)off;
}

void ora_tls_set_keepalive(struct ora_tls *t, int idle_s, int intvl_s, int cnt)
{
    int one = 1;

    if (t->net.fd < 0)
        return;
    setsockopt(t->net.fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
    setsockopt(t->net.fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_s, sizeof(idle_s));
    setsockopt(t->net.fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl_s, sizeof(intvl_s));
    setsockopt(t->net.fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
}

void ora_tls_close(struct ora_tls *t)
{
    if (t->connected) {
        /* best-effort close_notify (ignore errors) */
        while (mbedtls_ssl_close_notify(&t->ssl) == MBEDTLS_ERR_SSL_WANT_WRITE)
            ;
    }
    mbedtls_net_free(&t->net);
    t->connected = false;
}

void ora_tls_free(struct ora_tls *t)
{
    mbedtls_net_free(&t->net);
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_x509_crt_free(&t->ca);
    mbedtls_ctr_drbg_free(&t->ctr_drbg);
    mbedtls_entropy_free(&t->entropy);
    t->connected = false;
}

bool ora_tls_wait_readable(struct ora_tls *t, int timeout_ms)
{
    struct pollfd pfd;

    if (t->net.fd < 0)
        return false;
    pfd.fd = t->net.fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    return poll(&pfd, 1, timeout_ms) > 0;
}
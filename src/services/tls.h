/* orouteragent - mbedTLS TCP/TLS client wrapper */
#ifndef ORA_TLS_H
#define ORA_TLS_H

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One TLS client connection. The agent is always the client; the
 * controller is the TLS server (SNI "localhost", vendor cert CN=localhost,
 * verification off unless verify_tls=1). */
struct ora_tls {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    char peer[64];
    int port;
    bool connected;
};

/* Initialize contexts (must be called once per connection). */
bool ora_tls_init(struct ora_tls *t);

/* Connect + TLS handshake. timeout_ms = 0 -> default.
 * verify: false => accept the vendor certificate. */
bool ora_tls_connect(struct ora_tls *t, const char *host, int port,
                     int timeout_ms, bool verify);

/* Plain-TCP variant (for capture transfer channel 29815, which per the
 * reference is TLS as well; kept for flexibility). */
bool ora_tcp_connect(struct ora_tls *t, const char *host, int port,
                     int timeout_ms);

/* Blocking-with-timeout IO. Return: >0 bytes, 0 clean EOF/timeout retry,
 * negative error (connection dead). */
int ora_tls_read(struct ora_tls *t, void *buf, size_t len, int timeout_ms);
int ora_tls_write(struct ora_tls *t, const void *buf, size_t len);

/* Setsockopt-level keepalive. */
void ora_tls_set_keepalive(struct ora_tls *t, int idle_s, int intvl_s, int cnt);

void ora_tls_close(struct ora_tls *t);
void ora_tls_free(struct ora_tls *t);

/* Wait until readable or timeout_ms elapsed. true = readable. */
bool ora_tls_wait_readable(struct ora_tls *t, int timeout_ms);

#endif
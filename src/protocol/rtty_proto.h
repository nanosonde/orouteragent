/* orouteragent - RTTY (remote TTY) wire protocol
 *
 * The controller is the RTTY server (TLS 29816); the device connects in,
 * registers, and relays shell I/O.
 *
 * Frame variants (the type byte selects the header):
 *   V1: type(1) + length(2 BE) + payload
 *   V2: type(1) + length(4 BE) + payload
 */
#ifndef ORA_RTTY_PROTO_H
#define ORA_RTTY_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buf.h"

enum {
    ORA_RTTY_REGISTER = 0,
    ORA_RTTY_LOGIN = 1,
    ORA_RTTY_LOGOUT = 2,
    ORA_RTTY_TERMDATA = 3,
    ORA_RTTY_WINSIZE = 4,
    ORA_RTTY_CMD = 5,
    ORA_RTTY_HEARTBEAT = 6,
    ORA_RTTY_ACK = 9,
    ORA_RTTY_DISCONNECT_EXCEPTION = 10,
    ORA_RTTY_DISCONNECT_NORMALLY = 11,
    ORA_RTTY_TCPDATA = 20,
    ORA_RTTY_HTTPSDATA = 22,
    ORA_RTTY_SSHDATA = 31,
    ORA_RTTY_TELNETDATA = 32,
    ORA_RTTY_TUNNEL_ADD = 40,
    ORA_RTTY_TUNNEL_DELETE = 41,
    ORA_RTTY_STANDALONE_AUTH = 42,
};

/* Minimum protocol version the controller accepts in REGISTER. */
#define ORA_RTTY_MIN_VERSION 3
/* Session ids are always 32 ASCII hex chars (UUID with hyphens stripped). */
#define ORA_RTTY_SID_LEN 32
/* Largest frame payload accepted from the controller. */
#define ORA_RTTY_MAX_PAYLOAD (1024 * 1024)

/* REGISTER response codes */
#define ORA_RTTY_REGISTER_OK 0
/* LOGIN response codes */
#define ORA_RTTY_LOGIN_OK 0
#define ORA_RTTY_LOGIN_DEVICE_BUSY 1

bool ora_rtty_is_v1(int msg_type);

/* Frame writers: append a complete frame to @out. */
bool ora_rtty_pack(struct ora_buf *out, int msg_type,
                   const void *payload, size_t len);

/* REGISTER: version(1) + devid\0 + description\0 + token\0
 * The controller splits the tail on NUL and requires EXACTLY four
 * segments, so no extra trailing NUL may be emitted. */
bool ora_rtty_pack_register(struct ora_buf *out, int version, const char *devid,
                            const char *description, const char *token);

/* LOGIN reply: sid(32) + code(1) */
bool ora_rtty_pack_login_response(struct ora_buf *out, const char *sid, int code);

/* TERMDATA: sid(32) + raw data */
bool ora_rtty_pack_termdata(struct ora_buf *out, const char *sid,
                            const void *data, size_t len);

/* HEARTBEAT: uptime(4 BE). An empty payload makes the controller throw
 * and tear the channel down. */
bool ora_rtty_pack_heartbeat(struct ora_buf *out, uint32_t uptime);

/* TCPDATA: tunnelId(1) + requestId(16) + data */
bool ora_rtty_pack_tcpdata(struct ora_buf *out, uint8_t tunnel_id,
                           const uint8_t req_id[16], const void *data, size_t len);

/* Incremental frame reader over a byte stream. */
struct ora_rtty_reader {
    struct ora_buf in;
    int type;
    uint32_t declared;
    bool have_header;
};

void ora_rtty_reader_init(struct ora_rtty_reader *r);
void ora_rtty_reader_free(struct ora_rtty_reader *r);
bool ora_rtty_reader_feed(struct ora_rtty_reader *r, const void *data, size_t n);

/* 1 = frame ready (payload borrowed until consume), 0 = need more data,
 * -1 = protocol error. */
int ora_rtty_reader_next(struct ora_rtty_reader *r, int *type,
                         const uint8_t **payload, size_t *len);
void ora_rtty_reader_consume(struct ora_rtty_reader *r);

/* REGISTER response: err(1) + msg. */
bool ora_rtty_parse_register_response(const uint8_t *payload, size_t len,
                                      int *err, char *msg, size_t msgsz);

/* Extract the session id prefix of a payload. */
bool ora_rtty_parse_sid(const uint8_t *payload, size_t len, char *sid, size_t sidsz);

#endif
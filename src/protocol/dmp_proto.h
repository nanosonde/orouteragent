/* orouteragent - Device Monitor Platform (DMP) protobuf codec
 *
 * Backs the controller's Tools -> Network Check (TLS 29817). Messages
 * are protobuf:
 *
 *   MonitorMessageHeader {
 *     1 mac (raw 6 bytes, hyphens stripped)  2 token       3 path
 *     4 version                              5 msgType     6 seq
 *     7 devType                              8 errorCode   9 needReply
 *     10 epochMs (fixed64)                   11 contentType
 *   }
 *   MonitorMessage { 1 header, 2 data }
 */
#ifndef ORA_DMP_PROTO_H
#define ORA_DMP_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buf.h"

enum {
    ORA_DMP_MSG_EMPTY = 1,
    ORA_DMP_MSG_COMPONENT_LIST = 2,
    ORA_DMP_MSG_JSON_COMPONENT_LIST = 3,
};

struct ora_dmp_header {
    uint8_t mac[6];
    char token[128];
    char path[128];
    int64_t version;
    int64_t msg_type;
    int64_t seq;
    int64_t dev_type;
    int64_t error_code;
    int64_t need_reply;
    uint64_t epoch_ms;
    int64_t content_type;
};

/* Encode a complete MonitorMessage (header + optional data) into @out. */
bool ora_dmp_encode(struct ora_buf *out, const struct ora_dmp_header *hdr,
                    const void *data, size_t data_len);

/* Decode a MonitorMessage. @data points into @payload (borrowed). */
bool ora_dmp_decode(const uint8_t *payload, size_t len,
                    struct ora_dmp_header *hdr,
                    const uint8_t **data, size_t *data_len);

/* Low-level helpers (exposed for tests). */
bool ora_pb_put_varint(struct ora_buf *b, uint32_t field, uint64_t value);
bool ora_pb_put_bytes(struct ora_buf *b, uint32_t field,
                      const void *data, size_t len);
bool ora_pb_put_fixed64(struct ora_buf *b, uint32_t field, uint64_t value);

/* Read a varint at @*off; false when truncated. */
bool ora_pb_get_varint(const uint8_t *buf, size_t len, size_t *off, uint64_t *out);

#endif
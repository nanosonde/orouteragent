/* orouteragent - ECSP JSON message envelope codec (json-c) */
#ifndef ORA_MESSAGE_H
#define ORA_MESSAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <json-c/json.h>

#include "../util.h"
#include "buf.h"
#include "constants.h"

/* Header (top-level object fields of every ECSP message):
 *   mac       hyphenated uppercase AA-BB-CC-DD-EE-FF
 *   type      message type enum
 *   device    "gateway"
 *   version   "2.2.0"
 *   verCap    3
 *   timestamp epoch milliseconds
 *   seq       incrementing sequence
 *   error     error code (0)
 *   dest      controller id (required in NOTIFY)
 */
struct ora_header {
    char mac[24];
    int64_t type;
    char device[16];
    char version[16];
    int64_t ver_cap;
    int64_t timestamp;
    int64_t seq;
    int64_t error;
    char dest[40];
    bool have_dest;
};

struct ora_message {
    struct ora_header hdr;
    json_object *body; /* owned; NULL if absent */
};

/* Sequence counter: process-wide, monotonically increasing. */
void ora_msg_seq_set(uint64_t start);
uint64_t ora_msg_seq_next(void);

/* Build the canonical header into a fresh json_object (caller owns). */
json_object *ora_header_to_json(const struct ora_header *h);

/* Encode a complete framed message: header (+ body if non-NULL).
 * Compact JSON (no whitespace). Returns false on failure (OOM/bad args). */
bool ora_msg_encode(struct ora_buf *out, const struct ora_header *h,
                    const json_object *body);

/* Decode a complete message (header + body). @msg->body is owned by the
 * caller; free with ora_msg_free(). Returns false on malformed input. */
bool ora_msg_decode(struct ora_message *msg, const uint8_t *payload, size_t len);

void ora_msg_free(struct ora_message *msg);

/* Fill a header with the device's current identity/defaults. */
void ora_header_init(struct ora_header *h, const char *mac, int64_t type,
                     const char *dest /* optional */);

/* Helper: fetch a required string/int from a json object without
 * crashing on wrong types (returns NULL / specified default). */
const char *ora_json_get_str(json_object *o, const char *key, const char *dflt);
int64_t ora_json_get_int(json_object *o, const char *key, int64_t dflt);
bool ora_json_get_bool(json_object *o, const char *key, bool dflt);
json_object *ora_json_get_obj(json_object *o, const char *key);

#endif
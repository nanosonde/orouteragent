/* orouteragent - management channel service (TLS 29814)
 *
 * Runs the 10-step adoption handshake, then the steady-state loop:
 * INFORM every N seconds, SET ack&store, GET echo, FORGET, UPGRADE
 * (declined), NOTIFY, FILE_TRANSFER_RESPONSE_V2.
 *
 * The handshake is event-driven and single-threaded. After the management
 * channel is published, cross-thread senders serialize access internally;
 * callers must not retain or write the underlying TLS connection directly.
 */
#ifndef ORA_MANAGE_H
#define ORA_MANAGE_H

#include <json-c/json.h>
#include <stdbool.h>

#include "../config.h"
#include "../state.h"

/* Outcome of a management session. */
enum ora_manage_result {
    ORA_MANAGE_OK = 0,        /* controller closed / FORGET -> rediscover */
    ORA_MANAGE_RECONNECT,    /* transient error -> retry with backoff */
    ORA_MANAGE_FATAL,        /* unrecoverable (config/state errors) */
};

/* Run one full management session against the configured controller.
 * Blocks until the session ends (socket closed, FORGET, or *stop).
 * Handles the adoption handshake if not yet adopted. */
enum ora_manage_result ora_manage_run(const struct ora_config *cfg,
                                      struct ora_state *st,
                                      volatile bool *stop);

/* --- handshake steps (exposed for tests) --- */

/* Build PRE_CONNECT_INFO body. rebuild=1 when re-adopting an already
 * connected controller (Force Provision). */
json_object *ora_manage_pre_connect_body(const struct ora_config *cfg,
                                         struct ora_state *st, bool rebuild);

/* Build DEVICE_NEGOTIATION body. */
json_object *ora_manage_negotiation_body(const struct ora_config *cfg,
                                         struct ora_state *st);

/* Compute the DEVICE_VERIFY_INFO auth from credentials + random key. */
bool ora_manage_device_verify_info(const char *username, const char *password,
                                   const char *random_key,
                                   char *auth_out, size_t outsz);

/* --- cross-thread senders on the management channel ---
 *
 * The capture/transfer services run on their own threads but must emit
 * on the management connection (29814). Both take the channel lock.
 * They fail quietly when the channel is down. */

/* NOTIFY subject for the file-transfer/capture-ready notification. */
#define ORA_NOTIFY_SUBJECT_FILE_TRANSFER 6

/* Send NOTIFY_REQUEST (V1 type 80 - V2 lands on a topic nobody listens
 * to). Body: {nid, sub, nre, ctnt}. @content is borrowed. */
bool ora_manage_send_notify(int subject, json_object *content);

/* Send a FILE_TRANSFER_RESPONSE_V2 frame with @body, echoing @seq. */
bool ora_manage_send_file_transfer(json_object *body, int64_t seq);

/* True while a management session is connected. */
bool ora_manage_connected(void);

#endif
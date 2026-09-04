/* orouteragent - persistent adoption state (state.json) */
#ifndef ORA_STATE_H
#define ORA_STATE_H

#include <json-c/json.h>

#include <stdbool.h>
#include <stddef.h>

/* Everything the agent must remember across restarts. Stored as JSON,
 * written atomically (write tmp + rename). Initial state has an empty
 * controller ID and adopted=false; successful initial sync persists both,
 * while FORGET clears them. Device Account fields are credentials and the
 * state file must remain readable only by the service account. */
struct ora_state {
    /* Adoption */
    char omadac_id[40];         /* controller ID once known ("" = unknown) */
    bool adopted;               /* completed handshake & initial sync */
    char device_username[64];   /* captured Device Account (from SYSTEM_*/
    char device_password[64];   /* NEGOTIATION userAccount) */
    bool device_password_is_md5;/* stored value is already MD5_UPPER */
    int64_t config_version;     /* last acknowledged config version */
    char capture_file[256];     /* active capture target ("" = none) */

    /* SET blobs stored verbatim (key -> JSON string), capped by
     * max_config_kb. */
    json_object *set_blobs;     /* object: key -> string(JSON) */
    size_t blob_bytes;

    /* Rolling history of recent SET responses (cfgResults), newest last,
     * capped at ORA_CFG_RESULT_MAX. */
    json_object *cfg_results;   /* array of SET_RESPONSE bodies */
    json_object *last_cfg_result; /* newest entry, or NULL */

    /* uptime anchor for stable synthetic values */
    int64_t boot_epoch;

    char path[256];
    size_t max_config_kb;
};

void ora_state_init(struct ora_state *st, const char *path, size_t max_config_kb);
bool ora_state_load(struct ora_state *st);
bool ora_state_save(const struct ora_state *st);

/* Remember/clear the controller identity. */
void ora_state_set_omadac(struct ora_state *st, const char *omadac_id);
void ora_state_set_adopted(struct ora_state *st, bool adopted);

/* Store a SET blob (takes ownership of nothing; copies the compact
 * string). Returns false when the store is full or on OOM. */
bool ora_state_set_blob(struct ora_state *st, const char *key, const char *json_str);
/* Drop a blob (FORGET/config reset). */
void ora_state_clear_blobs(struct ora_state *st);

/* Record a SET response in the rolling history (cfgResults, cap 10).
 * @resp is borrowed. */
#define ORA_CFG_RESULT_MAX 10
void ora_state_record_cfg_result(struct ora_state *st, json_object *resp);

/* Device Account capture (SYSTEM_NEGOTIATION userAccount). */
void ora_state_set_account(struct ora_state *st, const char *user, const char *pass,
                           bool password_is_md5);

void ora_state_free(struct ora_state *st);

#endif
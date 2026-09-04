/* orouteragent - controller configuration transform registry */
#ifndef ORA_CONFIG_REGISTRY_H
#define ORA_CONFIG_REGISTRY_H

#include <json-c/json.h>

#include <stdbool.h>
#include <stddef.h>

#include "../config.h"

enum ora_config_transform_status {
    ORA_CONFIG_TRANSFORM_APPLIED,
    ORA_CONFIG_TRANSFORM_STAGED,
    ORA_CONFIG_TRANSFORM_LIVE_COMMAND,
    ORA_CONFIG_TRANSFORM_STUB,
    ORA_CONFIG_TRANSFORM_DECLINED,
};

/* One controller key. @apply and @export_native are set only for keys with a
 * native transform and are reached exclusively through the UCI boundary. */
struct ora_config_transform {
    const char *key;
    const char *ack_key;
    const char *ack_error_key;
    unsigned domains;
    enum ora_config_transform_status status;
    bool (*validate)(const struct ora_config *cfg, json_object *value,
                     char *error, size_t error_size);
    bool (*apply)(const struct ora_config *cfg, json_object *value,
                  char *error, size_t error_size);
    json_object *(*export_native)(const struct ora_config *cfg);
};

/* Look up a controller key. Unknown future keys deliberately return NULL. */
const struct ora_config_transform *ora_config_transform_find(const char *key);

/* Validate a known controller value. Unknown keys remain protocol-tolerant. */
bool ora_config_transform_validate(const struct ora_config_transform *transform,
                                   const struct ora_config *cfg,
                                   json_object *value, char *error,
                                   size_t error_size);

/* The wire acknowledgement contract, including documented aliases/casing. */
const char *ora_config_transform_ack_key(const char *key);
const char *ora_config_transform_ack_error_key(const char *key);

#endif

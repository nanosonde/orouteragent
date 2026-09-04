/* orouteragent - native UCI configuration transform boundary */
#ifndef ORA_CONFIG_UCI_H
#define ORA_CONFIG_UCI_H

#include <json-c/json.h>

#include <stdbool.h>
#include <stddef.h>

#include "../config.h"
#include "registry.h"

/* A transform is UCI-owned only after every required domain is opted in. */
bool ora_uci_config_transform_is_managed(const struct ora_config *cfg,
                                         const struct ora_config_transform *transform);

/* Apply one owned transform. The caller supplies a bounded error buffer. */
bool ora_uci_config_apply(const struct ora_config *cfg,
                          const struct ora_config_transform *transform,
                          json_object *value, char *error, size_t error_size);

/* Return the native representation for an owned transform, or NULL when the
 * transform is not implemented or the native package cannot be read. */
json_object *ora_uci_config_export(const struct ora_config *cfg, const char *key);

#endif

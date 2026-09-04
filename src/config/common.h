/* orouteragent - helpers shared by the configuration domains */
#ifndef ORA_CONFIG_COMMON_H
#define ORA_CONFIG_COMMON_H

#include <json-c/json.h>

#include <stdbool.h>
#include <stddef.h>

/* Error text is returned to the controller, so it must stay free of payload
 * values that may carry credentials. */
void ora_config_set_error(char *error, size_t error_size, const char *fmt, ...);

bool ora_config_require_object(json_object *value, char *error, size_t error_size);
bool ora_config_optional_array(json_object *value, const char *name, char *error,
                               size_t error_size);
bool ora_config_get_bool(json_object *object, const char *name, bool dflt,
                         bool *out);
bool ora_config_valid_ip4(const char *value);
bool ora_config_valid_port_range(const char *value);

#endif

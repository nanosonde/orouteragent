/* orouteragent - helpers shared by the configuration domains */
#include "common.h"
#include "../util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ora_config_set_error(char *error, size_t error_size, const char *fmt, ...)
{
    va_list ap;

    if (!error || !error_size)
        return;
    va_start(ap, fmt);
    vsnprintf(error, error_size, fmt, ap);
    va_end(ap);
}

bool ora_config_require_object(json_object *value, char *error, size_t error_size)
{
    if (value && json_object_is_type(value, json_type_object))
        return true;
    ora_config_set_error(error, error_size, "value must be an object");
    return false;
}

bool ora_config_optional_array(json_object *value, const char *name, char *error,
                               size_t error_size)
{
    json_object *member;

    if (!json_object_object_get_ex(value, name, &member))
        return true;
    if (json_object_is_type(member, json_type_array))
        return true;
    ora_config_set_error(error, error_size, "%s must be an array", name);
    return false;
}

bool ora_config_get_bool(json_object *object, const char *name, bool dflt,
                         bool *out)
{
    json_object *value;
    const char *text;

    if (!out)
        return false;
    if (!json_object_object_get_ex(object, name, &value)) {
        *out = dflt;
        return true;
    }
    if (json_object_is_type(value, json_type_boolean) ||
        json_object_is_type(value, json_type_int)) {
        *out = json_object_get_boolean(value);
        return true;
    }
    if (!json_object_is_type(value, json_type_string))
        return false;
    text = json_object_get_string(value);
    if (!strcmp(text, "1") || !strcasecmp(text, "true") ||
        !strcasecmp(text, "on")) {
        *out = true;
        return true;
    }
    if (!strcmp(text, "0") || !strcasecmp(text, "false") ||
        !strcasecmp(text, "off")) {
        *out = false;
        return true;
    }
    return false;
}

bool ora_config_valid_ip4(const char *value)
{
    uint32_t ignored;

    return value && ora_ip4_parse(value, &ignored);
}

bool ora_config_valid_port_range(const char *value)
{
    char *end;
    long first;
    long last;

    if (!value || !*value)
        return false;
    first = strtol(value, &end, 10);
    if (end == value || first < 1 || first > 65535)
        return false;
    if (!*end)
        return true;
    if (*end++ != '-')
        return false;
    last = strtol(end, &end, 10);
    return *end == '\0' && last >= first && last <= 65535;
}

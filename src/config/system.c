/* orouteragent - system domain (UCI system) configuration transforms */
#include "common.h"
#include "domains.h"
#include "uci_util.h"
#include "../protocol/message.h"

#include <stdio.h>
#include <string.h>

static bool validate_time_setting(const struct ora_config *cfg, json_object *value,
                                  char *error, size_t error_size)
{
    (void)cfg;
    return ora_config_require_object(value, error, error_size);
}

static bool apply_time_setting(const struct ora_config *cfg, json_object *value,
                               char *error, size_t error_size)
{
    struct uci_context *context = NULL;
    struct uci_package *package = NULL;
    struct uci_section *system;
    struct uci_section *ntp;
    json_object *servers;
    json_object *server_list = NULL;
    const char *timezone;
    bool ok = false;

    (void)cfg;
    context = uci_alloc_context();
    if (!context || uci_load(context, "system", &package) != UCI_OK || !package ||
        !(system = ora_uci_find_section(package, "system"))) {
        ora_config_set_error(error, error_size, "cannot load UCI package system");
        goto out;
    }
    ntp = ora_uci_find_section(package, "timeserver");
    if (!ntp) {
        ntp = ora_uci_add_named_section(context, package, "timeserver", "ntp");
        if (!ntp) {
            ora_config_set_error(error, error_size, "cannot create NTP UCI section");
            goto out;
        }
    }
    timezone = ora_json_get_str(value, "timeZone", NULL);
    if (timezone && !ora_uci_set_option(context, package, system, "zonename", timezone)) {
        ora_config_set_error(error, error_size, "cannot stage timezone");
        goto out;
    }
    if (json_object_object_get(value, "ntpEnable") &&
        !ora_uci_set_option(context, package, ntp, "enabled",
                            ora_json_get_bool(value, "ntpEnable", false) ? "1" : "0")) {
        ora_config_set_error(error, error_size, "cannot stage NTP enable state");
        goto out;
    }
    if (json_object_object_get_ex(value, "ntpServers", &servers)) {
        server_list = json_object_get(servers);
    } else if (ora_json_get_str(value, "ntpServer1", NULL) ||
               ora_json_get_str(value, "ntpServer2", NULL)) {
        const char *server;

        server_list = json_object_new_array();
        if ((server = ora_json_get_str(value, "ntpServer1", NULL)))
            json_object_array_add(server_list, json_object_new_string(server));
        if ((server = ora_json_get_str(value, "ntpServer2", NULL)))
            json_object_array_add(server_list, json_object_new_string(server));
    }
    if (server_list && !ora_uci_replace_list(context, package, ntp, "server", server_list)) {
        ora_config_set_error(error, error_size, "cannot stage NTP servers");
        goto out;
    }
    if (uci_commit(context, &package, false) != UCI_OK ||
        !ora_uci_reload_service("sysntpd")) {
        ora_config_set_error(error, error_size, "cannot apply UCI time settings");
        goto out;
    }
    ok = true;
out:
    if (server_list)
        json_object_put(server_list);
    if (context)
        uci_free_context(context);
    return ok;
}

static json_object *export_time_setting(const struct ora_config *cfg)
{
    struct uci_context *context = NULL;
    struct uci_package *package = NULL;
    struct uci_section *system;
    struct uci_section *ntp;
    struct uci_option *servers;
    struct uci_element *server;
    const char *timezone;
    const char *enabled;
    json_object *result = NULL;
    json_object *server_list;

    (void)cfg;
    context = uci_alloc_context();
    if (!context || uci_load(context, "system", &package) != UCI_OK || !package ||
        !(system = ora_uci_find_section(package, "system")))
        goto out;
    result = json_object_new_object();
    timezone = ora_uci_option_value(context, system, "zonename");
    if (timezone)
        json_object_object_add(result, "timeZone", json_object_new_string(timezone));
    ntp = ora_uci_find_section(package, "timeserver");
    if (!ntp)
        goto out;
    enabled = ora_uci_option_value(context, ntp, "enabled");
    json_object_object_add(result, "ntpEnable", json_object_new_boolean(
        !enabled || strcmp(enabled, "0") != 0));
    server_list = json_object_new_array();
    servers = uci_lookup_option(context, ntp, "server");
    if (servers && servers->type == UCI_TYPE_LIST) {
        uci_foreach_element(&servers->v.list, server)
            json_object_array_add(server_list, json_object_new_string(server->name));
    } else if (servers && servers->type == UCI_TYPE_STRING) {
        json_object_array_add(server_list, json_object_new_string(servers->v.string));
    }
    if (json_object_array_length(server_list) > 0)
        json_object_object_add(result, "ntpServers", server_list);
    else
        json_object_put(server_list);
out:
    if (context)
        uci_free_context(context);
    return result;
}

const struct ora_config_transform ora_config_transforms_system[] = {
    { "timeSetting", "timeSetting", "errcode", ORA_CONFIG_DOMAIN_SYSTEM,
            ORA_CONFIG_TRANSFORM_APPLIED, validate_time_setting, apply_time_setting,
            export_time_setting },
};

const size_t ora_config_transforms_system_count =
    sizeof(ora_config_transforms_system) / sizeof(ora_config_transforms_system[0]);

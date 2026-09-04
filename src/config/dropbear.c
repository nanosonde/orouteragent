/* orouteragent - dropbear domain (SSH service) configuration transforms */
#include "common.h"
#include "domains.h"
#include "uci_util.h"
#include "../util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool validate_ssh(const struct ora_config *cfg, json_object *value,
                         char *error, size_t error_size)
{
    json_object *port;
    json_object *global;

    (void)cfg;
    if (!ora_config_require_object(value, error, error_size))
        return false;
    if (json_object_object_get_ex(value, "port", &port) &&
        (!json_object_is_type(port, json_type_int) ||
         json_object_get_int(port) < 1 || json_object_get_int(port) > 65535)) {
        ora_config_set_error(error, error_size,
                             "port must be an integer from 1 through 65535");
        return false;
    }
    if (json_object_object_get_ex(value, "global", &global) &&
        !json_object_is_type(global, json_type_boolean) &&
        !json_object_is_type(global, json_type_int)) {
        ora_config_set_error(error, error_size,
                             "global must be a boolean or integer");
        return false;
    }
    return true;
}

static bool restore(struct uci_context *context, struct uci_package *package,
                    struct uci_section *section, bool created,
                    const char *old_port, const char *old_enable)
{
    struct uci_ptr pointer = { .p = package, .s = section };

    if (created) {
        if (uci_delete(context, &pointer) != UCI_OK)
            return false;
    } else {
        if (old_port) {
            if (!ora_uci_set_option(context, package, section, "Port", old_port))
                return false;
        } else if (!ora_uci_delete_option(context, package, section, "Port")) {
            return false;
        }
        if (old_enable) {
            if (!ora_uci_set_option(context, package, section, "enable", old_enable))
                return false;
        } else if (!ora_uci_delete_option(context, package, section, "enable")) {
            return false;
        }
    }
    return uci_commit(context, &package, false) == UCI_OK;
}

static bool apply_ssh(const struct ora_config *cfg, json_object *value,
                      char *error, size_t error_size)
{
    struct uci_context *context = NULL;
    struct uci_package *package = NULL;
    struct uci_section *section;
    const char *old_port;
    const char *old_enable;
    char old_port_copy[16] = {0};
    char old_enable_copy[8] = {0};
    char port[8];
    char enable[2];
    bool created = false;
    bool ok = false;
    json_object *member;

    (void)cfg;
    context = uci_alloc_context();
    if (!context || uci_load(context, "dropbear", &package) != UCI_OK || !package) {
        ora_config_set_error(error, error_size, "cannot load UCI package dropbear");
        goto out;
    }
    section = ora_uci_find_section(package, "dropbear");
    if (!section) {
        if (uci_add_section(context, package, "dropbear", &section) != UCI_OK) {
            ora_config_set_error(error, error_size,
                                 "cannot create Dropbear UCI section");
            goto out;
        }
        created = true;
    }

    old_port = ora_uci_option_value(context, section, "Port");
    old_enable = ora_uci_option_value(context, section, "enable");
    if (old_port)
        snprintf(old_port_copy, sizeof(old_port_copy), "%s", old_port);
    if (old_enable)
        snprintf(old_enable_copy, sizeof(old_enable_copy), "%s", old_enable);

    if (json_object_object_get_ex(value, "port", &member)) {
        snprintf(port, sizeof(port), "%d", json_object_get_int(member));
        if (!ora_uci_set_option(context, package, section, "Port", port)) {
            ora_config_set_error(error, error_size, "cannot set Dropbear port");
            goto out;
        }
    }
    if (json_object_object_get_ex(value, "global", &member)) {
        snprintf(enable, sizeof(enable), "%d",
                 json_object_get_boolean(member) ? 1 : 0);
        if (!ora_uci_set_option(context, package, section, "enable", enable)) {
            ora_config_set_error(error, error_size,
                                 "cannot set Dropbear enable state");
            goto out;
        }
    }
    if (uci_commit(context, &package, false) != UCI_OK) {
        ora_config_set_error(error, error_size,
                             "cannot commit UCI package dropbear");
        goto out;
    }
    if (!ora_uci_reload_service("dropbear")) {
        if (!restore(context, package, section, created,
                     old_port_copy[0] ? old_port_copy : NULL,
                     old_enable_copy[0] ? old_enable_copy : NULL)) {
            ora_log(ORA_LOG_ERR,
                    "failed to restore Dropbear UCI settings after reload failure");
        } else {
            (void)ora_uci_reload_service("dropbear");
        }
        ora_config_set_error(error, error_size, "cannot reload Dropbear service");
        goto out;
    }
    ok = true;
out:
    if (context)
        uci_free_context(context);
    return ok;
}

static json_object *export_ssh(const struct ora_config *cfg)
{
    struct uci_context *context = NULL;
    struct uci_package *package = NULL;
    struct uci_section *section;
    json_object *value = NULL;
    const char *port;
    const char *enable;

    (void)cfg;
    context = uci_alloc_context();
    if (!context || uci_load(context, "dropbear", &package) != UCI_OK || !package)
        goto out;
    section = ora_uci_find_section(package, "dropbear");
    if (!section)
        goto out;

    value = json_object_new_object();
    port = ora_uci_option_value(context, section, "Port");
    enable = ora_uci_option_value(context, section, "enable");
    if (port)
        json_object_object_add(value, "port", json_object_new_int(atoi(port)));
    json_object_object_add(value, "global",
                           json_object_new_boolean(!enable || strcmp(enable, "0") != 0));
out:
    if (context)
        uci_free_context(context);
    return value;
}

const struct ora_config_transform ora_config_transforms_dropbear[] = {
    { "ssh", "ssh", "errcode", ORA_CONFIG_DOMAIN_DROPBEAR,
      ORA_CONFIG_TRANSFORM_APPLIED, validate_ssh, apply_ssh, export_ssh },
};

const size_t ora_config_transforms_dropbear_count =
    sizeof(ora_config_transforms_dropbear) / sizeof(ora_config_transforms_dropbear[0]);

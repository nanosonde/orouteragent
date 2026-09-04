/* orouteragent - firewall domain (UCI firewall) configuration transforms */
#include "common.h"
#include "domains.h"
#include "uci_util.h"
#include "../protocol/message.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool validate_nat_pf(const struct ora_config *cfg, json_object *value,
                            char *error, size_t error_size)
{
    json_object *entries;
    size_t i;

    (void)cfg;
    if (!ora_config_require_object(value, error, error_size) ||
        !ora_config_optional_array(value, "settings", error, error_size) ||
        !json_object_object_get_ex(value, "settings", &entries))
        return !json_object_object_get(value, "settings");
    for (i = 0; i < json_object_array_length(entries); i++) {
        json_object *entry = json_object_array_get_idx(entries, i);
        const char *address;
        const char *external;
        const char *internal;
        bool enabled;

        if (!entry || !json_object_is_type(entry, json_type_object) ||
            ora_json_get_int(entry, "id", 0) <= 0) {
            ora_config_set_error(error, error_size, "port forward entry is invalid");
            return false;
        }
        if (ora_json_get_int(entry, "operation", 1) == 2)
            continue;
        address = ora_json_get_str(entry, "ipaddr", NULL);
        external = ora_json_get_str(entry, "externalPort", NULL);
        internal = ora_json_get_str(entry, "internalPort", NULL);
        if (!ora_config_valid_ip4(address) || !ora_config_valid_port_range(external) ||
            !ora_config_valid_port_range(internal) ||
            !ora_config_get_bool(entry, "enable", true, &enabled)) {
            ora_config_set_error(error, error_size, "port forward entry is invalid");
            return false;
        }
    }
    return true;
}

static bool apply_nat_pf(const struct ora_config *cfg, json_object *value,
                         char *error, size_t error_size)
{
    struct uci_context *context = NULL;
    struct uci_package *package = NULL;
    json_object *entries;
    size_t i;
    bool ok = false;

    (void)cfg;
    context = uci_alloc_context();
    if (!context || uci_load(context, "firewall", &package) != UCI_OK || !package) {
        ora_config_set_error(error, error_size, "cannot load UCI package firewall");
        goto out;
    }
    ora_uci_delete_sections_by_prefix(context, package, "ora_natpf_");
    if (!json_object_object_get_ex(value, "settings", &entries))
        goto commit;
    for (i = 0; i < json_object_array_length(entries); i++) {
        json_object *entry = json_object_array_get_idx(entries, i);
        int id = (int)ora_json_get_int(entry, "id", 0);
        int operation = (int)ora_json_get_int(entry, "operation", 1);
        const char *address = ora_json_get_str(entry, "ipaddr", NULL);
        const char *external = ora_json_get_str(entry, "externalPort", NULL);
        const char *internal = ora_json_get_str(entry, "internalPort", NULL);
        const char *protocol;
        char name[48];
        bool enabled;
        struct uci_section *section;

        if (!entry || !json_object_is_type(entry, json_type_object) || id <= 0 ||
            !address || !external || !internal) {
            ora_config_set_error(error, error_size, "port forward entry is invalid");
            goto out;
        }
        if (operation == 2)
            continue;
        if (!ora_config_get_bool(entry, "enable", true, &enabled)) {
            ora_config_set_error(error, error_size, "port forward enable is invalid");
            goto out;
        }
        if (json_object_is_type(json_object_object_get(entry, "protocol"),
                                json_type_int)) {
            switch (ora_json_get_int(entry, "protocol", 2)) {
            case 0: protocol = "tcp"; break;
            case 1: protocol = "udp"; break;
            case 2: protocol = "tcp udp"; break;
            default:
                ora_config_set_error(error, error_size, "port forward protocol is invalid");
                goto out;
            }
        } else {
            protocol = ora_json_get_str(entry, "protocol", "tcp udp");
        }
        snprintf(name, sizeof(name), "ora_natpf_%d", id);
        section = ora_uci_add_named_section(context, package, "redirect", name);
        if (!section ||
            !ora_uci_set_option(context, package, section, "src", "wan") ||
            !ora_uci_set_option(context, package, section, "dest", "lan") ||
            !ora_uci_set_option(context, package, section, "dest_ip", address) ||
            !ora_uci_set_option(context, package, section, "src_dport", external) ||
            !ora_uci_set_option(context, package, section, "dest_port", internal) ||
            !ora_uci_set_option(context, package, section, "proto", protocol) ||
            !ora_uci_set_option(context, package, section, "enabled",
                                enabled ? "1" : "0")) {
            ora_config_set_error(error, error_size, "cannot stage port forward");
            goto out;
        }
    }
commit:
    if (uci_commit(context, &package, false) != UCI_OK ||
        !ora_uci_reload_service("firewall")) {
        ora_config_set_error(error, error_size, "cannot apply UCI firewall settings");
        goto out;
    }
    ok = true;
out:
    if (context)
        uci_free_context(context);
    return ok;
}

static json_object *export_nat_pf(const struct ora_config *cfg)
{
    struct uci_context *context = NULL;
    struct uci_package *package = NULL;
    struct uci_element *element;
    json_object *result = NULL;

    (void)cfg;
    context = uci_alloc_context();
    if (!context || uci_load(context, "firewall", &package) != UCI_OK || !package)
        goto out;
    result = json_object_new_object();
    json_object_object_add(result, "settings", json_object_new_array());
    uci_foreach_element(&package->sections, element) {
        struct uci_section *section = uci_to_section(element);
        const char *address = ora_uci_option_value(context, section, "dest_ip");
        const char *external = ora_uci_option_value(context, section, "src_dport");
        const char *internal = ora_uci_option_value(context, section, "dest_port");
        const char *protocol = ora_uci_option_value(context, section, "proto");
        const char *enabled = ora_uci_option_value(context, section, "enabled");
        json_object *entry;

        if (strncmp(section->e.name, "ora_natpf_", 10) || !address ||
            !external || !internal)
            continue;
        entry = json_object_new_object();
        json_object_object_add(entry, "id", json_object_new_int(atoi(section->e.name + 10)));
        json_object_object_add(entry, "operation", json_object_new_int(1));
        json_object_object_add(entry, "enable", json_object_new_string(
            !enabled || strcmp(enabled, "0") ? "true" : "false"));
        json_object_object_add(entry, "ipaddr", json_object_new_string(address));
        json_object_object_add(entry, "externalPort", json_object_new_string(external));
        json_object_object_add(entry, "internalPort", json_object_new_string(internal));
        json_object_object_add(entry, "protocol", json_object_new_string(protocol ?: "tcp udp"));
        json_object_array_add(json_object_object_get(result, "settings"), entry);
    }
out:
    if (context)
        uci_free_context(context);
    return result;
}

const struct ora_config_transform ora_config_transforms_firewall[] = {
    { "natPf", "natPf", "errcode", ORA_CONFIG_DOMAIN_FIREWALL,
            ORA_CONFIG_TRANSFORM_APPLIED, validate_nat_pf, apply_nat_pf, export_nat_pf },
};

const size_t ora_config_transforms_firewall_count =
    sizeof(ora_config_transforms_firewall) / sizeof(ora_config_transforms_firewall[0]);

/* orouteragent - network domain (UCI network) configuration transforms */
#include "common.h"
#include "domains.h"
#include "uci_util.h"
#include "../protocol/message.h"
#include "../util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool validate_wan_basic(const struct ora_config *cfg, json_object *value,
                               char *error, size_t error_size)
{
    (void)cfg;
    return ora_config_require_object(value, error, error_size) &&
           ora_config_optional_array(value, "wanPorts", error, error_size);
}

static bool validate_wan_ipv4(const struct ora_config *cfg, json_object *value,
                              char *error, size_t error_size)
{
    json_object *settings;
    size_t i;

    (void)cfg;
    if (!ora_config_require_object(value, error, error_size) ||
        !ora_config_optional_array(value, "settings", error, error_size))
        return false;
    if (!json_object_object_get_ex(value, "settings", &settings))
        return true;
    for (i = 0; i < json_object_array_length(settings); i++) {
        json_object *entry = json_object_array_get_idx(settings, i);
        const char *proto;

        if (!entry || !json_object_is_type(entry, json_type_object)) {
            ora_config_set_error(error, error_size,
                                 "settings[%zu] must be an object", i);
            return false;
        }
        if (ora_json_get_int(entry, "portId", 0) < 1) {
            ora_config_set_error(error, error_size, "settings[%zu].portId is invalid", i);
            return false;
        }
        proto = json_object_get_string(json_object_object_get(entry, "proto"));
        if (!proto || (strcmp(proto, "dhcp") && strcmp(proto, "static") &&
                       strcmp(proto, "pppoe"))) {
            ora_config_set_error(error, error_size,
                                 "settings[%zu].proto is unsupported", i);
            return false;
        }
        if (!strcmp(proto, "static") &&
            (!ora_config_valid_ip4(ora_json_get_str(entry, "ipaddr", NULL)) ||
             !ora_config_valid_ip4(ora_json_get_str(entry, "netmask", NULL)))) {
            ora_config_set_error(error, error_size,
                                 "settings[%zu] static address is invalid", i);
            return false;
        }
    }
    return true;
}

static bool validate_wan_mac(const struct ora_config *cfg, json_object *value,
                             char *error, size_t error_size)
{
    json_object *settings;
    size_t i;

    (void)cfg;
    if (!ora_config_require_object(value, error, error_size) ||
        !ora_config_optional_array(value, "settings", error, error_size) ||
        !json_object_object_get_ex(value, "settings", &settings))
        return !json_object_object_get(value, "settings");
    for (i = 0; i < json_object_array_length(settings); i++) {
        json_object *entry = json_object_array_get_idx(settings, i);
        char normalized[18];

        if (!entry || !json_object_is_type(entry, json_type_object) ||
            ora_json_get_int(entry, "portId", 0) < 1 ||
            !ora_mac_normalize(ora_json_get_str(entry, "mac", NULL), normalized,
                               sizeof(normalized))) {
            ora_config_set_error(error, error_size, "WAN MAC entry is invalid");
            return false;
        }
    }
    return true;
}

static bool apply_wan_ipv4(const struct ora_config *cfg, json_object *value,
                           char *error, size_t error_size)
{
    struct uci_context *context = NULL;
    struct uci_package *package = NULL;
    json_object *settings;
    size_t i;
    bool ok = false;

    context = uci_alloc_context();
    if (!context || uci_load(context, "network", &package) != UCI_OK || !package) {
        ora_config_set_error(error, error_size, "cannot load UCI package network");
        goto out;
    }
    ora_uci_delete_owned_sections(context, package, "wanIpv4");
    if (!json_object_object_get_ex(value, "settings", &settings))
        goto commit;
    for (i = 0; i < json_object_array_length(settings); i++) {
        json_object *entry = json_object_array_get_idx(settings, i);
        int port_id = (int)ora_json_get_int(entry, "portId", 0);
        const char *proto = ora_json_get_str(entry, "proto", NULL);
        const char *device = ora_config_port_ifname(cfg, port_id);
        char name[48];
        char port_text[16];
        struct uci_section *section;
        json_object *dns = json_object_new_array();
        const char *dns1 = ora_json_get_str(entry, "dns1", NULL);
        const char *dns2 = ora_json_get_str(entry, "dns2", NULL);

        if (port_id == 1) {
            snprintf(name, sizeof(name), "wan");
            section = ora_uci_find_named_section(context, package, name);
            if (!section) {
                section = ora_uci_add_named_section(context, package, "interface", name);
                if (!section) {
                    json_object_put(dns);
                    ora_config_set_error(error, error_size, "cannot create WAN interface");
                    goto out;
                }
            }
        } else {
            snprintf(name, sizeof(name), "ora_wan_%d", port_id);
            section = ora_uci_add_named_section(context, package, "interface", name);
            if (!section) {
                json_object_put(dns);
                ora_config_set_error(error, error_size, "cannot create managed WAN interface");
                goto out;
            }
            snprintf(port_text, sizeof(port_text), "%d", port_id);
            if (!ora_uci_set_option(context, package, section, "ora_owner", "orouteragent") ||
                !ora_uci_set_option(context, package, section, "ora_key", "wanIpv4") ||
                !ora_uci_set_option(context, package, section, "ora_port", port_text)) {
                json_object_put(dns);
                ora_config_set_error(error, error_size, "cannot mark managed WAN interface");
                goto out;
            }
        }
        if (dns1)
            json_object_array_add(dns, json_object_new_string(dns1));
        if (dns2)
            json_object_array_add(dns, json_object_new_string(dns2));
        if (!ora_uci_set_option(context, package, section, "device", device) ||
            !ora_uci_set_option(context, package, section, "proto", proto) ||
            !ora_uci_delete_option(context, package, section, "ipaddr") ||
            !ora_uci_delete_option(context, package, section, "netmask") ||
            !ora_uci_delete_option(context, package, section, "gateway") ||
            !ora_uci_delete_option(context, package, section, "username") ||
            !ora_uci_delete_option(context, package, section, "password") ||
            !ora_uci_replace_list(context, package, section, "dns", dns)) {
            json_object_put(dns);
            ora_config_set_error(error, error_size, "cannot stage WAN IPv4 settings");
            goto out;
        }
        json_object_put(dns);
        if (!strcmp(proto, "static") &&
            (!ora_uci_set_option(context, package, section, "ipaddr",
                                 ora_json_get_str(entry, "ipaddr", "")) ||
             !ora_uci_set_option(context, package, section, "netmask",
                                 ora_json_get_str(entry, "netmask", "")) ||
             (ora_json_get_str(entry, "gateway", NULL) &&
              !ora_uci_set_option(context, package, section, "gateway",
                                  ora_json_get_str(entry, "gateway", ""))))) {
            ora_config_set_error(error, error_size, "cannot stage static WAN IPv4");
            goto out;
        }
        if (!strcmp(proto, "pppoe") &&
            (!ora_uci_set_option(context, package, section, "username",
                                 ora_json_get_str(entry, "username", "")) ||
             !ora_uci_set_option(context, package, section, "password",
                                 ora_json_get_str(entry, "password", "")))) {
            ora_config_set_error(error, error_size, "cannot stage PPPoE settings");
            goto out;
        }
    }
commit:
    if (uci_commit(context, &package, false) != UCI_OK ||
        !ora_uci_reload_service("network")) {
        ora_config_set_error(error, error_size, "cannot apply UCI WAN settings");
        goto out;
    }
    ok = true;
out:
    if (context)
        uci_free_context(context);
    return ok;
}

static bool apply_wan_mac(const struct ora_config *cfg, json_object *value,
                          char *error, size_t error_size)
{
    struct uci_context *context = NULL;
    struct uci_package *package = NULL;
    json_object *settings;
    size_t i;
    bool ok = false;

    context = uci_alloc_context();
    if (!context || uci_load(context, "network", &package) != UCI_OK || !package) {
        ora_config_set_error(error, error_size, "cannot load UCI package network");
        goto out;
    }
    if (!json_object_object_get_ex(value, "settings", &settings))
        goto commit;
    for (i = 0; i < json_object_array_length(settings); i++) {
        json_object *entry = json_object_array_get_idx(settings, i);
        int port_id = (int)ora_json_get_int(entry, "portId", 0);
        const char *section_name = port_id == 1 ? "wan" : NULL;
        struct uci_section *section;
        char normalized[18];

        if (!section_name) {
            ora_config_set_error(error, error_size,
                                 "WAN MAC changes require the primary WAN port");
            goto out;
        }
        section = ora_uci_find_named_section(context, package, section_name);
        if (!section || !ora_mac_normalize(ora_json_get_str(entry, "mac", NULL),
                                            normalized, sizeof(normalized)) ||
            !ora_uci_set_option(context, package, section, "macaddr", normalized)) {
            ora_config_set_error(error, error_size, "cannot stage WAN MAC address");
            goto out;
        }
    }
commit:
    if (uci_commit(context, &package, false) != UCI_OK ||
        !ora_uci_reload_service("network")) {
        ora_config_set_error(error, error_size, "cannot apply WAN MAC settings");
        goto out;
    }
    ok = true;
out:
    if (context)
        uci_free_context(context);
    return ok;
}

static bool validate_network(const struct ora_config *cfg, json_object *value,
                             char *error, size_t error_size)
{
    (void)cfg;
    return ora_config_require_object(value, error, error_size) &&
           ora_config_optional_array(value, "networks", error, error_size);
}

static bool validate_static_routing(const struct ora_config *cfg, json_object *value,
                                    char *error, size_t error_size)
{
    json_object *routes;
    size_t i;

    if (!ora_config_require_object(value, error, error_size) ||
        !ora_config_optional_array(value, "staticRoutings", error, error_size) ||
        !json_object_object_get_ex(value, "staticRoutings", &routes))
        return !json_object_object_get(value, "staticRoutings");
    for (i = 0; i < json_object_array_length(routes); i++) {
        json_object *entry = json_object_array_get_idx(routes, i);
        json_object *destinations;
        size_t destination_index;

        if (!entry || !json_object_is_type(entry, json_type_object) ||
            ora_json_get_int(entry, "id", 0) <= 0 ||
            ora_json_get_int(entry, "operation", 1) == 2)
            continue;
        if (!ora_config_valid_ip4(ora_json_get_str(entry, "nextHopIp", NULL)) ||
            !json_object_object_get_ex(entry, "destinations", &destinations) ||
            !json_object_is_type(destinations, json_type_array) ||
            !ora_config_port_ifname(cfg, (int)ora_json_get_int(entry, "interface", 1))) {
            ora_config_set_error(error, error_size, "static route is invalid");
            return false;
        }
        for (destination_index = 0;
             destination_index < json_object_array_length(destinations);
             destination_index++) {
            const char *destination = json_object_get_string(
                json_object_array_get_idx(destinations, destination_index));

            if (!destination || !strchr(destination, '/')) {
                ora_config_set_error(error, error_size,
                                     "static route destination is invalid");
                return false;
            }
        }
    }
    return true;
}

static bool apply_static_routing(const struct ora_config *cfg, json_object *value,
                                 char *error, size_t error_size)
{
    struct uci_context *context = NULL;
    struct uci_package *package = NULL;
    json_object *routes;
    size_t i;
    bool ok = false;

    context = uci_alloc_context();
    if (!context || uci_load(context, "network", &package) != UCI_OK || !package) {
        ora_config_set_error(error, error_size, "cannot load UCI package network");
        goto out;
    }
    ora_uci_delete_owned_sections(context, package, "staticRouting");
    if (!json_object_object_get_ex(value, "staticRoutings", &routes))
        goto commit;
    for (i = 0; i < json_object_array_length(routes); i++) {
        json_object *entry = json_object_array_get_idx(routes, i);
        json_object *destinations;
        int id = (int)ora_json_get_int(entry, "id", 0);
        int operation = (int)ora_json_get_int(entry, "operation", 1);
        const char *gateway = ora_json_get_str(entry, "nextHopIp", NULL);
        const char *ifname;
        size_t destination_index;

        if (!entry || !json_object_is_type(entry, json_type_object) || id <= 0 ||
            !gateway || !ora_ip4_parse(gateway, &(uint32_t){0}) ||
            !json_object_object_get_ex(entry, "destinations", &destinations) ||
            !json_object_is_type(destinations, json_type_array)) {
            ora_config_set_error(error, error_size, "static route is invalid");
            goto out;
        }
        if (operation == 2)
            continue;
        ifname = json_object_is_type(json_object_object_get(entry, "interface"),
                                     json_type_string) ?
            ora_json_get_str(entry, "interface", NULL) :
            ora_config_port_ifname(cfg, (int)ora_json_get_int(entry, "interface", 1));
        if (!ifname || !*ifname) {
            ora_config_set_error(error, error_size, "static route interface is invalid");
            goto out;
        }
        for (destination_index = 0;
             destination_index < json_object_array_length(destinations);
             destination_index++) {
            const char *target = json_object_get_string(
                json_object_array_get_idx(destinations, destination_index));
            char name[48];
            char id_text[16];
            char metric[16];
            struct uci_section *section;

            if (!target || !strchr(target, '/')) {
                ora_config_set_error(error, error_size, "static route destination is invalid");
                goto out;
            }
            snprintf(name, sizeof(name), "ora_route_%d_%zu", id, destination_index);
            section = ora_uci_add_named_section(context, package, "route", name);
            if (!section) {
                ora_config_set_error(error, error_size, "cannot create UCI route section");
                goto out;
            }
            snprintf(id_text, sizeof(id_text), "%d", id);
            snprintf(metric, sizeof(metric), "%" PRId64,
                     ora_json_get_int(entry, "metric", 1));
            if (!ora_uci_set_option(context, package, section, "ora_owner", "orouteragent") ||
                !ora_uci_set_option(context, package, section, "ora_key", "staticRouting") ||
                !ora_uci_set_option(context, package, section, "ora_id", id_text) ||
                !ora_uci_set_option(context, package, section, "target", target) ||
                !ora_uci_set_option(context, package, section, "gateway", gateway) ||
                !ora_uci_set_option(context, package, section, "interface", ifname) ||
                !ora_uci_set_option(context, package, section, "metric", metric)) {
                ora_config_set_error(error, error_size, "cannot stage UCI route");
                goto out;
            }
        }
    }
commit:
    if (uci_commit(context, &package, false) != UCI_OK ||
        !ora_uci_reload_service("network")) {
        ora_config_set_error(error, error_size, "cannot apply UCI network routes");
        goto out;
    }
    ok = true;
out:
    if (context)
        uci_free_context(context);
    return ok;
}

static json_object *export_static_routing(const struct ora_config *cfg)
{
    struct uci_context *context = NULL;
    struct uci_package *package = NULL;
    struct uci_element *element;
    json_object *result = NULL;

    (void)cfg;
    context = uci_alloc_context();
    if (!context || uci_load(context, "network", &package) != UCI_OK || !package)
        goto out;
    result = json_object_new_object();
    json_object_object_add(result, "staticRoutings", json_object_new_array());
    uci_foreach_element(&package->sections, element) {
        struct uci_section *section = uci_to_section(element);
        const char *owner = ora_uci_option_value(context, section, "ora_owner");
        const char *key = ora_uci_option_value(context, section, "ora_key");
        const char *target = ora_uci_option_value(context, section, "target");
        const char *gateway = ora_uci_option_value(context, section, "gateway");
        const char *interface = ora_uci_option_value(context, section, "interface");
        const char *metric = ora_uci_option_value(context, section, "metric");
        const char *id = ora_uci_option_value(context, section, "ora_id");
        json_object *entry;
        json_object *destinations;

        if (!owner || !key || strcmp(owner, "orouteragent") ||
            strcmp(key, "staticRouting") || !target || !gateway || !id)
            continue;
        entry = json_object_new_object();
        destinations = json_object_new_array();
        json_object_array_add(destinations, json_object_new_string(target));
        json_object_object_add(entry, "id", json_object_new_int(atoi(id)));
        json_object_object_add(entry, "operation", json_object_new_int(1));
        json_object_object_add(entry, "destinations", destinations);
        json_object_object_add(entry, "nextHopIp", json_object_new_string(gateway));
        json_object_object_add(entry, "interface", json_object_new_string(interface ?: ""));
        json_object_object_add(entry, "metric", json_object_new_int(atoi(metric ?: "1")));
        json_object_array_add(json_object_object_get(result, "staticRoutings"), entry);
    }
out:
    if (context)
        uci_free_context(context);
    return result;
}

const struct ora_config_transform ora_config_transforms_network[] = {
    { "wanBasicSetting", "wanBasicSetting", "errcode", ORA_CONFIG_DOMAIN_NETWORK,
      ORA_CONFIG_TRANSFORM_STAGED, validate_wan_basic, NULL, NULL },
    { "wanIpv4", "wanIpv4", "errcode", ORA_CONFIG_DOMAIN_NETWORK,
            ORA_CONFIG_TRANSFORM_APPLIED, validate_wan_ipv4, apply_wan_ipv4, NULL },
        { "wanMac", "wanMac", "errcode", ORA_CONFIG_DOMAIN_NETWORK,
            ORA_CONFIG_TRANSFORM_APPLIED, validate_wan_mac, apply_wan_mac, NULL },
    { "network", "network", "errcode",
      ORA_CONFIG_DOMAIN_NETWORK | ORA_CONFIG_DOMAIN_DHCP,
      ORA_CONFIG_TRANSFORM_STAGED, validate_network, NULL, NULL },
    { "staticRouting", "staticRouting", "errcode", ORA_CONFIG_DOMAIN_NETWORK,
            ORA_CONFIG_TRANSFORM_APPLIED, validate_static_routing,
            apply_static_routing, export_static_routing },
};

const size_t ora_config_transforms_network_count =
    sizeof(ora_config_transforms_network) / sizeof(ora_config_transforms_network[0]);

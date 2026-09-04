/* orouteragent - dhcp domain (dnsmasq/odhcpd UCI) configuration transforms */
#include "common.h"
#include "domains.h"
#include "uci_util.h"
#include "../protocol/message.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static bool validate_lan_dns(const struct ora_config *cfg, json_object *value,
                             char *error, size_t error_size)
{
  json_object *entries;
  size_t i;

    (void)cfg;
  if (!ora_config_require_object(value, error, error_size) ||
    !ora_config_optional_array(value, "dnsList", error, error_size) ||
    !json_object_object_get_ex(value, "dnsList", &entries))
    return !json_object_object_get(value, "dnsList");
  for (i = 0; i < json_object_array_length(entries); i++) {
    json_object *entry = json_object_array_get_idx(entries, i);
    json_object *addresses;
    size_t address_index;

    if (!entry || !json_object_is_type(entry, json_type_object) ||
      ora_json_get_int(entry, "id", 0) <= 0 ||
      !ora_json_get_str(entry, "domain", NULL) ||
      !json_object_object_get_ex(entry, "ipAddrs", &addresses) ||
      !json_object_is_type(addresses, json_type_array)) {
      ora_config_set_error(error, error_size, "LAN DNS entry is invalid");
      return false;
    }
    for (address_index = 0; address_index < json_object_array_length(addresses);
       address_index++) {
      if (!ora_config_valid_ip4(json_object_get_string(
          json_object_array_get_idx(addresses, address_index)))) {
        ora_config_set_error(error, error_size, "LAN DNS address is invalid");
        return false;
      }
    }
  }
  return true;
}

static bool validate_dns_cache(const struct ora_config *cfg, json_object *value,
                               char *error, size_t error_size)
{
    (void)cfg;
    return ora_config_require_object(value, error, error_size);
}

  static bool apply_lan_dns(const struct ora_config *cfg, json_object *value,
                char *error, size_t error_size)
  {
    struct uci_context *context = NULL;
    struct uci_package *package = NULL;
    json_object *entries;
    size_t i;
    bool ok = false;

    (void)cfg;
    context = uci_alloc_context();
    if (!context || uci_load(context, "dhcp", &package) != UCI_OK || !package) {
      ora_config_set_error(error, error_size, "cannot load UCI package dhcp");
      goto out;
    }
    ora_uci_delete_sections_by_prefix(context, package, "ora_dns_");
    if (!json_object_object_get_ex(value, "dnsList", &entries))
      goto commit;
    for (i = 0; i < json_object_array_length(entries); i++) {
      json_object *entry = json_object_array_get_idx(entries, i);
      json_object *addresses;
      int id = (int)ora_json_get_int(entry, "id", 0);
      const char *domain = ora_json_get_str(entry, "domain", NULL);
      char name[48];
      struct uci_section *section;

      if (!entry || !json_object_is_type(entry, json_type_object) || id <= 0 ||
        !domain || !*domain ||
        !json_object_object_get_ex(entry, "ipAddrs", &addresses) ||
        !json_object_is_type(addresses, json_type_array)) {
        ora_config_set_error(error, error_size, "LAN DNS entry is invalid");
        goto out;
      }
      snprintf(name, sizeof(name), "ora_dns_%d", id);
      section = ora_uci_add_named_section(context, package, "domain", name);
      if (!section ||
        !ora_uci_set_option(context, package, section, "name", domain) ||
        !ora_uci_replace_list(context, package, section, "ip", addresses)) {
        ora_config_set_error(error, error_size, "cannot stage LAN DNS entry");
        goto out;
      }
    }
  commit:
    if (uci_commit(context, &package, false) != UCI_OK ||
      !ora_uci_reload_service("dnsmasq")) {
      ora_config_set_error(error, error_size, "cannot apply UCI DHCP settings");
      goto out;
    }
    ok = true;
  out:
    if (context)
      uci_free_context(context);
    return ok;
  }

  static bool apply_dns_cache(const struct ora_config *cfg, json_object *value,
                char *error, size_t error_size)
  {
    struct uci_context *context = NULL;
    struct uci_package *package = NULL;
    struct uci_section *section;
    json_object *settings;
    char ttl[16];
    bool ok = false;

    (void)cfg;
    context = uci_alloc_context();
    if (!context || uci_load(context, "dhcp", &package) != UCI_OK || !package ||
      !(section = ora_uci_find_section(package, "dnsmasq"))) {
      ora_config_set_error(error, error_size, "cannot load dnsmasq UCI settings");
      goto out;
    }
    settings = ora_json_get_obj(value, "config");
    if (!settings)
      settings = value;
    if (json_object_object_get(settings, "enable")) {
      if (!ora_uci_set_option(context, package, section, "cachesize",
                  ora_json_get_bool(settings, "enable", false) ? "1000" : "0")) {
        ora_config_set_error(error, error_size, "cannot stage DNS cache state");
        goto out;
      }
    }
    if (json_object_object_get(settings, "ttl")) {
      snprintf(ttl, sizeof(ttl), "%" PRId64,
           ora_json_get_int(settings, "ttl", 0));
      if (!ora_uci_set_option(context, package, section, "max_ttl", ttl)) {
        ora_config_set_error(error, error_size, "cannot stage DNS cache TTL");
        goto out;
      }
    }
    if (uci_commit(context, &package, false) != UCI_OK ||
      !ora_uci_reload_service("dnsmasq")) {
      ora_config_set_error(error, error_size, "cannot apply DNS cache settings");
      goto out;
    }
    ok = true;
  out:
    if (context)
      uci_free_context(context);
    return ok;
  }

  static json_object *export_lan_dns(const struct ora_config *cfg)
  {
    struct uci_context *context = NULL;
    struct uci_package *package = NULL;
    struct uci_element *element;
    json_object *result = NULL;

    (void)cfg;
    context = uci_alloc_context();
    if (!context || uci_load(context, "dhcp", &package) != UCI_OK || !package)
      goto out;
    result = json_object_new_object();
    json_object_object_add(result, "dnsList", json_object_new_array());
    uci_foreach_element(&package->sections, element) {
      struct uci_section *section = uci_to_section(element);
      struct uci_option *addresses;
      const char *domain = ora_uci_option_value(context, section, "name");
      json_object *entry;
      json_object *values;
      struct uci_element *address;

      if (strncmp(section->e.name, "ora_dns_", 8) || !domain)
        continue;
      entry = json_object_new_object();
      values = json_object_new_array();
      addresses = uci_lookup_option(context, section, "ip");
      if (addresses && addresses->type == UCI_TYPE_LIST) {
        uci_foreach_element(&addresses->v.list, address)
          json_object_array_add(values, json_object_new_string(address->name));
      } else if (addresses && addresses->type == UCI_TYPE_STRING) {
        json_object_array_add(values, json_object_new_string(addresses->v.string));
      }
      json_object_object_add(entry, "id", json_object_new_int(atoi(section->e.name + 8)));
      json_object_object_add(entry, "operation", json_object_new_int(1));
      json_object_object_add(entry, "domain", json_object_new_string(domain));
      json_object_object_add(entry, "ipAddrs", values);
      json_object_array_add(json_object_object_get(result, "dnsList"), entry);
    }
  out:
    if (context)
      uci_free_context(context);
    return result;
  }

  static json_object *export_dns_cache(const struct ora_config *cfg)
  {
    struct uci_context *context = NULL;
    struct uci_package *package = NULL;
    struct uci_section *section;
    json_object *result = NULL;
    json_object *settings;
    const char *cache_size;
    const char *ttl;

    (void)cfg;
    context = uci_alloc_context();
    if (!context || uci_load(context, "dhcp", &package) != UCI_OK || !package ||
      !(section = ora_uci_find_section(package, "dnsmasq")))
      goto out;
    result = json_object_new_object();
    settings = json_object_new_object();
    cache_size = ora_uci_option_value(context, section, "cachesize");
    ttl = ora_uci_option_value(context, section, "max_ttl");
    json_object_object_add(settings, "enable", json_object_new_boolean(
      !cache_size || strcmp(cache_size, "0") != 0));
    if (ttl)
      json_object_object_add(settings, "ttl", json_object_new_int(atoi(ttl)));
    json_object_object_add(result, "operation", json_object_new_int(0));
    json_object_object_add(result, "config", settings);
  out:
    if (context)
      uci_free_context(context);
    return result;
  }

const struct ora_config_transform ora_config_transforms_dhcp[] = {
    { "lanDns", "lanDns", "errcode", ORA_CONFIG_DOMAIN_DHCP,
      ORA_CONFIG_TRANSFORM_APPLIED, validate_lan_dns, apply_lan_dns, export_lan_dns },
    { "dnsCache", "dnsCache", "errcode", ORA_CONFIG_DOMAIN_DHCP,
      ORA_CONFIG_TRANSFORM_APPLIED, validate_dns_cache, apply_dns_cache,
      export_dns_cache },
};

const size_t ora_config_transforms_dhcp_count =
    sizeof(ora_config_transforms_dhcp) / sizeof(ora_config_transforms_dhcp[0]);

/* orouteragent - libuci and service helpers for the configuration domains */
#ifndef ORA_CONFIG_UCI_UTIL_H
#define ORA_CONFIG_UCI_UTIL_H

#include <json-c/json.h>
#include <uci.h>

#include <stdbool.h>

struct uci_section *ora_uci_find_section(struct uci_package *package,
                                         const char *type);
struct uci_section *ora_uci_find_named_section(struct uci_context *context,
                                               struct uci_package *package,
                                               const char *name);
const char *ora_uci_option_value(struct uci_context *context,
                                 struct uci_section *section, const char *option);
bool ora_uci_set_option(struct uci_context *context, struct uci_package *package,
                        struct uci_section *section, const char *option,
                        const char *value);
bool ora_uci_delete_option(struct uci_context *context, struct uci_package *package,
                           struct uci_section *section, const char *option);
bool ora_uci_replace_list(struct uci_context *context, struct uci_package *package,
                          struct uci_section *section, const char *option,
                          json_object *values);
bool ora_uci_delete_section(struct uci_context *context, struct uci_package *package,
                            struct uci_section *section);
struct uci_section *ora_uci_add_named_section(struct uci_context *context,
                                              struct uci_package *package,
                                              const char *type, const char *name);
void ora_uci_delete_sections_by_prefix(struct uci_context *context,
                                       struct uci_package *package,
                                       const char *prefix);
void ora_uci_delete_owned_sections(struct uci_context *context,
                                   struct uci_package *package,
                                   const char *key);

/* Ask procd to reload a service. False means the running service kept its
 * previous configuration, so the caller must roll its UCI changes back. */
bool ora_uci_reload_service(const char *name);

#endif

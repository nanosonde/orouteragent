/* orouteragent - libuci and service helpers for the configuration domains */
#include "uci_util.h"

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct uci_section *ora_uci_find_section(struct uci_package *package,
                                         const char *type)
{
    struct uci_element *element;

    uci_foreach_element(&package->sections, element) {
        struct uci_section *section = uci_to_section(element);

        if (!strcmp(section->type, type))
            return section;
    }
    return NULL;
}

struct uci_section *ora_uci_find_named_section(struct uci_context *context,
                                               struct uci_package *package,
                                               const char *name)
{
    struct uci_element *element;

    uci_foreach_element(&package->sections, element) {
        struct uci_section *section = uci_to_section(element);

        if (!strcmp(section->e.name, name))
            return section;
    }
    return NULL;
}

const char *ora_uci_option_value(struct uci_context *context,
                                 struct uci_section *section, const char *option)
{
    struct uci_option *value = uci_lookup_option(context, section, option);

    return value && value->type == UCI_TYPE_STRING ? value->v.string : NULL;
}

bool ora_uci_set_option(struct uci_context *context, struct uci_package *package,
                        struct uci_section *section, const char *option,
                        const char *value)
{
    struct uci_ptr pointer = {
        .p = package,
        .s = section,
        .option = option,
        .value = value,
    };

    return uci_set(context, &pointer) == UCI_OK;
}

bool ora_uci_delete_option(struct uci_context *context, struct uci_package *package,
                           struct uci_section *section, const char *option)
{
    struct uci_ptr pointer = {
        .p = package,
        .s = section,
        .option = option,
    };

    int rc = uci_delete(context, &pointer);

    return rc == UCI_OK || rc == UCI_ERR_NOTFOUND;
}

bool ora_uci_replace_list(struct uci_context *context, struct uci_package *package,
                          struct uci_section *section, const char *option,
                          json_object *values)
{
    struct uci_ptr pointer = {
        .p = package,
        .s = section,
        .option = option,
    };
    size_t i;

    if (!ora_uci_delete_option(context, package, section, option))
        return false;
    if (!values)
        return true;
    for (i = 0; i < json_object_array_length(values); i++) {
        const char *value = json_object_get_string(
            json_object_array_get_idx(values, i));

        if (!value)
            return false;
        pointer.value = value;
        if (uci_add_list(context, &pointer) != UCI_OK)
            return false;
    }
    return true;
}

bool ora_uci_delete_section(struct uci_context *context, struct uci_package *package,
                            struct uci_section *section)
{
    struct uci_ptr pointer = { .p = package, .s = section };

    return uci_delete(context, &pointer) == UCI_OK;
}

struct uci_section *ora_uci_add_named_section(struct uci_context *context,
                                              struct uci_package *package,
                                              const char *type, const char *name)
{
    struct uci_section *section;

    section = ora_uci_find_named_section(context, package, name);
    if (section)
        return section;
    if (uci_add_section(context, package, type, &section) != UCI_OK)
        return NULL;
    {
        struct uci_ptr pointer = {
            .target = UCI_TYPE_SECTION,
            .p = package,
            .s = section,
            .value = name,
        };

        if (uci_rename(context, &pointer) != UCI_OK)
            return NULL;
    }
    return ora_uci_find_named_section(context, package, name);
}

void ora_uci_delete_sections_by_prefix(struct uci_context *context,
                                       struct uci_package *package,
                                       const char *prefix)
{
    struct uci_element *element;
    struct uci_section *matches[128];
    size_t count = 0;
    size_t i;
    size_t prefix_len = strlen(prefix);

    uci_foreach_element(&package->sections, element) {
        struct uci_section *section = uci_to_section(element);

        if (!strncmp(section->e.name, prefix, prefix_len) &&
            count < sizeof(matches) / sizeof(matches[0]))
            matches[count++] = section;
    }
    for (i = 0; i < count; i++)
        (void)ora_uci_delete_section(context, package, matches[i]);
}

void ora_uci_delete_owned_sections(struct uci_context *context,
                                   struct uci_package *package,
                                   const char *key)
{
    struct uci_element *element;
    struct uci_section *matches[128];
    size_t count = 0;
    size_t i;

    uci_foreach_element(&package->sections, element) {
        struct uci_section *section = uci_to_section(element);
        const char *owner = ora_uci_option_value(context, section, "ora_owner");
        const char *section_key = ora_uci_option_value(context, section, "ora_key");

        if (owner && section_key && !strcmp(owner, "orouteragent") &&
            !strcmp(section_key, key) && count < sizeof(matches) / sizeof(matches[0]))
            matches[count++] = section;
    }
    for (i = 0; i < count; i++)
        (void)ora_uci_delete_section(context, package, matches[i]);
}

bool ora_uci_reload_service(const char *name)
{
    static const char *const supported[] = {
        "network", "dnsmasq", "firewall", "sysntpd", "dropbear"
    };
    char path[64];
    pid_t pid;
    int status;
    size_t i;

    for (i = 0; i < sizeof(supported) / sizeof(supported[0]); i++)
        if (!strcmp(name, supported[i]))
            break;
    if (i == sizeof(supported) / sizeof(supported[0]))
        return false;
    snprintf(path, sizeof(path), "/etc/init.d/%s", name);
    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        execl(path, path, "reload", (char *)NULL);
        _exit(127);
    }
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

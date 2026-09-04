/* orouteragent - controller configuration transform registry */
#include "registry.h"
#include "domains.h"

#include <string.h>

/* Keys without a native transform whose acknowledgement name or error-key
 * spelling is nevertheless fixed by the protocol. */
static const struct ora_config_transform protocol_acks[] = {
    { "remoteLog", "logSetting", "errcode", 0,
      ORA_CONFIG_TRANSFORM_STAGED, NULL, NULL, NULL },
    { "dstConfig", "dst", "errcode", 0,
      ORA_CONFIG_TRANSFORM_STAGED, NULL, NULL, NULL },
    { "vpnUser", "vpnUsers", "errcode", 0,
      ORA_CONFIG_TRANSFORM_STAGED, NULL, NULL, NULL },
    { "domainNoPortGroup", "fqdnGroup", "errcode", 0,
      ORA_CONFIG_TRANSFORM_STAGED, NULL, NULL, NULL },
    { "highAbility", "backupConfig", "errcode", 0,
      ORA_CONFIG_TRANSFORM_STAGED, NULL, NULL, NULL },
    { "packageCapture", "packageCapture", "errCode", 0,
      ORA_CONFIG_TRANSFORM_LIVE_COMMAND, NULL, NULL, NULL },
};

static const struct ora_config_transform *
find_in(const struct ora_config_transform *entries, size_t count, const char *key)
{
    size_t i;

    for (i = 0; i < count; i++)
        if (!strcmp(entries[i].key, key))
            return &entries[i];
    return NULL;
}

const struct ora_config_transform *ora_config_transform_find(const char *key)
{
    const struct ora_config_transform *transform;

    if (!key)
        return NULL;

    transform = find_in(ora_config_transforms_network,
                        ora_config_transforms_network_count, key);
    if (transform)
        return transform;
    transform = find_in(ora_config_transforms_dhcp,
                        ora_config_transforms_dhcp_count, key);
    if (transform)
        return transform;
    transform = find_in(ora_config_transforms_firewall,
                        ora_config_transforms_firewall_count, key);
    if (transform)
        return transform;
    transform = find_in(ora_config_transforms_system,
                        ora_config_transforms_system_count, key);
    if (transform)
        return transform;
    transform = find_in(ora_config_transforms_dropbear,
                        ora_config_transforms_dropbear_count, key);
    if (transform)
        return transform;

    return find_in(protocol_acks,
                   sizeof(protocol_acks) / sizeof(protocol_acks[0]), key);
}

bool ora_config_transform_validate(const struct ora_config_transform *transform,
                                   const struct ora_config *cfg,
                                   json_object *value, char *error,
                                   size_t error_size)
{
    if (error && error_size)
        error[0] = '\0';
    return !transform || !transform->validate ||
           transform->validate(cfg, value, error, error_size);
}

const char *ora_config_transform_ack_key(const char *key)
{
    const struct ora_config_transform *transform = ora_config_transform_find(key);

    return transform ? transform->ack_key : key;
}

const char *ora_config_transform_ack_error_key(const char *key)
{
    const struct ora_config_transform *transform = ora_config_transform_find(key);

    return transform ? transform->ack_error_key : "errcode";
}

/* orouteragent - native UCI configuration transform boundary */
#include "uci.h"
#include "common.h"

bool ora_uci_config_transform_is_managed(const struct ora_config *cfg,
                                         const struct ora_config_transform *transform)
{
    return transform && ora_config_domains_enabled(cfg, transform->domains);
}

bool ora_uci_config_apply(const struct ora_config *cfg,
                          const struct ora_config_transform *transform,
                          json_object *value, char *error, size_t error_size)
{
    if (!cfg) {
        ora_config_set_error(error, error_size, "configuration is unavailable");
        return false;
    }
    if (!transform) {
        ora_config_set_error(error, error_size, "configuration key is not native");
        return false;
    }
    if (!ora_uci_config_transform_is_managed(cfg, transform)) {
        ora_config_set_error(error, error_size, "transform domain is not managed");
        return false;
    }
    if (transform->status != ORA_CONFIG_TRANSFORM_APPLIED || !transform->apply) {
        ora_config_set_error(error, error_size, "native transform is not implemented");
        return false;
    }
    return transform->apply(cfg, value, error, error_size);
}

json_object *ora_uci_config_export(const struct ora_config *cfg, const char *key)
{
    const struct ora_config_transform *transform = ora_config_transform_find(key);

    if (!ora_uci_config_transform_is_managed(cfg, transform) ||
        transform->status != ORA_CONFIG_TRANSFORM_APPLIED ||
        !transform->export_native)
        return NULL;
    return transform->export_native(cfg);
}

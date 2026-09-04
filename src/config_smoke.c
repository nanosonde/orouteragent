/* Target-only smoke tests for controller/UCI configuration transforms. */
#include "config.h"
#include "config/registry.h"
#include "config/uci.h"

#include <stdio.h>
#include <string.h>

static bool apply_json(const struct ora_config *cfg, const char *key,
                       const char *text)
{
    const struct ora_config_transform *transform = ora_config_transform_find(key);
    json_object *value = json_tokener_parse(text);
    json_object *native;
    char error[256];
    bool ok;

    if (!value) {
        fprintf(stderr, "%s: cannot parse fixture\n", key);
        return false;
    }
    fprintf(stderr, "%s: apply\n", key);
    ok = ora_config_transform_validate(transform, cfg, value, error, sizeof(error)) &&
         ora_uci_config_apply(cfg, transform, value, error, sizeof(error));
    fprintf(stderr, "%s: export\n", key);
    native = ok ? ora_uci_config_export(cfg, key) : NULL;
    if (!ok || ((strcmp(key, "staticRouting") == 0 || strcmp(key, "lanDns") == 0 ||
                 strcmp(key, "dnsCache") == 0 || strcmp(key, "timeSetting") == 0 ||
                 strcmp(key, "ssh") == 0 || strcmp(key, "natPf") == 0) && !native)) {
        fprintf(stderr, "%s: %s\n", key, ok ? "no native export" : error);
        ok = false;
    }
    if (native)
        json_object_put(native);
    json_object_put(value);
    return ok;
}

static bool selected(int argc, char **argv, const char *domain)
{
    int i;

    if (argc == 1)
        return true;
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], domain))
            return true;
    return false;
}

int main(int argc, char **argv)
{
    struct ora_config cfg;
    unsigned required_domains = 0;

    if (!ora_config_load(&cfg)) {
        fprintf(stderr, "cannot load agent configuration\n");
        return 1;
    }
    if (selected(argc, argv, "network"))
        required_domains |= ORA_CONFIG_DOMAIN_NETWORK;
    if (selected(argc, argv, "dhcp"))
        required_domains |= ORA_CONFIG_DOMAIN_DHCP;
    if (selected(argc, argv, "firewall"))
        required_domains |= ORA_CONFIG_DOMAIN_FIREWALL;
    if (selected(argc, argv, "system"))
        required_domains |= ORA_CONFIG_DOMAIN_SYSTEM;
    if (!ora_config_domains_enabled(&cfg, required_domains)) {
        fprintf(stderr, "selected smoke-test domains must be enabled\n");
        return 1;
    }
    if (selected(argc, argv, "network")) {
        if (!apply_json(&cfg, "wanIpv4",
                        "{\"settings\":[{\"portId\":1,\"proto\":\"dhcp\"}]}"))
            return 1;
        if (!apply_json(&cfg, "staticRouting",
                        "{\"staticRoutings\":[{\"id\":9001,\"operation\":1,\"destinations\":[\"203.0.113.0/24\"],\"nextHopIp\":\"192.0.2.1\",\"interface\":1,\"metric\":10}]}"))
            return 1;
    }
    if (selected(argc, argv, "dhcp")) {
        if (!apply_json(&cfg, "lanDns",
                        "{\"dnsList\":[{\"id\":9002,\"operation\":1,\"domain\":\"config-smoke.test\",\"ipAddrs\":[\"192.0.2.2\"]}]}"))
            return 1;
        if (!apply_json(&cfg, "dnsCache",
                        "{\"operation\":0,\"config\":{\"enable\":true,\"ttl\":60}}"))
            return 1;
    }
    if (selected(argc, argv, "firewall") &&
        !apply_json(&cfg, "natPf",
                    "{\"settings\":[{\"id\":9003,\"operation\":1,\"enable\":\"true\",\"ipaddr\":\"192.0.2.3\",\"externalPort\":\"8443\",\"internalPort\":\"443\",\"protocol\":0}]}"))
        return 1;
    if (selected(argc, argv, "system") &&
        !apply_json(&cfg, "timeSetting",
                    "{\"timeZone\":\"UTC\",\"ntpEnable\":true,\"ntpServers\":[\"0.openwrt.pool.ntp.org\"]}"))
        return 1;
    puts("configuration transform smoke tests passed");
    return 0;
}
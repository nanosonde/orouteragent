/* orouteragent - UCI configuration implementation */
#include "config.h"
#include "protocol/constants.h"
#include "util.h"

#include <libubox/blobmsg_json.h>
#include <libubus.h>
#include <uci.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void uci_str(char *dst, size_t dstsz, struct uci_option *o)
{
    const char *s;

    dst[0] = '\0';
    if (!o || o->type != UCI_TYPE_STRING)
        return;
    s = o->v.string;
    if (s)
        snprintf(dst, dstsz, "%s", s);
}

static int uci_int(int dflt, struct uci_option *o)
{
    const char *s;
    long v;

    if (!o || o->type != UCI_TYPE_STRING)
        return dflt;
    s = o->v.string;
    if (!s || !*s)
        return dflt;
    v = strtol(s, NULL, 10);
    return (int)v;
}

struct ubus_str_ctx {
    const char *key;
    char *out;
    size_t outsz;
};

static void ubus_str_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
    struct ubus_str_ctx *ctx = req->priv;
    struct blob_attr *cur;
    int rem;

    if (!msg || !ctx)
        return;
    blob_for_each_attr(cur, msg, rem) {
        if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING)
            continue;
        if (strcmp(blobmsg_name(cur), ctx->key) == 0) {
            snprintf(ctx->out, ctx->outsz, "%s", blobmsg_get_string(cur));
            return;
        }
    }
}

/* Call <object>.<method> and pull one top-level string out of the reply. */
static bool ubus_get_string(struct ubus_context *ctx, const char *object,
                            const char *method, const char *arg_name,
                            const char *arg_value, const char *key,
                            char *out, size_t outsz)
{
    struct blob_buf b = {0};
    struct ubus_str_ctx sctx = { .key = key, .out = out, .outsz = outsz };
    uint32_t id;
    bool ok = false;

    out[0] = '\0';
    if (ubus_lookup_id(ctx, object, &id) != 0)
        return false;

    blob_buf_init(&b, 0);
    if (arg_name && arg_value)
        blobmsg_add_string(&b, arg_name, arg_value);
    if (ubus_invoke(ctx, id, method, b.head, ubus_str_cb, &sctx, 3000) == 0)
        ok = out[0] != '\0';
    blob_buf_free(&b);
    return ok;
}

static bool read_sysfs_mac(const char *ifname, char *out, size_t outsz)
{
    char path[128];
    char raw[32];
    FILE *f;
    bool ok = false;

    if (!ifname || !*ifname)
        return false;
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);
    f = fopen(path, "re");
    if (!f)
        return false;
    if (fgets(raw, sizeof(raw), f)) {
        raw[strcspn(raw, "\r\n")] = '\0';
        ok = ora_mac_normalize(raw, out, outsz) &&
             strcmp(out, "00-00-00-00-00-00") != 0;
    }
    fclose(f);
    return ok;
}

static bool resolve_interface_device(struct ubus_context *ctx, const char *name,
                                     char *out, size_t outsz)
{
    char path[128];
    char object[96];

    snprintf(path, sizeof(path), "/sys/class/net/%s", name);
    if (access(path, F_OK) == 0) {
        snprintf(out, outsz, "%s", name);
        return true;
    }
    if (!ctx)
        return false;

    snprintf(object, sizeof(object), "network.interface.%s", name);
    return ubus_get_string(ctx, object, "status", NULL, NULL,
                           "l3_device", out, outsz) ||
           ubus_get_string(ctx, object, "status", NULL, NULL,
                           "device", out, outsz);
}

/* Resolve the device backing the LAN interface. "lan" is a logical
 * interface name, so network.device does not know it; the device name
 * has to come from network.interface.lan first. */
static bool get_board_mac(char *out, size_t outsz)
{
    static const char *fallback_ifaces[] = { "br-lan", "eth0", "lan" };
    struct ubus_context *ctx;
    char device[64] = {0};
    size_t i;

    ctx = ubus_connect(NULL);
    if (ctx) {
        if (ubus_get_string(ctx, "network.interface.lan", "status", NULL, NULL,
                            "l3_device", device, sizeof(device)) ||
            ubus_get_string(ctx, "network.interface.lan", "status", NULL, NULL,
                            "device", device, sizeof(device))) {
            char mac[32];

            if (ubus_get_string(ctx, "network.device", "status", "name", device,
                                "macaddr", mac, sizeof(mac)) &&
                ora_mac_normalize(mac, out, outsz)) {
                ubus_free(ctx);
                return true;
            }
        }
        ubus_free(ctx);
    }

    if (device[0] && read_sysfs_mac(device, out, outsz))
        return true;
    for (i = 0; i < sizeof(fallback_ifaces) / sizeof(fallback_ifaces[0]); i++)
        if (read_sysfs_mac(fallback_ifaces[i], out, outsz))
            return true;
    return false;
}

bool ora_config_load(struct ora_config *cfg)
{
    struct uci_context *uctx = uci_alloc_context();
    struct uci_element *e;
    bool ok = false;

    memset(cfg, 0, sizeof(*cfg));
    cfg->wireless = -1;
    cfg->controller_port = ORA_MGMT_HTTPS_PORT;
    cfg->adopt_port = ORA_MANAGER_V2_TCP_PORT;
    cfg->inform_interval = ORA_INFORM_INTERVAL_S;
    cfg->log_level = ORA_LOG_INFO;
    cfg->state_file[0] = '\0';
    snprintf(cfg->default_wan_if, sizeof(cfg->default_wan_if), "wan");
    snprintf(cfg->default_lan_if, sizeof(cfg->default_lan_if), "lan");

    if (!uctx) {
        ora_log(ORA_LOG_ERR, "uci_alloc_context failed");
        return false;
    }

    /* walk the package sections directly */
    struct uci_package *pkg = NULL;
    if (uci_load(uctx, "orouteragent", &pkg) != 0 || !pkg) {
        ora_log(ORA_LOG_ERR, "cannot load UCI package orouteragent");
        goto out;
    }

    uci_foreach_element(&pkg->sections, e) {
        struct uci_section *s = uci_to_section(e);

        if (strcmp(s->type, "agent") == 0) {
            cfg->enabled = !!uci_int(0, uci_lookup_option(uctx, s, "enabled"));

            char tmp[128];
            const char *model = tmp;

            uci_str(tmp, sizeof(tmp), uci_lookup_option(uctx, s, "model"));
            cfg->profile = ora_profile_lookup(model);
            /* the lookup normalizes separators, so compare against the
             * raw name only when it matches exactly; a profile found via
             * normalization is not "unknown" */
            {
                const char *pn = cfg->profile->name;
                size_t k = 0;
                bool matches = false;

                for (; model && model[k] && pn[k]; k++) {
                    if (tolower((unsigned char)model[k]) != pn[k])
                        break;
                }
                matches = model && !model[k] && !pn[k];
                cfg->profile_is_default = !model || !*model || !matches;
                if (cfg->profile_is_default && model && *model)
                    ora_log(ORA_LOG_WARN, "unknown model '%s', using %s", model,
                            cfg->profile->name);
            }

            uci_str(cfg->hw_version, sizeof(cfg->hw_version),
                    uci_lookup_option(uctx, s, "hw_version"));
            uci_str(cfg->fw_version, sizeof(cfg->fw_version),
                    uci_lookup_option(uctx, s, "fw_version"));

            char macin[32];
            uci_str(macin, sizeof(macin), uci_lookup_option(uctx, s, "mac"));
            if (*macin && ora_mac_normalize(macin, cfg->mac, sizeof(cfg->mac))) {
                cfg->mac_from_board = false;
            } else if (*macin) {
                ora_log(ORA_LOG_WARN, "invalid mac '%s', using board MAC", macin);
            }

            int w = uci_int(-1, uci_lookup_option(uctx, s, "wireless"));
            cfg->wireless = w;

            uci_str(cfg->controller, sizeof(cfg->controller),
                    uci_lookup_option(uctx, s, "controller"));
            cfg->controller_port = uci_int(cfg->controller_port,
                    uci_lookup_option(uctx, s, "controller_port"));
            cfg->adopt_port = uci_int(cfg->adopt_port,
                    uci_lookup_option(uctx, s, "adopt_port"));
            cfg->inform_interval = uci_int(cfg->inform_interval,
                    uci_lookup_option(uctx, s, "inform_interval"));
            cfg->verify_tls = !!uci_int(0, uci_lookup_option(uctx, s, "verify_tls"));

            uci_str(cfg->device_username, sizeof(cfg->device_username),
                    uci_lookup_option(uctx, s, "device_username"));
            if (!cfg->device_username[0])
                snprintf(cfg->device_username, sizeof(cfg->device_username), "admin");
            uci_str(cfg->device_password, sizeof(cfg->device_password),
                    uci_lookup_option(uctx, s, "device_password"));
            if (!cfg->device_password[0])
                snprintf(cfg->device_password, sizeof(cfg->device_password), "admin");
            uci_str(cfg->managed_username, sizeof(cfg->managed_username),
                    uci_lookup_option(uctx, s, "managed_username"));
            uci_str(cfg->managed_password, sizeof(cfg->managed_password),
                    uci_lookup_option(uctx, s, "managed_password"));

            uci_str(cfg->state_file, sizeof(cfg->state_file),
                    uci_lookup_option(uctx, s, "state_file"));
            if (!cfg->state_file[0])
                snprintf(cfg->state_file, sizeof(cfg->state_file),
                         "/etc/orouteragent/state.json");
            cfg->max_config_kb = (size_t)uci_int(512,
                    uci_lookup_option(uctx, s, "max_config_kb"));
            cfg->log_level = uci_int(cfg->log_level,
                    uci_lookup_option(uctx, s, "log_level"));

            char dm[16];
            uci_str(dm, sizeof(dm), uci_lookup_option(uctx, s, "discovery_mode"));
            if (!strcmp(dm, "unicast"))
                cfg->discovery_mode = ORA_DISC_UNICAST;
            else if (!strcmp(dm, "broadcast"))
                cfg->discovery_mode = ORA_DISC_BROADCAST;
            else
                cfg->discovery_mode = ORA_DISC_AUTO;
        } else if (strcmp(s->type, "portmap") == 0) {
            if (cfg->n_portmap < sizeof(cfg->portmap) / sizeof(cfg->portmap[0])) {
                struct ora_portmap_entry *pm =
                    &cfg->portmap[cfg->n_portmap];
                pm->index = uci_int(0, uci_lookup_option(uctx, s, "index"));
                uci_str(pm->interface, sizeof(pm->interface),
                        uci_lookup_option(uctx, s, "interface"));
                if (pm->index > 0 && pm->interface[0])
                    cfg->n_portmap++;
            }
        }
    }

    /* defaults depending on profile */
    if (!cfg->hw_version[0])
        snprintf(cfg->hw_version, sizeof(cfg->hw_version), "%s", cfg->profile->hw_ver);
    if (!cfg->fw_version[0])
        snprintf(cfg->fw_version, sizeof(cfg->fw_version), "%s", cfg->profile->fw_ver);
    if (cfg->inform_interval < 1)
        cfg->inform_interval = 1;

    {
        struct ubus_context *ctx = ubus_connect(NULL);
        char resolved[32];
        size_t i;

        if (resolve_interface_device(ctx, "lan", resolved, sizeof(resolved)))
            snprintf(cfg->default_lan_if, sizeof(cfg->default_lan_if),
                     "%s", resolved);
        if (resolve_interface_device(ctx, "wan", resolved, sizeof(resolved))) {
            snprintf(cfg->default_wan_if, sizeof(cfg->default_wan_if),
                     "%s", resolved);
        } else {
            snprintf(cfg->default_wan_if, sizeof(cfg->default_wan_if),
                     "%s", cfg->default_lan_if);
        }
        for (i = 0; i < cfg->n_portmap; i++) {
            if (resolve_interface_device(ctx, cfg->portmap[i].interface,
                                         resolved, sizeof(resolved)))
                snprintf(cfg->portmap[i].interface,
                         sizeof(cfg->portmap[i].interface), "%s", resolved);
        }
        if (ctx)
            ubus_free(ctx);
    }

    /* board MAC fallback */
    if (!cfg->mac[0]) {
        if (!get_board_mac(cfg->mac, sizeof(cfg->mac))) {
            ora_log(ORA_LOG_WARN, "no MAC configured and board lookup failed; using 02:00:00:00:00:01");
            snprintf(cfg->mac, sizeof(cfg->mac), "02-00-00-00-00-01");
        } else {
            cfg->mac_from_board = true;
        }
    }

    ok = true;
out:
    if (uctx)
        uci_free_context(uctx);
    return ok;
}

const char *ora_config_port_ifname(const struct ora_config *cfg, int emu_port)
{
    size_t i;

    for (i = 0; i < cfg->n_portmap; i++)
        if (cfg->portmap[i].index == emu_port)
            return cfg->portmap[i].interface;
    /* built-in fallback: port 1 -> WAN device, others -> LAN device */
    if (emu_port == 1)
        return cfg->default_wan_if;
    return cfg->default_lan_if;
}

void ora_config_free(struct ora_config *cfg)
{
    (void)cfg;
}
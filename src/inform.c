/* orouteragent - INFORM body builder
 *
 * Produces the periodic INFORM body from REAL router state (/proc, /sys,
 * netlink, dnsmasq leases) mapped onto the emulated model's port layout.
 * Sections the router cannot observe are reported empty rather than
 * fabricated; sections driven by controller config are derived from the
 * stored SET blobs.
 */
#include "inform.h"
#include "netlink.h"
#include "protocol/message.h"
#include "system_info.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* ---------- helpers ---------- */

static json_object *jobj_new(void)
{
    return json_object_new_object();
}

static void jadd_str(json_object *o, const char *k, const char *v)
{
    json_object_object_add(o, k, json_object_new_string(v ? v : ""));
}

static void jadd_int(json_object *o, const char *k, int64_t v)
{
    json_object_object_add(o, k, json_object_new_int64(v));
}

static void jadd_bool(json_object *o, const char *k, bool v)
{
    json_object_object_add(o, k, json_object_new_boolean(v));
}

/* Parse a stored SET blob into a fresh object (caller puts it), or NULL
 * when the controller never pushed that key. */
static json_object *stored_cfg(struct ora_state *st, const char *key)
{
    json_object *b = st->set_blobs ? json_object_object_get(st->set_blobs, key) : NULL;
    const char *s = b ? json_object_get_string(b) : NULL;

    return s ? json_tokener_parse(s) : NULL;
}

/* Borrowed array member of a stored config, e.g. staticRouting.staticRoutings. */
static json_object *cfg_array(json_object *cfg, const char *key)
{
    json_object *a = cfg ? json_object_object_get(cfg, key) : NULL;

    return (a && json_object_is_type(a, json_type_array)) ? a : NULL;
}

/* The real interface backing an emulated port. */
static const char *port_if(const struct ora_config *cfg, int port)
{
    return ora_config_port_ifname(cfg, port);
}

static int port_mode(const struct ora_port_info *cap, json_object *wan_ports)
{
    size_t i;

    if (cap->type != 1 || !wan_ports)
        return cap->mode;
    for (i = 0; i < json_object_array_length(wan_ports); i++) {
        json_object *port = json_object_array_get_idx(wan_ports, i);

        if (json_object_get_int(port) == cap->port)
            return 0;
    }
    return 1;
}

/* ---------- deviceInfo ---------- */

/* Negotiation deviceInfo: strict subset — INFORM-only fields
 * (sm/fac/temp/txRate/rxRate/ipv6List...) must NOT appear here;
 * leaking them stalls the handshake. */
json_object *ora_inform_negotiation_device_info(const struct ora_config *cfg)
{
    json_object *di = jobj_new();
    char ipbuf[INET_ADDRSTRLEN];
    char hwver[64];
    const struct ora_model_profile *p = cfg->profile;
    char wanmac[24], m[24];
    int i, nwan;

    ora_sys_device_ip(cfg->controller[0] ? cfg->controller : NULL, ipbuf, sizeof(ipbuf));
    snprintf(hwver, sizeof(hwver), "%s v%s", p->model, cfg->hw_version);

    jadd_str(di, "model", p->model);
    jadd_str(di, "modelVer", p->model_ver);
    jadd_str(di, "fwVer", cfg->fw_version);
    jadd_str(di, "hwVer", hwver);
    jadd_str(di, "ip", ipbuf);
    jadd_str(di, "mask", "255.255.255.0");
    jadd_str(di, "lanMac", cfg->mac);
    jadd_int(di, "modelId", 0);
    jadd_str(di, "hwId", p->hw_id);
    jadd_str(di, "encryptedHwId", p->encrypted_hw_id);
    jadd_str(di, "oemId", p->oem_id);
    jadd_str(di, "encryptedOemId", p->encrypted_oem_id);
    jadd_int(di, "cu", ora_sys_cpu_percent());
    jadd_int(di, "mu", ora_sys_mem_percent());

    {
        json_object *speeds = json_object_new_array();

        json_object_array_add(speeds, json_object_new_int(1));
        json_object_array_add(speeds, json_object_new_int(2));
        json_object_array_add(speeds, json_object_new_int(3));
        json_object_object_add(di, "speeds", speeds);
    }

    {
        json_object *arr = json_object_new_array();

        nwan = p->port_num - 1;
        if (nwan > 8)
            nwan = 8;
        snprintf(m, sizeof(m), "%s", cfg->mac);
        for (i = 0; i < nwan; i++) {
            json_object *e = jobj_new();

            ora_mac_increment(m, wanmac, sizeof(wanmac));
            snprintf(m, sizeof(m), "%s", wanmac);
            jadd_str(e, "defMac", m);
            jadd_int(e, "portId", i + 1);
            json_object_array_add(arr, e);
        }
        json_object_object_add(di, "wanDefaultMacs", arr);
    }
    json_object_object_add(di, "extraWanDefaultMacs", json_object_new_array());

    return di;
}

json_object *ora_inform_device_info(const struct ora_config *cfg)
{
    json_object *di = ora_inform_negotiation_device_info(cfg);
    struct ora_link_state wan;
    char up[64];

    ora_format_uptime(ora_uptime_s(), up, sizeof(up));
    ora_sys_link_state(port_if(cfg, 1), &wan);

    jadd_str(di, "cerVer", "1.0");
    jadd_str(di, "time", up);
    jadd_int(di, "sm", 0);
    jadd_bool(di, "fac", false);
    jadd_int(di, "txRate", wan.up ? (int64_t)wan.speed * 1000000 : 0);
    jadd_int(di, "rxRate", wan.up ? (int64_t)wan.speed * 1000000 : 0);

    {
        json_object *v6 = json_object_new_array();

        if (wan.ip6[0])
            json_object_array_add(v6, json_object_new_string(wan.ip6));
        json_object_object_add(di, "ipv6List", v6);
    }
    return di;
}

/* ---------- portInfo ---------- */

static json_object *port_info_section(const struct ora_config *cfg,
                                      struct ora_state *st)
{
    json_object *inner = jobj_new();
    json_object *arr = json_object_new_array();
    json_object *wan_cfg = stored_cfg(st, "wanBasicSetting");
    json_object *wan_ports = cfg_array(wan_cfg, "wanPorts");
    const struct ora_model_profile *p = cfg->profile;
    char pri_dns[64], snd_dns[64];
    struct ora_route defroute;
    bool have_def = ora_nl_default_route(&defroute);
    size_t i;

    ora_sys_dns(pri_dns, sizeof(pri_dns), snd_dns, sizeof(snd_dns));

    for (i = 0; i < p->n_ports; i++) {
        const struct ora_port_info *cap = &p->ports[i];
        struct ora_link_state ls;
        int mode = port_mode(cap, wan_ports);
        bool is_wan = mode == 0;
        json_object *e = jobj_new();

        ora_sys_link_state(port_if(cfg, cap->port), &ls);

        jadd_int(e, "port", cap->port);
        jadd_int(e, "physicalType", 0);
        jadd_str(e, "name", cap->name);
        jadd_int(e, "mode", mode);
        jadd_str(e, "mac", cfg->mac);
        jadd_int(e, "status", ls.up ? 1 : 0);
        jadd_int(e, "speed", ls.up ? (ls.speed ? ls.speed : 1000) : 0);
        jadd_int(e, "duplex", ls.up ? 1 : 0);
        /* These fields are mandatory for controller port-state handling. */
        jadd_int(e, "internetState", (is_wan && ls.up && have_def) ? 1 : 0);
        jadd_int(e, "internetV6", (is_wan && ls.ip6[0]) ? 1 : 0);
        jadd_int(e, "latency", ls.latency >= 0 ? ls.latency : 0);

        if (is_wan) {
            json_object *ip4 = jobj_new();

            jadd_str(e, "ip", ls.ip[0] ? ls.ip : "0.0.0.0");
            jadd_str(e, "netmask", ls.netmask[0] ? ls.netmask : "255.255.255.0");
            jadd_int(e, "publicWanIp", 1);

            jadd_str(ip4, "gw", have_def ? defroute.next_hop : "0.0.0.0");
            jadd_str(ip4, "gw2", "");
            jadd_str(ip4, "priDns", pri_dns);
            jadd_str(ip4, "sndDns", snd_dns);
            jadd_str(ip4, "priDns2", "");
            jadd_str(ip4, "sndDns2", "");
            json_object_object_add(e, "ip4", ip4);

            if (p->supports_ipv6 && ls.ip6[0]) {
                json_object *ip6 = jobj_new();
                char prefix[8];

                snprintf(prefix, sizeof(prefix), "%d", ls.ip6_prefix);
                jadd_str(e, "ip2", ls.ip6);
                jadd_str(e, "netmask2", "");
                jadd_str(ip6, "addr", ls.ip6);
                jadd_str(ip6, "gw", "");
                jadd_str(ip6, "priDns", "");
                jadd_str(ip6, "sndDns", "");
                jadd_str(ip6, "prefix", prefix);
                json_object_object_add(e, "ip6", ip6);
            }
        }
        json_object_array_add(arr, e);
    }
    if (wan_cfg)
        json_object_put(wan_cfg);
    json_object_object_add(inner, "portInfos", arr);
    return inner;
}

/* ---------- trafficStat ---------- */

/* Previous counter sample per emulated port, for rate calculation. */
struct rate_sample {
    uint64_t rx_bytes, tx_bytes, at_ms;
    bool valid;
};
static struct rate_sample g_rate[32];

static json_object *traffic_section(const struct ora_config *cfg)
{
    json_object *inner = jobj_new();
    json_object *arr = json_object_new_array();
    const struct ora_model_profile *p = cfg->profile;
    uint64_t now = ora_now_ms();
    size_t i;

    for (i = 0; i < p->n_ports && i < sizeof(g_rate) / sizeof(g_rate[0]); i++) {
        const struct ora_port_info *cap = &p->ports[i];
        const char *ifname = port_if(cfg, cap->port);
        struct ora_link_state ls;
        struct ora_traffic t;
        struct rate_sample *prev = &g_rate[i];
        uint64_t rx_r = 0, tx_r = 0;
        json_object *e;

        ora_sys_link_state(ifname, &ls);
        if (!ls.up && cap->mode != 0)
            continue; /* the controller only expects linked ports + WAN */

        ora_sys_traffic_iface(ifname, &t);
        if (prev->valid && now > prev->at_ms) {
            uint64_t dt = now - prev->at_ms;

            rx_r = ora_sys_rate_bps(t.rx_bytes, prev->rx_bytes, dt);
            tx_r = ora_sys_rate_bps(t.tx_bytes, prev->tx_bytes, dt);
        }
        prev->rx_bytes = t.rx_bytes;
        prev->tx_bytes = t.tx_bytes;
        prev->at_ms = now;
        prev->valid = true;

        e = jobj_new();
        jadd_int(e, "port", cap->port);
        jadd_int(e, "physicalType", 0);
        jadd_int(e, "rx", (int64_t)t.rx_bytes);
        jadd_int(e, "tx", (int64_t)t.tx_bytes);
        jadd_int(e, "rxP", (int64_t)t.rx_pkts);
        jadd_int(e, "txP", (int64_t)t.tx_pkts);
        jadd_int(e, "rxR", (int64_t)rx_r);
        jadd_int(e, "txR", (int64_t)tx_r);
        jadd_int(e, "rxErrPkt", (int64_t)t.rx_errs);
        jadd_int(e, "txErrPkt", (int64_t)t.tx_errs);
        jadd_int(e, "errPkt", (int64_t)(t.rx_errs + t.tx_errs));
        jadd_int(e, "lossPkt", (int64_t)(t.rx_drop + t.tx_drop));
        json_object_array_add(arr, e);
    }
    json_object_object_add(inner, "trafficStats", arr);
    return inner;
}

/* ---------- client / dhcpClient / arp ---------- */

static json_object *client_section(const struct ora_config *cfg)
{
    json_object *inner = jobj_new();
    json_object *arr = json_object_new_array();
    json_object *leases = ora_sys_dhcp_clients();
    size_t i, n;

    n = leases ? json_object_array_length(leases) : 0;
    for (i = 0; i < n; i++) {
        json_object *l = json_object_array_get_idx(leases, i);
        json_object *c = jobj_new();

        jadd_str(c, "mac", ora_json_get_str(l, "mac", ""));
        jadd_str(c, "name", ora_json_get_str(l, "host", ""));
        jadd_str(c, "ip", ora_json_get_str(l, "ip", ""));
        jadd_int(c, "vid", 1);
        jadd_int(c, "time", 0);
        jadd_int(c, "rx", 0);
        jadd_int(c, "rxP", 0);
        jadd_int(c, "tx", 0);
        jadd_int(c, "txP", 0);
        jadd_int(c, "txT", 0);
        jadd_int(c, "firstSeen", (int64_t)ora_now_ms());
        jadd_int(c, "authed", 1);
        jadd_int(c, "port", cfg->profile->port_num);
        json_object_array_add(arr, c);
    }
    if (leases)
        json_object_put(leases);
    json_object_object_add(inner, "clients", arr);
    return inner;
}

static json_object *dhcp_section(void)
{
    json_object *inner = jobj_new();
    json_object *arr = json_object_new_array();
    json_object *leases = ora_sys_dhcp_clients();
    size_t i, n;

    n = leases ? json_object_array_length(leases) : 0;
    for (i = 0; i < n; i++) {
        json_object *l = json_object_array_get_idx(leases, i);
        json_object *c = jobj_new();

        jadd_str(c, "name", ora_json_get_str(l, "host", "*"));
        jadd_str(c, "ip", ora_json_get_str(l, "ip", ""));
        jadd_str(c, "mac", ora_json_get_str(l, "mac", ""));
        jadd_int(c, "leaseTime", 7200);
        json_object_array_add(arr, c);
    }
    if (leases)
        json_object_put(leases);
    json_object_object_add(inner, "clients", arr);
    return inner;
}

static json_object *arp_section(const struct ora_config *cfg)
{
    json_object *inner = jobj_new();
    json_object *arr = json_object_new_array();
    json_object *arps = ora_sys_arp();
    size_t i, n;

    n = arps ? json_object_array_length(arps) : 0;
    for (i = 0; i < n; i++) {
        json_object *a = json_object_array_get_idx(arps, i);
        json_object *e = jobj_new();

        jadd_str(e, "mac", ora_json_get_str(a, "mac", ""));
        jadd_str(e, "ip", ora_json_get_str(a, "ip", ""));
        jadd_int(e, "port", cfg->profile->port_num);
        jadd_int(e, "vlan", 1);
        json_object_array_add(arr, e);
    }
    if (arps)
        json_object_put(arps);
    json_object_object_add(inner, "arps", arr);
    return inner;
}

/* ---------- routingTable ---------- */

/* Controller interface id -> routing-table interface name: port 1 is the
 * WAN, other ports are LAN interfaces. */
static const char *interface_name(int port, char *buf, size_t bufsz)
{
    if (port <= 1)
        snprintf(buf, bufsz, "wan1");
    else
        snprintf(buf, bufsz, "lan%d", port - 1);
    return buf;
}

static void add_route_entry(json_object *arr, int id, json_object *dest_list,
                            const char *next_hop, const char *ifname, int metric)
{
    json_object *e = jobj_new();

    jadd_int(e, "id", id);
    json_object_object_add(e, "destIp", dest_list);
    jadd_str(e, "nextHop", next_hop);
    jadd_str(e, "interfaceName", ifname);
    jadd_int(e, "metric", metric);
    json_object_array_add(arr, e);
}

static json_object *single_dest(const char *dest)
{
    json_object *l = json_object_new_array();

    json_object_array_add(l, json_object_new_string(dest));
    return l;
}

/* Operator routes echoed from a stored staticRouting/policyRouting SET. */
static void append_cfg_routes(json_object *arr, struct ora_state *st,
                              const char *cfg_key, const char *list_key, int *id)
{
    json_object *conf = stored_cfg(st, cfg_key);
    json_object *list = cfg_array(conf, list_key);
    char ifbuf[32];
    size_t i, n;

    n = list ? json_object_array_length(list) : 0;
    for (i = 0; i < n; i++) {
        json_object *entry = json_object_array_get_idx(list, i);
        json_object *dests;
        json_object *dl;

        if (!json_object_is_type(entry, json_type_object))
            continue;
        dests = cfg_array(entry, "destinations");
        if (dests) {
            size_t j;

            dl = json_object_new_array();
            for (j = 0; j < json_object_array_length(dests); j++)
                json_object_array_add(dl, json_object_new_string(
                    json_object_get_string(json_object_array_get_idx(dests, j))));
        } else {
            dl = single_dest("0.0.0.0/0");
        }
        add_route_entry(arr, (int)ora_json_get_int(entry, "id", ++(*id)), dl,
                        ora_json_get_str(entry, "nextHopIp", "0.0.0.0"),
                        interface_name((int)ora_json_get_int(entry, "interface", 1),
                                       ifbuf, sizeof(ifbuf)),
                        (int)ora_json_get_int(entry, "metric", 1));
    }
    if (conf)
        json_object_put(conf);
}

static json_object *routing_table_section(struct ora_state *st)
{
    json_object *inner = jobj_new();
    json_object *arr = json_object_new_array();
    struct ora_route routes[64];
    size_t n = ora_nl_routes(routes, sizeof(routes) / sizeof(routes[0]));
    size_t i;
    int id = 0;

    for (i = 0; i < n; i++)
        add_route_entry(arr, ++id, single_dest(routes[i].dest),
                        routes[i].next_hop, routes[i].ifname, routes[i].metric);

    append_cfg_routes(arr, st, "staticRouting", "staticRoutings", &id);
    append_cfg_routes(arr, st, "policyRouting", "policyRoutings", &id);

    json_object_object_add(inner, "routingTables", arr);
    return inner;
}

/* ---------- misc real-state sections ---------- */

static json_object *ct_table_section(void)
{
    json_object *inner = jobj_new();
    int64_t count, max;

    ora_sys_conntrack(&count, &max);
    jadd_int(inner, "ctMax", max);
    jadd_int(inner, "ctNum", count);
    return inner;
}

static json_object *network_traffic_section(const struct ora_config *cfg)
{
    json_object *inner = jobj_new();
    json_object *arr = json_object_new_array();
    json_object *e = jobj_new();
    struct ora_traffic t;
    struct ora_link_state lan;
    const char *lanif = port_if(cfg, cfg->profile->port_num);
    char net[32];
    json_object *leases;
    int64_t lease_count;

    ora_sys_traffic_iface(lanif, &t);
    ora_sys_link_state(lanif, &lan);

    net[0] = '\0';
    if (lan.ip[0] && lan.netmask[0]) {
        uint32_t ip, mask;

        if (ora_ip4_parse(lan.ip, &ip) && ora_ip4_parse(lan.netmask, &mask))
            ora_ip4_format(ip & mask, net, sizeof(net));
    }
    leases = ora_sys_dhcp_clients();
    lease_count = leases ? (int64_t)json_object_array_length(leases) : 0;
    if (leases)
        json_object_put(leases);

    jadd_str(e, "ip", net[0] ? net : "0.0.0.0");
    jadd_str(e, "ip6", lan.ip6);
    jadd_int(e, "rx", (int64_t)t.rx_bytes);
    jadd_int(e, "tx", (int64_t)t.tx_bytes);
    jadd_int(e, "vlan", 1);
    jadd_int(e, "dhcpsUtil", lease_count);
    jadd_int(e, "dhcps6Util", 0);
    jadd_int(e, "dhcpsOffer", lease_count);
    jadd_int(e, "dhcps6Offer", 0);
    json_object_array_add(arr, e);

    json_object_object_add(inner, "networkTraffics", arr);
    return inner;
}

/* ---------- config-driven sections ---------- */

static json_object *ddns_section(struct ora_state *st)
{
    json_object *inner = jobj_new();
    json_object *arr = json_object_new_array();
    json_object *conf = stored_cfg(st, "ddns");
    json_object *list = cfg_array(conf, "ddnss");
    size_t i, n;

    n = list ? json_object_array_length(list) : 0;
    for (i = 0; i < n; i++) {
        json_object *c = json_object_array_get_idx(list, i);
        json_object *e = jobj_new();
        json_object *domains = json_object_new_array();
        json_object *d = cfg_array(c, "domain");

        if (d) {
            size_t j;

            for (j = 0; j < json_object_array_length(d); j++)
                json_object_array_add(domains, json_object_new_string(
                    json_object_get_string(json_object_array_get_idx(d, j))));
        }
        jadd_int(e, "id", ora_json_get_int(c, "id", (int64_t)i + 1));
        json_object_object_add(e, "domain", domains);
        jadd_int(e, "interface", ora_json_get_int(c, "interface", 1));
        jadd_int(e, "status", 1);
        jadd_str(e, "statusMsg", "");
        jadd_int(e, "lastUpdated", (int64_t)ora_uptime_s());
        json_object_array_add(arr, e);
    }
    if (conf)
        json_object_put(conf);
    json_object_object_add(inner, "ddnss", arr);
    return inner;
}

static json_object *portforward_section(struct ora_state *st)
{
    json_object *inner = jobj_new();
    json_object *users = json_object_new_array();
    json_object *conf = stored_cfg(st, "portforward");
    json_object *list = cfg_array(conf, "settings");
    size_t i, n;

    n = list ? json_object_array_length(list) : 0;
    for (i = 0; i < n; i++) {
        json_object *c = json_object_array_get_idx(list, i);
        json_object *e = jobj_new();
        json_object *infa = json_object_new_array();
        char port_s[16];

        json_object_array_add(infa, json_object_new_int(
            (int)ora_json_get_int(c, "interface", 1)));

        jadd_int(e, "id", ora_json_get_int(c, "id", (int64_t)i + 1));
        jadd_int(e, "proto", ora_json_get_int(c, "protocol", 0));
        json_object_object_add(e, "infa", infa);
        snprintf(port_s, sizeof(port_s), "%" PRId64,
                 ora_json_get_int(c, "externalPort", 0));
        jadd_str(e, "export", port_s);
        jadd_str(e, "inip", ora_json_get_str(c, "ipaddr", ""));
        snprintf(port_s, sizeof(port_s), "%" PRId64,
                 ora_json_get_int(c, "internalPort", 0));
        jadd_str(e, "inport", port_s);
        jadd_int(e, "bts", 0);
        jadd_int(e, "pkts", 0);
        jadd_int(e, "dura", 0);
        json_object_array_add(users, e);
    }
    if (conf)
        json_object_put(conf);
    json_object_object_add(inner, "users", users);
    json_object_object_add(inner, "upnps", json_object_new_array());
    return inner;
}

/* Hit counters need per-rule accounting the agent does not install yet. */
static json_object *acl_hit_section(struct ora_state *st)
{
    json_object *arr = json_object_new_array();
    json_object *conf = stored_cfg(st, "acl");
    json_object *list = cfg_array(conf, "rules");
    size_t i, n;

    if (!list)
        list = cfg_array(conf, "acls");
    n = list ? json_object_array_length(list) : 0;
    for (i = 0; i < n; i++) {
        json_object *c = json_object_array_get_idx(list, i);
        json_object *e;

        if (!json_object_is_type(c, json_type_object))
            continue;
        e = jobj_new();
        jadd_int(e, "id", ora_json_get_int(c, "id", 0));
        jadd_int(e, "hitCount", 0);
        json_object_array_add(arr, e);
    }
    if (conf)
        json_object_put(conf);
    return arr;
}

static json_object *qos_section(const struct ora_config *cfg, struct ora_state *st)
{
    json_object *inner = jobj_new();
    json_object *data = json_object_new_array();
    json_object *conf = stored_cfg(st, "qos");
    json_object *rules = cfg_array(conf, "classRules");
    const struct ora_model_profile *p = cfg->profile;
    size_t i;

    for (i = 0; i < p->n_ports; i++) {
        const struct ora_port_info *cap = &p->ports[i];
        struct ora_link_state ls;
        json_object *e, *thr, *voip;
        size_t c, ncls;

        ora_sys_link_state(port_if(cfg, cap->port), &ls);
        if (!ls.up && cap->port != 1)
            continue;

        e = jobj_new();
        thr = json_object_new_array();
        ncls = rules ? json_object_array_length(rules) : 3;
        for (c = 0; c < ncls; c++) {
            json_object *t = jobj_new();
            int64_t cls = (int64_t)c;

            if (rules) {
                json_object *r = json_object_array_get_idx(rules, c);

                cls = ora_json_get_int(r, "class", (int64_t)c);
            }
            jadd_int(t, "class", cls);
            jadd_int(t, "inbound", 0);
            jadd_int(t, "outbound", 0);
            json_object_array_add(thr, t);
        }
        voip = jobj_new();
        jadd_int(voip, "inbound", 0);
        jadd_int(voip, "outbound", 0);

        jadd_int(e, "port", cap->port);
        json_object_object_add(e, "throughputs", thr);
        json_object_object_add(e, "voip", voip);
        json_object_array_add(data, e);
    }
    if (conf)
        json_object_put(conf);
    json_object_object_add(inner, "data", data);
    return inner;
}

/* An unconfigured router has no tunnels; none are invented. */
static json_object *vpn_section(void)
{
    json_object *inner = jobj_new();

    json_object_object_add(inner, "ipsec", json_object_new_array());
    json_object_object_add(inner, "openvpn", json_object_new_array());
    json_object_object_add(inner, "pptpAndL2tp", json_object_new_array());
    return inner;
}

static json_object *ssl_vpn_section(void)
{
    json_object *inner = jobj_new();

    json_object_object_add(inner, "sslVpnServers", json_object_new_array());
    return inner;
}

static json_object *wireguard_section(void)
{
    json_object *inner = jobj_new();

    json_object_object_add(inner, "wireguards", json_object_new_array());
    return inner;
}

/* ---------- model-gated sections ---------- */

static json_object *sdwan_section(void)
{
    json_object *inner = jobj_new();

    json_object_object_add(inner, "tuns", json_object_new_array());
    return inner;
}

static json_object *virtual_wan_section(const struct ora_config *cfg)
{
    json_object *inner = jobj_new();
    json_object *arr = json_object_new_array();
    const struct ora_model_profile *p = cfg->profile;
    char mac[24], next[24];
    struct ora_route defroute;
    bool have_def = ora_nl_default_route(&defroute);
    char pri_dns[64], snd_dns[64];
    size_t i;
    int64_t nwan = 0;

    ora_sys_dns(pri_dns, sizeof(pri_dns), snd_dns, sizeof(snd_dns));
    snprintf(mac, sizeof(mac), "%s", cfg->mac);

    for (i = 0; i < p->n_ports; i++) {
        const struct ora_port_info *cap = &p->ports[i];
        struct ora_link_state ls;
        json_object *e, *ipv4;

        if (cap->type == 2) /* LAN-only ports are not WANs */
            continue;
        ora_mac_increment(mac, next, sizeof(next));
        snprintf(mac, sizeof(mac), "%s", next);
        ora_sys_link_state(port_if(cfg, cap->port), &ls);

        e = jobj_new();
        ipv4 = jobj_new();
        jadd_int(e, "virtualWanEntryId", ++nwan);
        jadd_str(e, "ip", ls.ip);
        jadd_str(e, "ip2", "");
        jadd_int(e, "status", ls.up ? 1 : 0);
        jadd_int(e, "internetState", (ls.up && have_def) ? 1 : 0);
        jadd_int(e, "onlineDetection", ls.up ? 1 : 0);
        jadd_str(e, "mac", mac);
        jadd_str(ipv4, "gw", have_def ? defroute.next_hop : "");
        jadd_str(ipv4, "gw2", "");
        jadd_str(ipv4, "priDns", pri_dns);
        jadd_str(ipv4, "sndDns", snd_dns);
        jadd_str(ipv4, "priDns2", "");
        jadd_str(ipv4, "sndDns2", "");
        json_object_object_add(e, "ipv4", ipv4);
        json_object_array_add(arr, e);
    }
    json_object_object_add(inner, "virtualWans", arr);
    return inner;
}

static json_object *lte_section(void)
{
    json_object *inner = jobj_new();

    json_object_object_add(inner, "selectedApns", json_object_new_array());
    json_object_object_add(inner, "selectedApns1", json_object_new_array());
    return inner;
}

static json_object *poe_section(const struct ora_config *cfg)
{
    json_object *inner = jobj_new();
    json_object *ports = json_object_new_array();
    const struct ora_model_profile *p = cfg->profile;
    size_t i;

    for (i = 0; i < p->n_ports; i++) {
        json_object *e;

        if (!p->ports[i].support_poe)
            continue;
        e = jobj_new();
        jadd_int(e, "port", p->ports[i].port);
        jadd_int(e, "state", 0);
        json_object_object_add(e, "p", json_object_new_double(0.0));
        json_object_object_add(e, "u", json_object_new_double(0.0));
        json_object_object_add(e, "i", json_object_new_double(0.0));
        json_object_array_add(ports, e);
    }
    json_object_object_add(inner, "limit", json_object_new_double(0.0));
    json_object_object_add(inner, "remain", json_object_new_double(0.0));
    json_object_object_add(inner, "percent", json_object_new_double(0.0));
    jadd_int(inner, "fan", 0);
    json_object_object_add(inner, "ports", ports);
    return inner;
}

/* Wireless sections for WiFi-capable models. Radio state is reported
 * disabled: OpenWrt WiFi is managed by the router itself, not by the
 * controller (real radio reporting is a later phase). */
static void add_wireless_sections(json_object *body)
{
    static const struct {
        int rid;
        const char *suffix;
        int ch;
        int bw;
        const char *rd_mode;
    } radios[] = {
        { 0, "2G", 6, 20, "11ng" },
        { 1, "5G", 36, 80, "11ac" },
    };
    size_t i;

    for (i = 0; i < sizeof(radios) / sizeof(radios[0]); i++) {
        char key[32];
        json_object *ws = jobj_new();
        json_object *rt = jobj_new();

        snprintf(key, sizeof(key), "wSettings_%s", radios[i].suffix);
        jadd_int(ws, "rid", radios[i].rid);
        jadd_int(ws, "ch", radios[i].ch);
        jadd_int(ws, "bw", radios[i].bw);
        jadd_int(ws, "txPower", 20);
        jadd_str(ws, "rdMode", radios[i].rd_mode);
        jadd_bool(ws, "radioEnable", false);
        json_object_object_add(body, key, ws);

        snprintf(key, sizeof(key), "radioTraffic_%s", radios[i].suffix);
        jadd_int(rt, "rid", radios[i].rid);
        jadd_int(rt, "rx", 0);
        jadd_int(rt, "tx", 0);
        jadd_int(rt, "rxRate", 0);
        jadd_int(rt, "txRate", 0);
        jadd_int(rt, "clientNum", 0);
        json_object_object_add(body, key, rt);

        snprintf(key, sizeof(key), "ssidStats_%s", radios[i].suffix);
        json_object_object_add(body, key, json_object_new_array());
    }
    {
        json_object *mesh = jobj_new();
        json_object *roaming = jobj_new();
        json_object *cand = jobj_new();

        jadd_int(mesh, "status", 0);
        jadd_int(mesh, "meshRid", 0);
        json_object_object_add(mesh, "isolatedAPs", json_object_new_array());
        json_object_object_add(mesh, "childAPs", json_object_new_array());
        /* candidateParents is an OBJECT with a parentList, not a bare
         * array: the OSG inform parser deserializes it into a
         * CandidateParents bean, and an array makes the whole INFORM
         * fail to parse (which blanks the controller's Internet page). */
        jadd_int(cand, "status", 0);
        json_object_object_add(cand, "parentList", json_object_new_array());
        json_object_object_add(mesh, "candidateParents", cand);
        json_object_object_add(body, "mesh", mesh);

        jadd_bool(roaming, "enable", false);
        jadd_int(roaming, "mode", 0);
        json_object_object_add(body, "roaming", roaming);
    }
}

/* ---------- main body assembly ---------- */

json_object *ora_inform_get_key(const struct ora_config *cfg,
                                struct ora_state *st, const char *key)
{
    if (!key)
        return NULL;

    if (!strcmp(key, "arptable"))
        return arp_section(cfg);
    if (!strcmp(key, "dhcpClient"))
        return dhcp_section();
    if (!strcmp(key, "sessionLimit"))
        return ct_table_section();
    if (!strcmp(key, "dnsCache")) {
        json_object *o = jobj_new();
        char pri[64], snd[64];
        json_object *servers = json_object_new_array();

        ora_sys_dns(pri, sizeof(pri), snd, sizeof(snd));
        if (pri[0])
            json_object_array_add(servers, json_object_new_string(pri));
        if (snd[0])
            json_object_array_add(servers, json_object_new_string(snd));
        json_object_object_add(o, "servers", servers);
        json_object_object_add(o, "entries", json_object_new_array());
        return o;
    }
    if (!strcmp(key, "dpiProtocols")) {
        json_object *o = jobj_new();

        /* no DPI engine on the router */
        json_object_object_add(o, "protocols", json_object_new_array());
        return o;
    }
    if (!strcmp(key, "radioStatus")) {
        json_object *o = jobj_new();

        json_object_object_add(o, "radios", json_object_new_array());
        return o;
    }
    (void)st;
    return NULL;
}

json_object *ora_inform_build_body(const struct ora_config *cfg,
                                   struct ora_state *st)
{
    const struct ora_model_profile *p = cfg->profile;
    json_object *body = jobj_new();

    json_object_object_add(body, "deviceInfo", ora_inform_device_info(cfg));
    jadd_int(body, "configVersion", st->config_version);

    /* observed router state */
    json_object_object_add(body, "portInfo", port_info_section(cfg, st));
    json_object_object_add(body, "trafficStat", traffic_section(cfg));
    json_object_object_add(body, "client", client_section(cfg));
    json_object_object_add(body, "dhcpClient", dhcp_section());
    json_object_object_add(body, "arp", arp_section(cfg));
    json_object_object_add(body, "routingTable", routing_table_section(st));
    json_object_object_add(body, "ctTable", ct_table_section());
    json_object_object_add(body, "networkTraffic", network_traffic_section(cfg));

    /* driven by pushed controller config */
    json_object_object_add(body, "ddns", ddns_section(st));
    json_object_object_add(body, "portforward", portforward_section(st));
    json_object_object_add(body, "aclHit", acl_hit_section(st));
    json_object_object_add(body, "qos", qos_section(cfg, st));
    json_object_object_add(body, "vpn", vpn_section());
    json_object_object_add(body, "sslVpn", ssl_vpn_section());
    json_object_object_add(body, "wireguard", wireguard_section());

    /* not observable on generic OpenWrt hardware */
    {
        json_object *o = jobj_new();

        json_object_object_add(o, "access", json_object_new_array());
        json_object_object_add(o, "dev", json_object_new_array());
        json_object_object_add(body, "abnormalDt", o);
    }
    json_object_object_add(body, "eventInform", json_object_new_array());
    {
        json_object *o = jobj_new();

        json_object_object_add(o, "traffic", json_object_new_array());
        json_object_object_add(o, "block", json_object_new_array());
        json_object_object_add(body, "applicationsTraffic", o);
    }
    {
        json_object *o = jobj_new();

        json_object_object_add(o, "portalDurations", json_object_new_array());
        json_object_object_add(body, "portalDuration", o);
    }
    {
        json_object *o = jobj_new();

        json_object_object_add(o, "traffic", json_object_new_array());
        json_object_object_add(body, "clientTraffic", o);
    }
    {
        json_object *o = jobj_new();

        json_object_object_add(o, "lldps", json_object_new_array());
        json_object_object_add(body, "lldp", o);
    }
    {
        json_object *o = jobj_new();

        jadd_int(o, "link", 1);
        json_object_object_add(body, "monitor", o);
    }

    /* config-push results */
    if (st->last_cfg_result)
        json_object_object_add(body, "lastCfgResult",
                               json_object_get(st->last_cfg_result));
    {
        json_object *o = jobj_new();

        json_object_object_add(o, "setResults",
            st->cfg_results ? json_object_get(st->cfg_results)
                            : json_object_new_array());
        json_object_object_add(body, "cfgResults", o);
    }

    /* model-gated */
    if (p->support_sdwan)
        json_object_object_add(body, "sdwan", sdwan_section());
    if (p->support_discrete_wan || p->support_wan_load_balance)
        json_object_object_add(body, "virtualWanInfo", virtual_wan_section(cfg));
    if (p->support_lte)
        json_object_object_add(body, "lte", lte_section());
    if (p->support_poe)
        json_object_object_add(body, "poe", poe_section(cfg));
    if (p->wireless)
        add_wireless_sections(body);

    return body;
}
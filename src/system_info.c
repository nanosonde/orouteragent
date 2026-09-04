/* orouteragent - real router state collection (proc/sys/uci) */
#include "system_info.h"
#include "netlink.h"
#include "protocol/message.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- device IP ---- */

bool ora_sys_device_ip(const char *towards, char *out, size_t outsz)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    const char *dst = towards && *towards ? towards : "8.8.8.8";

    out[0] = '\0';
    if (fd < 0)
        return false;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    {
        uint32_t ip;

        if (!ora_ip4_parse(dst, &ip)) {
            close(fd);
            return false;
        }
        sa.sin_addr.s_addr = htonl(ip);
    }
    /* connect() on UDP picks the outbound interface address */
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        struct sockaddr_in local;
        socklen_t ll = sizeof(local);

        if (getsockname(fd, (struct sockaddr *)&local, &ll) == 0)
            inet_ntop(AF_INET, &local.sin_addr, out, (socklen_t)outsz);
    }
    close(fd);
    return out[0] != '\0';
}

/* ---- CPU / MEM ---- */

int ora_sys_cpu_percent(void)
{
    /* parse /proc/stat: cpu  user nice system idle iowait irq softirq */
    FILE *f = fopen("/proc/stat", "re");
    long long u = 0, n = 0, s = 0, i = 0, w = 0, irq = 0, sirq = 0, st = 0;

    if (!f)
        return 1;
    if (fscanf(f, "cpu %lld %lld %lld %lld %lld %lld %lld %lld",
               &u, &n, &s, &i, &w, &irq, &sirq, &st) < 4) {
        fclose(f);
        return 1;
    }
    fclose(f);
    {
        long long total = u + n + s + i + w + irq + sirq + st;
        long long idle = i + w;
        long long used = total - idle;
        if (total <= 0)
            return 1;
        return (int)(used * 100 / total);
    }
}

int ora_sys_mem_percent(void)
{
    FILE *f = fopen("/proc/meminfo", "re");
    long long total = 0, avail = 0, freeb = 0;
    char line[128];

    if (!f)
        return 32;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %lld", &total) == 1)
            continue;
        if (sscanf(line, "MemAvailable: %lld", &avail) == 1)
            continue;
        if (sscanf(line, "MemFree: %lld", &freeb) == 1)
            continue;
    }
    fclose(f);
    if (total <= 0)
        return 32;
    if (avail <= 0)
        avail = freeb;
    return (int)((total - avail) * 100 / total);
}

/* ---- link state ---- */

static int read_sys_int(const char *ifname, const char *leaf, long *out)
{
    char path[128];
    FILE *f;
    long v = -1;

    snprintf(path, sizeof(path), "/sys/class/net/%s/%s", ifname, leaf);
    f = fopen(path, "re");
    if (!f)
        return -1;
    if (fscanf(f, "%ld", &v) != 1)
        v = -1;
    fclose(f);
    *out = v;
    return 0;
}

static int speed_duplex_to_mbps(const char *ifname)
{
    long s;

    if (read_sys_int(ifname, "speed", &s) != 0 || s <= 0 || s > 100000)
        return 0;
    return (int)s;
}

bool ora_sys_link_state(const char *ifname, struct ora_link_state *ls)
{
    char path[128];
    char flags[64] = {0};
    FILE *f;
    long carrier = 0, oper = 0;

    memset(ls, 0, sizeof(*ls));
    ls->latency = -1;
    if (!ifname || !*ifname)
        return false;

    snprintf(path, sizeof(path), "/sys/class/net/%s/flags", ifname);
    f = fopen(path, "re");
    if (!f)
        return false;
    if (fscanf(f, "%63s", flags) == 1 && strstr(flags, "up")) {
        /* interface administratively up; nothing else to do here */
    }
    fclose(f);

    /* prefer operstate; carrier as fallback */
    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ifname);
    f = fopen(path, "re");
    if (f) {
        char st[32] = {0};
        if (fscanf(f, "%31s", st) == 1)
            oper = !strcmp(st, "up");
        fclose(f);
    }
    if (read_sys_int(ifname, "carrier", &carrier) != 0)
        carrier = oper; /* virt interfaces have no carrier */
    ls->up = (oper || carrier) && carrier != 0 ? (oper ? true : carrier != 0) : false;

    ls->speed = ls->up ? speed_duplex_to_mbps(ifname) : 0;

    /* IP address via netlink would be heavier; use ioctl SIOCGIFADDR */
    {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct ifreq ifr;

        if (fd >= 0) {
            memset(&ifr, 0, sizeof(ifr));
            snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
            ifr.ifr_addr.sa_family = AF_INET;
            if (ioctl(fd, SIOCGIFADDR, &ifr) == 0)
                inet_ntop(AF_INET,
                          &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr,
                          ls->ip, sizeof(ls->ip));
            if (ioctl(fd, SIOCGIFNETMASK, &ifr) == 0)
                inet_ntop(AF_INET,
                          &((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr,
                          ls->netmask, sizeof(ls->netmask));
            close(fd);
        }
    }
    ora_nl_ipv6_addr(ifname, ls->ip6, sizeof(ls->ip6), &ls->ip6_prefix);
    return true;
}

/* ---- traffic ---- */

/* Parse one /proc/net/dev row into @t. Returns the interface name in
 * @name. Layout: iface: rx_bytes rx_pkts rx_errs rx_drop fifo frame
 * compressed multicast tx_bytes tx_pkts tx_errs tx_drop ... */
static bool parse_netdev_line(const char *line, char *name, size_t namesz,
                              struct ora_traffic *t)
{
    const char *colon = strchr(line, ':');
    unsigned long long v[16];
    const char *p;
    int n;

    if (!colon)
        return false;
    {
        size_t len = (size_t)(colon - line);
        while (len && (*line == ' ' || *line == '\t')) {
            line++;
            len--;
        }
        if (len == 0 || len >= namesz)
            return false;
        memcpy(name, line, len);
        name[len] = '\0';
    }
    p = colon + 1;
    n = sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu"
                  " %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
               &v[8], &v[9], &v[10], &v[11]);
    if (n < 12)
        return false;
    t->rx_bytes = v[0];
    t->rx_pkts = v[1];
    t->rx_errs = v[2];
    t->rx_drop = v[3];
    t->tx_bytes = v[8];
    t->tx_pkts = v[9];
    t->tx_errs = v[10];
    t->tx_drop = v[11];
    return true;
}

static bool netdev_walk(const char *want, struct ora_traffic *acc)
{
    FILE *f = fopen("/proc/net/dev", "re");
    char line[512];
    bool any = false;

    memset(acc, 0, sizeof(*acc));
    if (!f)
        return false;
    if (!fgets(line, sizeof(line), f) || !fgets(line, sizeof(line), f)) {
        fclose(f);
        return false;
    }
    while (fgets(line, sizeof(line), f)) {
        char name[64];
        struct ora_traffic t;

        if (!parse_netdev_line(line, name, sizeof(name), &t))
            continue;
        if (want) {
            if (strcmp(name, want))
                continue;
            *acc = t;
            any = true;
            break;
        }
        if (!strcmp(name, "lo") || !strncmp(name, "ifb", 3))
            continue;
        acc->rx_bytes += t.rx_bytes;
        acc->tx_bytes += t.tx_bytes;
        acc->rx_pkts += t.rx_pkts;
        acc->tx_pkts += t.tx_pkts;
        acc->rx_errs += t.rx_errs;
        acc->tx_errs += t.tx_errs;
        acc->rx_drop += t.rx_drop;
        acc->tx_drop += t.tx_drop;
        any = true;
    }
    fclose(f);
    return any;
}

bool ora_sys_traffic_total(struct ora_traffic *t)
{
    return netdev_walk(NULL, t);
}

bool ora_sys_traffic_iface(const char *ifname, struct ora_traffic *t)
{
    if (!ifname || !*ifname) {
        memset(t, 0, sizeof(*t));
        return false;
    }
    return netdev_walk(ifname, t);
}

uint64_t ora_sys_rate_bps(uint64_t bytes_now, uint64_t bytes_prev, uint64_t dt_ms)
{
    if (dt_ms == 0 || bytes_now < bytes_prev)
        return 0;
    return ((bytes_now - bytes_prev) * 8ull * 1000ull) / dt_ms;
}

/* ---- WAN details ---- */

void ora_sys_dns(char *pri, size_t prisz, char *snd, size_t sndsz)
{
    static const char *paths[] = {
        "/tmp/resolv.conf.d/resolv.conf.auto",
        "/tmp/resolv.conf.auto",
        "/etc/resolv.conf",
    };
    size_t i;
    int found = 0;

    if (pri && prisz)
        pri[0] = '\0';
    if (snd && sndsz)
        snd[0] = '\0';

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]) && found < 2; i++) {
        FILE *f = fopen(paths[i], "re");
        char line[256];

        if (!f)
            continue;
        while (fgets(line, sizeof(line), f) && found < 2) {
            char addr[64];

            if (sscanf(line, " nameserver %63s", addr) != 1)
                continue;
            /* skip the local dnsmasq listener: it is not a useful
             * upstream value for the controller's WAN view */
            if (!strcmp(addr, "127.0.0.1") || !strcmp(addr, "::1"))
                continue;
            if (found == 0 && pri && prisz)
                snprintf(pri, prisz, "%s", addr);
            else if (found == 1 && snd && sndsz)
                snprintf(snd, sndsz, "%s", addr);
            found++;
        }
        fclose(f);
    }
}

void ora_sys_conntrack(int64_t *count, int64_t *max)
{
    FILE *f;
    long v;

    if (count) {
        *count = 0;
        f = fopen("/proc/sys/net/netfilter/nf_conntrack_count", "re");
        if (f) {
            if (fscanf(f, "%ld", &v) == 1)
                *count = v;
            fclose(f);
        }
    }
    if (max) {
        *max = 65536;
        f = fopen("/proc/sys/net/netfilter/nf_conntrack_max", "re");
        if (f) {
            if (fscanf(f, "%ld", &v) == 1 && v > 0)
                *max = v;
            fclose(f);
        }
    }
}

/* ---- clients ---- */

json_object *ora_sys_dhcp_clients(void)
{
    json_object *arr = json_object_new_array();
    FILE *f;
    char line[512];

    if (!arr)
        return NULL;
    f = fopen("/tmp/dhcp.leases", "re");
    if (!f)
        return arr; /* no dnsmasq leases: empty list */
    while (fgets(line, sizeof(line), f)) {
        char expire[32] = "", mac[32] = "", ip[64] = "", host[128] = "", cid[128] = "";
        int n = sscanf(line, "%31s %31s %63s %127s %127s",
                       expire, mac, ip, host, cid);
        if (n >= 4) {
            json_object *o = json_object_new_object();
            json_object_object_add(o, "host", json_object_new_string(host));
            /* normalize mac to hyphenated uppercase for the controller */
            char nmac[24];
            if (ora_mac_normalize(mac, nmac, sizeof(nmac)))
                json_object_object_add(o, "mac", json_object_new_string(nmac));
            else
                json_object_object_add(o, "mac", json_object_new_string(mac));
            json_object_object_add(o, "ip", json_object_new_string(ip));
            json_object_object_add(o, "expire", json_object_new_string(expire));
            json_object_array_add(arr, o);
        }
    }
    fclose(f);
    return arr;
}

json_object *ora_sys_arp(void)
{
    json_object *arr = json_object_new_array();
    FILE *f;
    char line[512];

    if (!arr)
        return NULL;
    f = fopen("/proc/net/arp", "re");
    if (!f)
        return arr;
    if (!fgets(line, sizeof(line), f)) { /* header */
        fclose(f);
        return arr;
    }
    while (fgets(line, sizeof(line), f)) {
        char ip[64] = "", hw[32] = "", mac[32] = "", mask[32] = "", dev[32] = "";
        if (sscanf(line, "%63s %31s %31s %31s %31s", ip, hw, mac, mask, dev) == 5) {
            json_object *o = json_object_new_object();
            char nmac[24];
            json_object_object_add(o, "ip", json_object_new_string(ip));
            if (ora_mac_normalize(mac, nmac, sizeof(nmac)))
                json_object_object_add(o, "mac", json_object_new_string(nmac));
            else
                json_object_object_add(o, "mac", json_object_new_string(mac));
            json_object_object_add(o, "dev", json_object_new_string(dev));
            json_object_array_add(arr, o);
        }
    }
    fclose(f);
    return arr;
}

/* ---- deviceInfo (negotiation + INFORM shared parts) ---- */

json_object *ora_sys_device_info(const struct ora_config *cfg)
{
    json_object *di = json_object_new_object();
    char ipbuf[INET_ADDRSTRLEN];
    char macbuf[24];
    const struct ora_model_profile *p = cfg->profile;
    char mac2[24];
    int i;

    if (!di)
        return NULL;

    ora_sys_device_ip(cfg->controller[0] ? cfg->controller : NULL,
                      ipbuf, sizeof(ipbuf));

    json_object_object_add(di, "ip", json_object_new_string(ipbuf));
    json_object_object_add(di, "model", json_object_new_string(p->model));
    json_object_object_add(di, "modelVer", json_object_new_string(p->model_ver));
    json_object_object_add(di, "fwVer", json_object_new_string(cfg->fw_version));
    json_object_object_add(di, "hwVer", json_object_new_string(cfg->hw_version));
    json_object_object_add(di, "cerVer", json_object_new_string("1.0"));
    json_object_object_add(di, "deviceType", json_object_new_string("router"));
    /* NOTE: mac/lanMac not in discovery deviceInfo; they are in
     * negotiation deviceInfo. */

    (void)macbuf;
    (void)mac2;
    (void)i;
    return di;
}
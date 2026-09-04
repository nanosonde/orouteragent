/* orouteragent - real router state collectors (proc/sys/uci/ubus) */
#ifndef ORA_SYSTEM_INFO_H
#define ORA_SYSTEM_INFO_H

#include <arpa/inet.h>
#include <json-c/json.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "state.h"

/* ---- identity ---- */

/* The IPv4 address the controller should use to reach us. Heuristic:
 * connect a UDP socket towards the controller (or 8.8.8.8 if no
 * controller) and read the local address; falls back to the first
 * address of "lan". Writes dotted quad. */
bool ora_sys_device_ip(const char *towards, char *out, size_t outsz);

/* Uptime helpers (util) */
uint64_t ora_uptime_s(void);

/* CPU usage percent since boot (rough). */
int ora_sys_cpu_percent(void);
/* MEM usage percent. */
int ora_sys_mem_percent(void);

/* ---- port / link state ---- */

struct ora_link_state {
    bool up;
    int speed;                /* Mbps, 0 unknown */
    char ip[INET_ADDRSTRLEN]; /* "" if none */
    char netmask[INET_ADDRSTRLEN];
    char ip6[INET6_ADDRSTRLEN];
    int ip6_prefix;
    int latency;              /* ms measured, -1 unknown */
};

/* Read link state for a real interface (via /sys/class/net). */
bool ora_sys_link_state(const char *ifname, struct ora_link_state *ls);

/* ---- counters ---- */

struct ora_traffic {
    uint64_t rx_bytes, tx_bytes, rx_pkts, tx_pkts;
    uint64_t rx_errs, tx_errs, rx_drop, tx_drop;
};

/* Total traffic across all interfaces (from /proc/net/dev). */
bool ora_sys_traffic_total(struct ora_traffic *t);

/* Counters of one interface (from /proc/net/dev). */
bool ora_sys_traffic_iface(const char *ifname, struct ora_traffic *t);

/* Instantaneous rate in bit/s, derived from two samples of the same
 * interface taken by the caller across @dt_ms. */
uint64_t ora_sys_rate_bps(uint64_t bytes_now, uint64_t bytes_prev, uint64_t dt_ms);

/* ---- WAN details ---- */

/* Resolver addresses from /tmp/resolv.conf.d/resolv.conf.auto (OpenWrt)
 * with /etc/resolv.conf fallback. */
void ora_sys_dns(char *pri, size_t prisz, char *snd, size_t sndsz);

/* Conntrack usage. */
void ora_sys_conntrack(int64_t *count, int64_t *max);

/* ---- clients ---- */

/* DHCP client list from /tmp/dhcp.leases (host, mac, ip, lease time,
 * client hostname). Returns a json array of
 * {host, mac (colon format), ip, expire...}. */
json_object *ora_sys_dhcp_clients(void);

/* ARP neighbours from /proc/net/arp. */
json_object *ora_sys_arp(void);

/* ---- INFORM builders (system_info + inform.c cooperate) ---- */

/* Build negotiation/inform deviceInfo (fresh per call). */
json_object *ora_sys_device_info(const struct ora_config *cfg);

/* Build full INFORM body (all Phase-1 sections). Caller owns. */
json_object *ora_inform_build_body(const struct ora_config *cfg,
                                   struct ora_state *st);

#endif
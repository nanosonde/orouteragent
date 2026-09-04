/* orouteragent - netlink queries (routes, addresses) via libmnl */
#ifndef ORA_NETLINK_H
#define ORA_NETLINK_H

#include <net/if.h>
#include <netinet/in.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ora_route {
    char dest[64];              /* "0.0.0.0/0", "192.168.1.0/24" */
    char next_hop[INET_ADDRSTRLEN];
    char ifname[IF_NAMESIZE];
    int metric;
    bool is_default;
};

/* Dump the main IPv4 routing table. Fills up to @max entries and
 * returns the number written (0 on error). */
size_t ora_nl_routes(struct ora_route *out, size_t max);

/* Default route (lowest-metric 0.0.0.0/0). false when none. */
bool ora_nl_default_route(struct ora_route *out);

/* First global IPv6 address + prefix length of @ifname. */
bool ora_nl_ipv6_addr(const char *ifname, char *out, size_t outsz, int *prefix);

#endif
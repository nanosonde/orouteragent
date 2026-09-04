/* orouteragent - netlink queries via libmnl */
#include "netlink.h"
#include "util.h"

#include <libmnl/libmnl.h>
#include <linux/if_addr.h>
#include <linux/rtnetlink.h>

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* MNL_SOCKET_BUFFER_SIZE evaluates sysconf() at runtime, which would
 * make the request buffers VLAs; netlink dumps here fit comfortably. */
#define ORA_NL_BUF_SIZE 8192

struct route_ctx {
    struct ora_route *out;
    size_t max;
    size_t n;
};

static int route_attr_cb(const struct nlattr *attr, void *data)
{
    const struct nlattr **tb = data;
    int type = mnl_attr_get_type(attr);

    if (mnl_attr_type_valid(attr, RTA_MAX) < 0)
        return MNL_CB_OK;

    switch (type) {
    case RTA_DST:
    case RTA_GATEWAY:
    case RTA_PREFSRC:
        if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
            return MNL_CB_ERROR;
        break;
    case RTA_OIF:
    case RTA_PRIORITY:
    case RTA_TABLE:
        if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
            return MNL_CB_ERROR;
        break;
    default:
        return MNL_CB_OK;
    }
    tb[type] = attr;
    return MNL_CB_OK;
}

static int route_cb(const struct nlmsghdr *nlh, void *data)
{
    struct route_ctx *ctx = data;
    struct rtmsg *rm = mnl_nlmsg_get_payload(nlh);
    const struct nlattr *tb[RTA_MAX + 1] = {0};
    struct ora_route *r;
    struct in_addr a;

    if (rm->rtm_family != AF_INET)
        return MNL_CB_OK;
    if (rm->rtm_type != RTN_UNICAST)
        return MNL_CB_OK;
    if (rm->rtm_table != RT_TABLE_MAIN)
        return MNL_CB_OK;
    if (ctx->n >= ctx->max)
        return MNL_CB_OK;

    mnl_attr_parse(nlh, sizeof(*rm), route_attr_cb, tb);

    r = &ctx->out[ctx->n];
    memset(r, 0, sizeof(*r));

    if (tb[RTA_DST]) {
        char ip[INET_ADDRSTRLEN];

        a.s_addr = mnl_attr_get_u32(tb[RTA_DST]);
        if (!inet_ntop(AF_INET, &a, ip, sizeof(ip)))
            return MNL_CB_OK;
        snprintf(r->dest, sizeof(r->dest), "%s/%u", ip, rm->rtm_dst_len);
    } else {
        snprintf(r->dest, sizeof(r->dest), "0.0.0.0/%u", rm->rtm_dst_len);
        r->is_default = rm->rtm_dst_len == 0;
    }
    if (tb[RTA_GATEWAY]) {
        a.s_addr = mnl_attr_get_u32(tb[RTA_GATEWAY]);
        if (!inet_ntop(AF_INET, &a, r->next_hop, sizeof(r->next_hop)))
            snprintf(r->next_hop, sizeof(r->next_hop), "0.0.0.0");
    } else {
        snprintf(r->next_hop, sizeof(r->next_hop), "0.0.0.0");
    }
    if (tb[RTA_OIF])
        if_indextoname(mnl_attr_get_u32(tb[RTA_OIF]), r->ifname);
    if (tb[RTA_PRIORITY])
        r->metric = (int)mnl_attr_get_u32(tb[RTA_PRIORITY]);

    ctx->n++;
    return MNL_CB_OK;
}

size_t ora_nl_routes(struct ora_route *out, size_t max)
{
    char buf[ORA_NL_BUF_SIZE];
    struct mnl_socket *nl;
    struct nlmsghdr *nlh;
    struct rtmsg *rtm;
    struct route_ctx ctx = { .out = out, .max = max, .n = 0 };
    unsigned int seq, portid;
    int ret;

    if (!out || max == 0)
        return 0;

    nl = mnl_socket_open(NETLINK_ROUTE);
    if (!nl) {
        ora_log(ORA_LOG_WARN, "netlink open: %s", strerror(errno));
        return 0;
    }
    if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
        mnl_socket_close(nl);
        return 0;
    }
    portid = mnl_socket_get_portid(nl);

    nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type = RTM_GETROUTE;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    nlh->nlmsg_seq = seq = (unsigned int)time(NULL);
    rtm = mnl_nlmsg_put_extra_header(nlh, sizeof(*rtm));
    rtm->rtm_family = AF_INET;

    if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
        mnl_socket_close(nl);
        return 0;
    }
    for (;;) {
        ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
        if (ret <= 0)
            break;
        ret = mnl_cb_run(buf, (size_t)ret, seq, portid, route_cb, &ctx);
        if (ret <= MNL_CB_STOP)
            break;
    }
    mnl_socket_close(nl);
    return ctx.n;
}

bool ora_nl_default_route(struct ora_route *out)
{
    struct ora_route routes[64];
    size_t n = ora_nl_routes(routes, sizeof(routes) / sizeof(routes[0]));
    size_t i;
    bool found = false;

    for (i = 0; i < n; i++) {
        if (!routes[i].is_default)
            continue;
        if (!found || routes[i].metric < out->metric) {
            *out = routes[i];
            found = true;
        }
    }
    return found;
}

struct addr_ctx {
    const char *ifname;
    char *out;
    size_t outsz;
    int *prefix;
    bool found;
};

static int addr_attr_cb(const struct nlattr *attr, void *data)
{
    const struct nlattr **tb = data;
    int type = mnl_attr_get_type(attr);

    if (mnl_attr_type_valid(attr, IFA_MAX) < 0)
        return MNL_CB_OK;
    if (type == IFA_ADDRESS &&
        mnl_attr_validate2(attr, MNL_TYPE_BINARY, sizeof(struct in6_addr)) < 0)
        return MNL_CB_ERROR;
    tb[type] = attr;
    return MNL_CB_OK;
}

static int addr_cb(const struct nlmsghdr *nlh, void *data)
{
    struct addr_ctx *ctx = data;
    struct ifaddrmsg *ifa = mnl_nlmsg_get_payload(nlh);
    const struct nlattr *tb[IFA_MAX + 1] = {0};
    char ifname[IF_NAMESIZE] = {0};
    char addr[INET6_ADDRSTRLEN];

    if (ctx->found || ifa->ifa_family != AF_INET6)
        return MNL_CB_OK;
    /* skip link-local / non-global scopes */
    if (ifa->ifa_scope != RT_SCOPE_UNIVERSE)
        return MNL_CB_OK;
    if (!if_indextoname(ifa->ifa_index, ifname) || strcmp(ifname, ctx->ifname))
        return MNL_CB_OK;

    mnl_attr_parse(nlh, sizeof(*ifa), addr_attr_cb, tb);
    if (!tb[IFA_ADDRESS])
        return MNL_CB_OK;
    if (!inet_ntop(AF_INET6, mnl_attr_get_payload(tb[IFA_ADDRESS]),
                   addr, sizeof(addr)))
        return MNL_CB_OK;

    snprintf(ctx->out, ctx->outsz, "%s", addr);
    if (ctx->prefix)
        *ctx->prefix = ifa->ifa_prefixlen;
    ctx->found = true;
    return MNL_CB_OK;
}

bool ora_nl_ipv6_addr(const char *ifname, char *out, size_t outsz, int *prefix)
{
    char buf[ORA_NL_BUF_SIZE];
    struct mnl_socket *nl;
    struct nlmsghdr *nlh;
    struct ifaddrmsg *ifa;
    struct addr_ctx ctx = {
        .ifname = ifname, .out = out, .outsz = outsz,
        .prefix = prefix, .found = false
    };
    unsigned int seq, portid;
    int ret;

    if (!ifname || !out || outsz == 0)
        return false;
    out[0] = '\0';

    nl = mnl_socket_open(NETLINK_ROUTE);
    if (!nl)
        return false;
    if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
        mnl_socket_close(nl);
        return false;
    }
    portid = mnl_socket_get_portid(nl);

    nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type = RTM_GETADDR;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    nlh->nlmsg_seq = seq = (unsigned int)time(NULL);
    ifa = mnl_nlmsg_put_extra_header(nlh, sizeof(*ifa));
    ifa->ifa_family = AF_INET6;

    if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
        mnl_socket_close(nl);
        return false;
    }
    for (;;) {
        ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
        if (ret <= 0)
            break;
        ret = mnl_cb_run(buf, (size_t)ret, seq, portid, addr_cb, &ctx);
        if (ret <= MNL_CB_STOP)
            break;
    }
    mnl_socket_close(nl);
    return ctx.found;
}
/* orouteragent - UDP discovery implementation
 *
 * Announces (type 1 DISCOVERY) go to the controller (unicast) or to the
 * broadcast address every 10 seconds while the device is unadopted. A
 * controller that wants to speed adoption up answers with
 * PRE_ADOPT_REQUEST naming the management port; the device then stops
 * announcing and connects there. The device sends nothing else here.
 */
#include "discovery.h"
#include "../protocol/constants.h"
#include "../protocol/framing.h"
#include "../protocol/message.h"
#include "../system_info.h"
#include "../util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

json_object *ora_discovery_build_announce(const struct ora_config *cfg,
                                          struct ora_state *st)
{
    char tbuf[64];
    char ipbuf[INET_ADDRSTRLEN];
    json_object *b, *di, *misc, *ctrl;

    b = json_object_new_object();
    if (!b)
        return NULL;

    ora_format_uptime(ora_uptime_s(), tbuf, sizeof(tbuf));
    ora_sys_device_ip(cfg->controller[0] ? cfg->controller : NULL,
                      ipbuf, sizeof(ipbuf));

    di = json_object_new_object();
    json_object_object_add(di, "ip", json_object_new_string(ipbuf));
    json_object_object_add(di, "model", json_object_new_string(cfg->profile->model));
    json_object_object_add(di, "modelVer", json_object_new_string(cfg->profile->model_ver));
    json_object_object_add(di, "fwVer", json_object_new_string(cfg->fw_version));
    json_object_object_add(di, "cerVer", json_object_new_string("1.0"));
    json_object_object_add(di, "hwVer", json_object_new_string(cfg->hw_version));
    json_object_object_add(di, "time", json_object_new_string(tbuf));
    json_object_object_add(di, "wireless", json_object_new_int(cfg->profile->wireless));
    json_object_object_add(b, "deviceInfo", di);

    misc = json_object_new_object();
    json_object_object_add(misc, "portNum", json_object_new_int(cfg->profile->port_num));
    /* customizeRegion is required discovery metadata; 0 means unset. */
    json_object_object_add(misc, "customizeRegion", json_object_new_int(0));
    json_object_object_add(b, "deviceMisc", misc);

    /* gateways report the controller id under controller.id (the AP-style
     * controllerSetting.controllerId is a different device class) */
    ctrl = json_object_new_object();
    json_object_object_add(ctrl, "id", json_object_new_string(
        st->omadac_id[0] ? st->omadac_id : ORA_FACTORY_SENTINEL_ID));
    json_object_object_add(b, "controller", ctrl);

    return b;
}

bool ora_discovery_parse_pre_adopt(const uint8_t *payload, size_t len,
                                   int *adopt_port)
{
    struct ora_message msg;
    bool ok = false;

    if (!ora_msg_decode(&msg, payload, len))
        return false;
    if (msg.hdr.type == ORA_MSG_PRE_ADOPT_REQUEST) {
        if (adopt_port)
            *adopt_port = (int)ora_json_get_int(msg.body, "adoptPort",
                                                ORA_MANAGER_V2_TCP_PORT);
        ok = true;
    }
    ora_msg_free(&msg);
    return ok;
}

static bool resolve_controller(const struct ora_config *cfg, struct in_addr *a)
{
    uint32_t ip;

    if (!cfg->controller[0])
        return false;
    if (ora_ip4_parse(cfg->controller, &ip)) {
        a->s_addr = htonl(ip);
        return true;
    }
    {
        struct hostent *he = gethostbyname(cfg->controller);

        if (he && he->h_addrtype == AF_INET) {
            memcpy(a, he->h_addr_list[0], sizeof(*a));
            return true;
        }
    }
    return false;
}

static void send_announce(int fd, const struct ora_config *cfg,
                          struct ora_state *st,
                          const struct sockaddr_in *bcast)
{
    struct ora_buf out;
    struct ora_header hdr;
    json_object *body;
    struct sockaddr_in dst;

    body = ora_discovery_build_announce(cfg, st);
    if (!body)
        return;

    ora_buf_init(&out);
    ora_header_init(&hdr, cfg->mac, ORA_MSG_DISCOVERY, NULL);
    if (!ora_msg_encode(&out, &hdr, body))
        goto out;

    /* ECSP frames every message with a 4-byte big-endian length prefix,
     * including the plaintext UDP announce; without it the controller
     * reads the first four JSON bytes as the length and drops the
     * datagram. */
    {
        struct ora_buf framed;

        ora_buf_init(&framed);
        if (ora_frame_encode(&framed, out.data, out.len)) {
            ora_buf_free(&out);
            out = framed;
        } else {
            ora_buf_free(&framed);
        }
    }

    dst = *bcast;
    if (cfg->discovery_mode == ORA_DISC_UNICAST ||
        (cfg->discovery_mode == ORA_DISC_AUTO && cfg->controller[0])) {
        struct in_addr a;

        if (!resolve_controller(cfg, &a)) {
            ora_log(ORA_LOG_WARN, "discovery: cannot resolve controller '%s'",
                    cfg->controller);
            goto out;
        }
        dst.sin_addr = a;
    }

    if (sendto(fd, out.data, out.len, 0, (const struct sockaddr *)&dst,
               sizeof(dst)) < 0) {
        ora_log(ORA_LOG_WARN, "discovery: sendto: %s", strerror(errno));
    } else if (ora_log_level() >= ORA_LOG_DEBUG) {
        char ip[INET_ADDRSTRLEN] = "?";

        inet_ntop(AF_INET, &dst.sin_addr, ip, sizeof(ip));
        ora_log(ORA_LOG_DEBUG, "discovery: announced to %s", ip);
    }
out:
    json_object_put(body);
    ora_buf_free(&out);
}

bool ora_discovery_run(const struct ora_config *cfg, struct ora_state *st,
                       volatile bool *stop, struct ora_discovery_result *out)
{
    int fd;
    struct sockaddr_in bcast;
    int one = 1;
    uint64_t last_announce = 0;
    bool got_reply = false;

    memset(out, 0, sizeof(*out));
    out->adopt_port = cfg->adopt_port;
    snprintf(out->controller, sizeof(out->controller), "%s", cfg->controller);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        ora_log(ORA_LOG_ERR, "discovery: socket: %s", strerror(errno));
        return false;
    }
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    /* Adopted devices do not wait for a pre-adopt reply (one only comes
     * when an operator adopts an unmanaged device). If a controller is
     * known, resume the management session directly; without one, keep
     * announcing so a controller that still holds the adoption can
     * answer with pre-adopt. */
    if (st->adopted && cfg->controller[0]) {
        close(fd);
        return false;
    }

    memset(&bcast, 0, sizeof(bcast));
    bcast.sin_family = AF_INET;
    bcast.sin_port = htons(ORA_DISCOVERY_UDP_PORT);
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    while (!*stop && !got_reply) {
        uint64_t now = ora_now_ms();
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        /* discovery datagrams are small; a frame-sized stack buffer here
         * would be 256 KB */
        uint8_t pkt[4096];
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        const uint8_t *payload;
        size_t payload_len;
        ssize_t n;
        int adopt_port = cfg->adopt_port;

        if (now - last_announce >= (uint64_t)ORA_ANNOUNCE_INTERVAL_S * 1000) {
            send_announce(fd, cfg, st, &bcast);
            last_announce = now;
        }

        if (poll(&pfd, 1, 1000) <= 0)
            continue;

        n = recvfrom(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&peer, &plen);
        if (n <= 0)
            continue;
        if (ora_frame_decode_datagram(pkt, (size_t)n, &payload, &payload_len) < 0) {
            ora_log(ORA_LOG_DEBUG, "discovery: malformed datagram dropped");
            continue;
        }
        if (!ora_discovery_parse_pre_adopt(payload, payload_len, &adopt_port))
            continue;

        /* Learn where the controller lives: the peer address is
         * authoritative even when no controller was configured. */
        if (!inet_ntop(AF_INET, &peer.sin_addr, out->controller,
                       (socklen_t)sizeof(out->controller)))
            continue;
        out->adopt_port = adopt_port > 0 ? adopt_port : cfg->adopt_port;
        got_reply = true;
        ora_log(ORA_LOG_INFO, "discovery: pre-adopt from %s (adoptPort=%d)",
                out->controller, out->adopt_port);
    }

    close(fd);
    return got_reply;
}
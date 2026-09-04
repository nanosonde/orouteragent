/* orouteragent - management channel implementation */
#include "manage.h"
#include "capture.h"
#include "dmp.h"
#include "rtty.h"
#include "tls.h"
#include "transfer.h"
#include "../inform.h"
#include "../protocol/auth.h"
#include "../protocol/base64.h"
#include "../protocol/constants.h"
#include "../protocol/framing.h"
#include "../protocol/message.h"
#include "../system_info.h"
#include "../util.h"

#include <ctype.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The live management channel, shared with the capture/transfer threads
 * that must emit NOTIFY and FILE_TRANSFER frames on it.
 *
 * lock guards the members below AND serialises every write to the TLS
 * context: mbedTLS has no internal locking, so two threads writing the
 * same context would interleave TLS records. */
static struct {
    pthread_mutex_t lock;
    struct ora_tls *tls;
    char mac[24];
    char dest[40];
    int64_t notify_id;
} g_mgmt = { .lock = PTHREAD_MUTEX_INITIALIZER };

#define ORA_LOG_JSON_CHUNK 700

static bool key_contains_ci(const char *key, const char *needle)
{
    size_t needle_len;

    if (!key || !needle)
        return false;
    needle_len = strlen(needle);
    if (!needle_len)
        return true;
    for (; *key; key++) {
        size_t i;

        for (i = 0; i < needle_len; i++) {
            if (!key[i] || tolower((unsigned char)key[i]) !=
                           tolower((unsigned char)needle[i]))
                break;
        }
        if (i == needle_len)
            return true;
    }
    return false;
}

static bool log_key_is_sensitive(const char *key)
{
    static const char *const fragments[] = {
        "password", "username", "credential", "token", "auth",
        "nonce", "secret", "randomkey", "aeskey", "privatekey",
        "presharedkey", "passphrase", "community", "psk"
    };
    size_t i;

    if (!key)
        return false;
    if (!strcasecmp(key, "iv") || !strcasecmp(key, "key"))
        return true;
    for (i = 0; i < sizeof(fragments) / sizeof(fragments[0]); i++) {
        if (key_contains_ci(key, fragments[i]))
            return true;
    }
    return false;
}

static json_object *redact_json_for_log(const json_object *value)
{
    json_object *copy;
    size_t i;

    if (!value)
        return json_object_new_null();

    switch (json_object_get_type(value)) {
    case json_type_object:
        copy = json_object_new_object();
        if (!copy)
            return NULL;
        json_object_object_foreach((json_object *)value, key, child) {
            json_object *child_copy;

            if (log_key_is_sensitive(key))
                child_copy = json_object_new_string("[redacted]");
            else
                child_copy = redact_json_for_log(child);
            if (!child_copy) {
                json_object_put(copy);
                return NULL;
            }
            json_object_object_add(copy, key, child_copy);
        }
        return copy;
    case json_type_array:
        copy = json_object_new_array();
        if (!copy)
            return NULL;
        for (i = 0; i < json_object_array_length(value); i++) {
            json_object *child_copy = redact_json_for_log(
                json_object_array_get_idx(value, i));

            if (!child_copy) {
                json_object_put(copy);
                return NULL;
            }
            json_object_array_add(copy, child_copy);
        }
        return copy;
    case json_type_boolean:
        return json_object_new_boolean(json_object_get_boolean(value));
    case json_type_double:
        return json_object_new_double(json_object_get_double(value));
    case json_type_int:
        return json_object_new_int64(json_object_get_int64(value));
    case json_type_string:
        return json_object_new_string(
            json_object_get_string((json_object *)value));
    case json_type_null:
    default:
        return json_object_new_null();
    }
}

static void log_management_body(const char *direction, const char *operation,
                                int64_t seq, const json_object *body)
{
    json_object *safe;
    const char *text;
    size_t len, chunks, offset;

    if (ora_log_level() < ORA_LOG_DEBUG)
        return;
    safe = redact_json_for_log(body);
    if (!safe) {
        ora_log(ORA_LOG_DEBUG, "%s %s seq %" PRId64
                " body unavailable (redaction allocation failed)",
                direction, operation, seq);
        return;
    }
    text = json_object_to_json_string_ext(safe, JSON_C_TO_STRING_PLAIN);
    if (!text) {
        ora_log(ORA_LOG_DEBUG, "%s %s seq %" PRId64
                " body unavailable (serialization failed)",
                direction, operation, seq);
        json_object_put(safe);
        return;
    }

    len = strlen(text);
    chunks = (len + ORA_LOG_JSON_CHUNK - 1) / ORA_LOG_JSON_CHUNK;
    if (!chunks)
        chunks = 1;
    for (offset = 0; offset < len || (!len && !offset);
         offset += ORA_LOG_JSON_CHUNK) {
        size_t remaining = len - offset;
        size_t chunk_len = remaining < ORA_LOG_JSON_CHUNK ? remaining
                                                           : ORA_LOG_JSON_CHUNK;

        ora_log(ORA_LOG_DEBUG,
                "%s %s seq %" PRId64 " body chunk %zu/%zu: %.*s",
                direction, operation, seq,
                offset / ORA_LOG_JSON_CHUNK + 1, chunks,
                (int)chunk_len, text + offset);
        if (!len)
            break;
    }
    json_object_put(safe);
}

static const char *management_operation(int64_t type)
{
    switch (type) {
    case ORA_MSG_SET_REQUEST:
    case ORA_MSG_SET_RESPONSE:
        return "SET";
    case ORA_MSG_GET_REQUEST:
    case ORA_MSG_GET_RESPONSE:
        return "GET";
    default:
        return NULL;
    }
}

/* Send one framed message (header + optional body) over the TLS stream.
 * Callers must hold g_mgmt.lock once the channel is published, or be the
 * only thread using it (handshake). */
static bool send_msg(struct ora_tls *t, const struct ora_header *hdr,
                     const json_object *body)
{
    struct ora_buf json, frame;
    bool ok;

    ora_buf_init(&json);
    ora_buf_init(&frame);
    ok = ora_msg_encode(&json, hdr, body) &&
         ora_frame_encode(&frame, json.data, json.len) &&
         ora_tls_write(t, frame.data, frame.len) == (int)frame.len;
    if (ok)
        ora_log(ORA_LOG_DEBUG, "-> type %" PRId64 " seq %" PRId64 " (%zu bytes)",
                hdr->type, hdr->seq, frame.len);
    if (ok && management_operation(hdr->type))
        log_management_body("->", management_operation(hdr->type),
                            hdr->seq, body);
    ora_buf_free(&json);
    ora_buf_free(&frame);
    return ok;
}

/* send_msg for use once the channel is published: takes the channel
 * lock so worker threads cannot interleave a write. */
static bool send_msg_sync(struct ora_tls *t, const struct ora_header *hdr,
                          const json_object *body)
{
    bool ok;

    pthread_mutex_lock(&g_mgmt.lock);
    ok = send_msg(t, hdr, body);
    pthread_mutex_unlock(&g_mgmt.lock);
    return ok;
}

/* Receive one framed message (blocking up to timeout). */
static bool recv_msg(struct ora_tls *t, struct ora_message *msg, int timeout_ms)
{
    struct ora_frame_reader rd;
    const uint8_t *payload;
    size_t len;
    int rv;

    ora_frame_reader_init(&rd, ORA_MAX_FRAME_PAYLOAD);
    for (;;) {
        uint8_t chunk[8192];
        int n = ora_tls_read(t, chunk, sizeof(chunk), timeout_ms);
        if (n < 0) {
            ora_frame_reader_free(&rd);
            return false;
        }
        if (n == 0) {
            if (!ora_tls_wait_readable(t, 100)) {
                /* total timeout */
                ora_frame_reader_free(&rd);
                return false;
            }
            continue;
        }
        if (!ora_frame_reader_feed(&rd, chunk, (size_t)n)) {
            ora_frame_reader_free(&rd);
            return false;
        }
        rv = ora_frame_reader_next(&rd, &payload, &len);
        if (rv == 1) {
            bool ok = ora_msg_decode(msg, payload, len);
            ora_frame_reader_consume(&rd);
            ora_frame_reader_free(&rd);
            if (ok)
                ora_log(ORA_LOG_DEBUG, "<- type %" PRId64 " seq %" PRId64,
                        msg->hdr.type, msg->hdr.seq);
            if (ok && management_operation(msg->hdr.type))
                log_management_body("<-", management_operation(msg->hdr.type),
                                    msg->hdr.seq, msg->body);
            return ok;
        }
        if (rv < 0) {
            ora_frame_reader_free(&rd);
            return false;
        }
        /* need more data */
        if (!ora_tls_wait_readable(t, timeout_ms)) {
            ora_frame_reader_free(&rd);
            return false;
        }
    }
}

/* ---- body builders (tests use these too) ---- */

json_object *ora_manage_pre_connect_body(const struct ora_config *cfg,
                                         struct ora_state *st, bool rebuild)
{
    json_object *b = json_object_new_object();
    json_object *di, *misc, *cs;
    char ipbuf[INET_ADDRSTRLEN];

    /* discovery-style deviceInfo */
    ora_sys_device_ip(cfg->controller[0] ? cfg->controller : NULL, ipbuf, sizeof(ipbuf));
    di = json_object_new_object();
    json_object_object_add(di, "ip", json_object_new_string(ipbuf));
    json_object_object_add(di, "model", json_object_new_string(cfg->profile->model));
    json_object_object_add(di, "modelVer", json_object_new_string(cfg->profile->model_ver));
    json_object_object_add(di, "fwVer", json_object_new_string(cfg->fw_version));
    json_object_object_add(di, "cerVer", json_object_new_string("1.0"));
    json_object_object_add(di, "hwVer", json_object_new_string(cfg->hw_version));
    {
        char up[64];
        ora_format_uptime(ora_uptime_s(), up, sizeof(up));
        json_object_object_add(di, "time", json_object_new_string(up));
    }
    json_object_object_add(di, "wireless", json_object_new_int(cfg->profile->wireless));
    json_object_object_add(b, "deviceInfo", di);

    misc = json_object_new_object();
    json_object_object_add(misc, "portNum", json_object_new_int(cfg->profile->port_num));
    /* customizeRegion is required negotiation metadata; 0 means unset. */
    json_object_object_add(misc, "customizeRegion", json_object_new_int(0));
    json_object_object_add(b, "deviceMisc", misc);

    json_object_object_add(b, "needUsername", json_object_new_boolean(true));
    json_object_object_add(b, "rebuild", json_object_new_int(rebuild ? 1 : 0));

    cs = json_object_new_object();
    json_object_object_add(cs, "controllerId", json_object_new_string(
        st->omadac_id[0] ? st->omadac_id : ORA_FACTORY_SENTINEL_ID));
    json_object_object_add(b, "controllerSetting", cs);

    return b;
}

json_object *ora_manage_negotiation_body(const struct ora_config *cfg,
                                         struct ora_state *st)
{
    json_object *b = json_object_new_object();
    json_object *cs, *comp_v2, *devcap, *misc, *spec;
    const struct ora_model_profile *p = cfg->profile;
    size_t i;

    json_object_object_add(b, "key", json_object_new_string(""));
    {
        char cv[24];
        snprintf(cv, sizeof(cv), "%" PRId64, st->config_version);
        json_object_object_add(b, "configVersion", json_object_new_string(cv));
    }
    json_object_object_add(b, "deviceInfo",
                           ora_inform_negotiation_device_info(cfg));

    cs = json_object_new_object();
    json_object_object_add(cs, "controllerId", json_object_new_string(
        st->omadac_id[0] ? st->omadac_id : ORA_FACTORY_SENTINEL_ID));
    json_object_object_add(b, "controllerSetting", cs);

    json_object_object_add(b, "components", json_object_new_string(""));
    /* components_v2: MUST be non-empty (controller warns otherwise) */
    comp_v2 = json_object_new_object();
    for (i = 0; i < p->n_components; i++)
        json_object_object_add(comp_v2, p->components[i].name,
                               json_object_new_string(p->components[i].version));
    json_object_object_add(b, "components_v2", comp_v2);

    /* channelInfo / radioCap: empty lists on gateways */
    json_object_object_add(b, "channelInfo", json_object_new_array());
    json_object_object_add(b, "radioCap", json_object_new_array());

    /* devCap */
    devcap = json_object_new_object();
    json_object_object_add(devcap, "defaultIgmpWan", json_object_new_int(p->default_igmp_wan));
    json_object_object_add(devcap, "extraPortInfos", json_object_new_array());
    json_object_object_add(devcap, "ipsecNum", json_object_new_int(p->ipsec_num));
    json_object_object_add(devcap, "mandatoryPorts", json_object_new_array());
    json_object_object_add(devcap, "maxSslVpnUserConcurrentNum",
                           json_object_new_int(p->max_ssl_vpn_user_concurrent_num));
    json_object_object_add(devcap, "maxVpnUserConcurrentNum",
                           json_object_new_int(p->max_vpn_user_concurrent_num));
    {
        json_object *ports = json_object_new_array();
        for (i = 0; i < p->n_ports; i++) {
            const struct ora_port_info *cap = &p->ports[i];
            json_object *e = json_object_new_object();
            json_object_object_add(e, "port", json_object_new_int(cap->port));
            json_object_object_add(e, "name", json_object_new_string(cap->name));
            json_object_object_add(e, "type", json_object_new_int(cap->type));
            json_object_object_add(e, "mode", json_object_new_int(cap->mode));
            if (cap->has_max_bandwidth)
                json_object_object_add(e, "maxBandwidth",
                                       json_object_new_int(cap->max_bandwidth));
            json_object_object_add(e, "defaultSpeedDuplex",
                                   json_object_new_string(cap->default_speed_duplex));
            {
                json_object *sdl = json_object_new_array();
                int j;
                for (j = 0; j < cap->n_speed_duplex; j++)
                    json_object_array_add(sdl,
                        json_object_new_string(cap->speed_duplex_list[j]));
                json_object_object_add(e, "speedDuplexList", sdl);
            }
            json_object_object_add(e, "supportInternetVlan",
                                   json_object_new_int(cap->support_internet_vlan));
            json_object_object_add(e, "supportIptv", json_object_new_int(cap->support_iptv));
            json_object_object_add(e, "supportMirror", json_object_new_int(cap->support_mirror));
            json_object_object_add(e, "supportPoe", json_object_new_int(cap->support_poe));
            json_object_array_add(ports, e);
        }
        json_object_object_add(devcap, "portInfos", ports);
    }
    spec = json_object_new_object();
    {
        const struct ora_spec *sp = p->spec;
        json_object_object_add(spec, "aclNum", json_object_new_int(sp->acl_num));
        json_object_object_add(spec, "bandwidthCtrlNum", json_object_new_int(sp->bandwidth_ctrl_num));
        json_object_object_add(spec, "clientIpBindingNum", json_object_new_int(sp->client_ip_binding_num));
        json_object_object_add(spec, "ddnsNum", json_object_new_int(sp->ddns_num));
        json_object_object_add(spec, "ipGroupNum", json_object_new_int(sp->ip_group_num));
        json_object_object_add(spec, "ipv6GroupNum", json_object_new_int(sp->ipv6_group_num));
        json_object_object_add(spec, "ldapClassRulesNum", json_object_new_int(sp->ldap_class_rules_num));
        json_object_object_add(spec, "natPfNum", json_object_new_int(sp->nat_pf_num));
        json_object_object_add(spec, "networkNum", json_object_new_int(sp->network_num));
        json_object_object_add(spec, "policyRoutingNum", json_object_new_int(sp->policy_routing_num));
        json_object_object_add(spec, "qosClassRulesNum", json_object_new_int(sp->qos_class_rules_num));
        json_object_object_add(spec, "serviceTypeNum", json_object_new_int(sp->service_type_num));
        json_object_object_add(spec, "sessionLimitNum", json_object_new_int(sp->session_limit_num));
        json_object_object_add(spec, "sslVpnConnectionsNum", json_object_new_int(sp->ssl_vpn_connections_num));
        json_object_object_add(spec, "sslVpnLocksNum", json_object_new_int(sp->ssl_vpn_locks_num));
        json_object_object_add(spec, "sslVpnResourceGroupsNum", json_object_new_int(sp->ssl_vpn_resource_groups_num));
        json_object_object_add(spec, "sslVpnResourcesNum", json_object_new_int(sp->ssl_vpn_resources_num));
        json_object_object_add(spec, "sslVpnUserGroupsNum", json_object_new_int(sp->ssl_vpn_user_groups_num));
        json_object_object_add(spec, "sslVpnUsersNum", json_object_new_int(sp->ssl_vpn_users_num));
        json_object_object_add(spec, "staticRoutingNum", json_object_new_int(sp->static_routing_num));
        json_object_object_add(spec, "urlFilteringNum", json_object_new_int(sp->url_filtering_num));
        json_object_object_add(spec, "vpnIPSecNum", json_object_new_int(sp->vpn_ipsec_num));
        json_object_object_add(spec, "vpnL2TPClientNum", json_object_new_int(sp->vpn_l2tp_client_num));
        json_object_object_add(spec, "vpnOpenVPNNum", json_object_new_int(sp->vpn_openvpn_num));
        json_object_object_add(spec, "vpnPPTPClientNum", json_object_new_int(sp->vpn_pptp_client_num));
        json_object_object_add(spec, "vpnUsersNum", json_object_new_int(sp->vpn_users_num));
        json_object_object_add(spec, "wireguardAllPeerNum", json_object_new_int(sp->wireguard_all_peer_num));
        json_object_object_add(spec, "wireguardNum", json_object_new_int(sp->wireguard_num));
        json_object_object_add(spec, "wireguardPeerNum", json_object_new_int(sp->wireguard_peer_num));
    }
    json_object_object_add(devcap, "specification", spec);
    json_object_object_add(devcap, "supportAclDisable", json_object_new_int(p->support_acl_disable));
    json_object_object_add(devcap, "supportAllWan", json_object_new_boolean(p->support_all_wan));
    if (p->has_discrete_wan)
        json_object_object_add(devcap, "supportDiscreteWan",
                               json_object_new_int(p->support_discrete_wan));
    if (p->has_wlb)
        json_object_object_add(devcap, "supportWanLoadBalance",
                               json_object_new_int(p->support_wan_load_balance ? 1 : 0));
    if (p->has_lte)
        json_object_object_add(devcap, "supportLte", json_object_new_int(p->support_lte ? 1 : 0));
    if (p->has_sdwan)
        json_object_object_add(devcap, "supportSdWan", json_object_new_int(p->support_sdwan ? 1 : 0));
    json_object_object_add(devcap, "supportIPsecFailover",
                           json_object_new_int(p->support_ipsec_failover));
    json_object_object_add(devcap, "supportRoutingVpnClient",
                           json_object_new_int(p->support_routing_vpn_client));
    json_object_object_add(devcap, "supportVpnUsb", json_object_new_int(p->support_vpn_usb));
    json_object_object_add(devcap, "supportVpnVerify", json_object_new_int(p->support_vpn_verify));
    /* WiredDevice adds the terminal capability flags so the controller's
     * Tools -> Terminal device picker includes the gateway. */
    json_object_object_add(devcap, "supportTerminal", json_object_new_boolean(true));
    json_object_object_add(devcap, "terminalSupport", json_object_new_boolean(true));
    json_object_object_add(b, "devCap", devcap);

    /* deviceMisc */
    misc = json_object_new_object();
    {
        json_object *epn = json_object_new_object();
        json_object_object_add(epn, "extraPort", json_object_new_int(p->extra_port));
        json_object_object_add(epn, "usbLteWan", json_object_new_int(p->usb_lte_wan));
        json_object_object_add(misc, "extraPortNum", epn);
    }
    json_object_object_add(misc, "portNum", json_object_new_int(p->port_num));
    json_object_object_add(misc, "customizeRegion", json_object_new_int(0));
    json_object_object_add(b, "deviceMisc", misc);

    /* monitorCapabilities is mandatory during capability exchange. */
    {
        json_object *mc = json_object_new_object();
        json_object *proto = json_object_new_array();
        json_object *out_types = json_object_new_array();
        json_object *in_types = json_object_new_array();
        json_object *compress = json_object_new_array();

        json_object_array_add(proto, json_object_new_string("TLS"));
        json_object_array_add(out_types, json_object_new_string("protobuf2"));
        json_object_array_add(in_types, json_object_new_string("protobuf2"));
        json_object_array_add(compress, json_object_new_string("lzo-2.07"));
        json_object_object_add(mc, "protocols", proto);
        json_object_object_add(mc, "outTypes", out_types);
        json_object_object_add(mc, "inTypes", in_types);
        json_object_object_add(mc, "compressMethods", compress);
        json_object_object_add(b, "monitorCapabilities", mc);
    }

    return b;
}

bool ora_manage_device_verify_info(const char *username, const char *password,
                                   const char *random_key,
                                   char *auth_out, size_t outsz)
{
    return ora_auth_compute(auth_out, outsz, username, password, random_key);
}

/* ---- handshake ---- */

/* Send PRE_CONNECT_INFO and consume PRE_CONNECT_INFO_RESPONSE.
 * The response carries randomKeyForDeviceVerify (the nonce the device
 * must hash its credentials against) and optionally the username the
 * controller expects. Returns 0 on success. */
static int pre_connect(struct ora_tls *t, const struct ora_config *cfg,
                       struct ora_state *st, bool rebuild,
                       char *nonce, size_t noncesz,
                       char *username, size_t usernamesz)
{
    struct ora_message msg;
    struct ora_header hdr;
    json_object *body;
    const char *s;
    int rc = -1;

    body = ora_manage_pre_connect_body(cfg, st, rebuild);
    ora_header_init(&hdr, cfg->mac, ORA_MSG_PRE_CONNECT_INFO, NULL);
    if (!send_msg(t, &hdr, body)) {
        json_object_put(body);
        return -1;
    }
    json_object_put(body);

    if (!recv_msg(t, &msg, ORA_IO_TIMEOUT_S * 1000))
        return -1;
    if (msg.hdr.type != ORA_MSG_PRE_CONNECT_INFO_RESPONSE)
        goto out;

    s = ora_json_get_str(msg.body, "randomKeyForDeviceVerify", NULL);
    if (!s || !*s) {
        ora_log(ORA_LOG_WARN, "pre-connect response without verify nonce");
        goto out;
    }
    snprintf(nonce, noncesz, "%s", s);

    s = ora_json_get_str(msg.body, "username", NULL);
    if (s && *s)
        snprintf(username, usernamesz, "%s", s);

    s = ora_json_get_str(ora_json_get_obj(msg.body, "controllerSetting"),
                         "controllerId", NULL);
    if (!s)
        s = ora_json_get_str(msg.body, "controllerId", NULL);
    if (s && *s)
        ora_state_set_omadac(st, s);

    rc = 0;
out:
    ora_msg_free(&msg);
    return rc;
}

/* The adoption exchange is event-driven, not lockstep: after
 * DEVICE_VERIFY_INFO the device reacts to whatever the controller sends.
 *
 *   dev -> PRE_CONNECT_INFO           dev <- PRE_CONNECT_INFO_RESPONSE
 *   dev -> DEVICE_VERIFY_INFO         dev <- DEVICE_VERIFY_RESPONSE
 *   dev -> SYSTEM_VERIFY_RESULT       dev <- VERIFY_RESULT_ACK
 *   dev -> DEVICE_NEGOTIATION         dev <- SYSTEM_NEGOTIATION / INIT_SYNC
 *   dev -> INIT_SYNC_RESULT           dev <- INIT_SYNC_RESULT_ACK  => CONNECTED
 *
 * Returns 0 once the device is connected, negative on failure. */
static int handshake(struct ora_tls *t, const struct ora_config *cfg,
                     struct ora_state *st, volatile bool *stop)
{
    struct ora_header hdr;
    json_object *body;
    char nonce[64] = {0};
    char username[64];
    const char *password;
    bool rebuild = st->adopted;
    bool negotiated = false;
    bool connected = false;
    uint64_t deadline;

    /* A device that was adopted before re-adopts with the provisioned
     * site account (Force Provision path). */
    if (rebuild && st->device_username[0])
        snprintf(username, sizeof(username), "%s", st->device_username);
    else
        snprintf(username, sizeof(username), "%s", cfg->device_username);
    password = (rebuild && st->device_password[0]) ? st->device_password
                                                   : cfg->device_password;

    if (pre_connect(t, cfg, st, rebuild, nonce, sizeof(nonce),
                    username, sizeof(username)) != 0)
        return -1;

    {
        char auth[ORA_AUTH_BUF_LEN];
        char device_nonce[ORA_UUID_LEN + 1];

        if (rebuild && st->device_password_is_md5) {
            if (!ora_auth_compute_md5(auth, sizeof(auth), username, password,
                          nonce))
            return -1;
        } else if (!ora_manage_device_verify_info(username, password, nonce,
                              auth, sizeof(auth))) {
            return -1;
        }
        /* the device's own nonce must be a full 36-char UUID: newer
         * controllers reject anything shorter */
        if (!ora_auth_uuid_v4(device_nonce, sizeof(device_nonce)))
            return -1;

        body = json_object_new_object();
        json_object_object_add(body, "auth", json_object_new_string(auth));
        json_object_object_add(body, "randomKeyForSystemVerify",
                               json_object_new_string(device_nonce));
        ora_header_init(&hdr, cfg->mac, ORA_MSG_DEVICE_VERIFY_INFO, NULL);
        if (!send_msg(t, &hdr, body)) {
            json_object_put(body);
            return -1;
        }
        json_object_put(body);
    }

    deadline = ora_now_ms() + 30000;
    while (!connected && !*stop && ora_now_ms() < deadline) {
        struct ora_message msg;

        if (!ora_tls_wait_readable(t, 500))
            continue;
        if (!recv_msg(t, &msg, ORA_IO_TIMEOUT_S * 1000))
            return -1;

        switch (msg.hdr.type) {
        case ORA_MSG_DEVICE_VERIFY_RESPONSE:
            if (msg.hdr.error != 0) {
                ora_log(ORA_LOG_ERR,
                        "adoption rejected: device verify failed (error=%" PRId64
                        ") - check the site Device Account", msg.hdr.error);
                ora_msg_free(&msg);
                return -1;
            }
            ora_header_init(&hdr, cfg->mac, ORA_MSG_SYSTEM_VERIFY_RESULT, NULL);
            if (!send_msg(t, &hdr, NULL))
                goto fail;
            break;

        case ORA_MSG_VERIFY_RESULT_ACK:
            if (!negotiated) {
                negotiated = true;
                body = ora_manage_negotiation_body(cfg, st);
                ora_header_init(&hdr, cfg->mac, ORA_MSG_DEVICE_NEGOTIATION, NULL);
                if (!send_msg(t, &hdr, body)) {
                    json_object_put(body);
                    goto fail;
                }
                json_object_put(body);
            }
            break;

        case ORA_MSG_SYSTEM_NEGOTIATION: {
            /* may carry the site Device Account to use from now on */
            json_object *ua = ora_json_get_obj(msg.body, "userAccount");

            if (ua) {
                const char *nu = ora_json_get_str(ua, "newUsername", NULL);
                const char *np = ora_json_get_str(ua, "newPassword", NULL);
                const char *cp = ora_json_get_str(ua, "compatiblePassword", NULL);
                const char *account_password = cp ? cp : np;
                bool password_is_md5 = cp != NULL ||
                    (np && strlen(np) == ORA_MD5_HEX_LEN &&
                     strspn(np, "0123456789abcdefABCDEF") == ORA_MD5_HEX_LEN);

                if (nu && *nu) {
                    ora_state_set_account(st, nu, account_password,
                                          password_is_md5);
                    ora_log(ORA_LOG_INFO, "captured provisioned device account");
                }
            }
            ora_header_init(&hdr, cfg->mac, ORA_MSG_INIT_SYNC_RESULT, NULL);
            hdr.seq = msg.hdr.seq;
            if (!send_msg(t, &hdr, NULL))
                goto fail;
            connected = true;
            break;
        }

        case ORA_MSG_INIT_SYNC_RESULT_ACK:
            connected = true;
            break;

        default:
            break; /* the controller may interleave other messages */
        }
        ora_msg_free(&msg);
        continue;
fail:
        ora_msg_free(&msg);
        return -1;
    }

    if (!connected)
        return -1;

    st->adopted = true;
    ora_state_save(st);
    ora_log(ORA_LOG_INFO, "adoption complete; device is CONNECTED (controller %s)",
            st->omadac_id[0] ? st->omadac_id : "?");
    return 0;
}

/* ---- cross-thread senders ---- */

static void mgmt_channel_set(struct ora_tls *tls, const char *mac, const char *dest)
{
    pthread_mutex_lock(&g_mgmt.lock);
    g_mgmt.tls = tls;
    snprintf(g_mgmt.mac, sizeof(g_mgmt.mac), "%s", mac ? mac : "");
    snprintf(g_mgmt.dest, sizeof(g_mgmt.dest), "%s", dest ? dest : "");
    pthread_mutex_unlock(&g_mgmt.lock);
}

static void mgmt_channel_clear(void)
{
    pthread_mutex_lock(&g_mgmt.lock);
    g_mgmt.tls = NULL;
    pthread_mutex_unlock(&g_mgmt.lock);
}

bool ora_manage_connected(void)
{
    bool up;

    pthread_mutex_lock(&g_mgmt.lock);
    up = g_mgmt.tls != NULL;
    pthread_mutex_unlock(&g_mgmt.lock);
    return up;
}

bool ora_manage_send_notify(int subject, json_object *content)
{
    struct ora_header hdr;
    json_object *body;
    bool ok = false;

    pthread_mutex_lock(&g_mgmt.lock);
    if (!g_mgmt.tls) {
        pthread_mutex_unlock(&g_mgmt.lock);
        ora_log(ORA_LOG_WARN, "notify: management channel not connected");
        return false;
    }

    body = json_object_new_object();
    json_object_object_add(body, "nid", json_object_new_int64(++g_mgmt.notify_id));
    json_object_object_add(body, "sub", json_object_new_int(subject));
    json_object_object_add(body, "nre", json_object_new_int(1));
    json_object_object_add(body, "ctnt", json_object_get(content));

    /* dest and timestamp are required for notification routing. */
    ora_header_init(&hdr, g_mgmt.mac, ORA_MSG_NOTIFY_REQUEST, g_mgmt.dest);
    ok = send_msg(g_mgmt.tls, &hdr, body);

    json_object_put(body);
    pthread_mutex_unlock(&g_mgmt.lock);
    return ok;
}

bool ora_manage_send_file_transfer(json_object *body, int64_t seq)
{
    struct ora_header hdr;
    bool ok;

    pthread_mutex_lock(&g_mgmt.lock);
    if (!g_mgmt.tls) {
        pthread_mutex_unlock(&g_mgmt.lock);
        return false;
    }
    ora_header_init(&hdr, g_mgmt.mac, ORA_MSG_FILE_TRANSFER_RESPONSE_V2, NULL);
    hdr.seq = seq;
    ok = send_msg(g_mgmt.tls, &hdr, body);
    pthread_mutex_unlock(&g_mgmt.lock);
    return ok;
}

/* ---- auxiliary services (Tools) ---- */
/* terminalSetting -> RTTY terminal channel. */
static void apply_terminal_setting(const struct ora_config *cfg, json_object *ts)
{
    struct ora_rtty_settings set;
    const char *s;

    memset(&set, 0, sizeof(set));
    set.enable = ora_json_get_bool(ts, "enable", false);
    s = ora_json_get_str(ts, "token", "");
    snprintf(set.token, sizeof(set.token), "%s", s);
    set.port = (int)ora_json_get_int(ts, "port", ORA_RTTY_TCP_PORT);
    set.ssl = ora_json_get_bool(ts, "ssl", true);
    /* the terminal server is the controller we are managed by */
    snprintf(set.host, sizeof(set.host), "%s", cfg->controller);

    if (set.enable && set.token[0])
        ora_rtty_start(cfg, &set);
    else
        ora_rtty_stop();
}

/* monitorServer -> DMP network-check channel. */
static void apply_monitor_server(const struct ora_config *cfg, json_object *ms)
{
    struct ora_dmp_settings set;
    const char *s;

    memset(&set, 0, sizeof(set));
    s = ora_json_get_str(ms, "token", "");
    snprintf(set.token, sizeof(set.token), "%s", s);
    set.port = (int)ora_json_get_int(ms, "port", ORA_DEVICE_MONITOR_PORT);
    s = ora_json_get_str(ms, "path", "/");
    snprintf(set.path, sizeof(set.path), "%s", s);
    s = ora_json_get_str(ms, "protocol", "tls");
    set.tls = strcmp(s, "tcp") != 0;
    s = ora_json_get_str(ms, "domain", NULL);
    snprintf(set.host, sizeof(set.host), "%s",
             (s && *s) ? s : cfg->controller);

    if (set.token[0])
        ora_dmp_start(cfg, &set);
    else
        ora_dmp_stop();
}

static void stop_aux_services(void)
{
    ora_rtty_stop();
    ora_dmp_stop();
    ora_capture_stop();
}

/* ---- steady state ---- */

/* Handle one controller request in the steady loop. Returns:
 * 0 keep going, 1 session ended (FORGET/close), negative error. */
static int handle_request(struct ora_tls *t, const struct ora_config *cfg,
                          struct ora_state *st, const struct ora_message *req)
{
    struct ora_header hdr;
    json_object *resp_body = NULL;
    int rc = 0;

    switch (req->hdr.type) {
    case ORA_MSG_SET_REQUEST: {
        /* SET: ack & store. Per-key acks are TOP-LEVEL members of the
         * response body (not nested under a results object), and an
         * empty body makes the controller forget the device. */
        json_object *resp = json_object_new_object();
        json_object *seq_in, *cv_in;

        json_object_object_add(resp, "errcode", json_object_new_int(0));

        /* Echo sequenceId and configVersion without changing their types. */
        seq_in = req->body ? json_object_object_get(req->body, "sequenceId") : NULL;
        if (seq_in)
            json_object_object_add(resp, "sequenceId", json_object_get(seq_in));
        cv_in = req->body ? json_object_object_get(req->body, "configVersion") : NULL;
        if (cv_in) {
            json_object_object_add(resp, "configVersion", json_object_get(cv_in));
            st->config_version = json_object_get_int64(cv_in);
        }

        if (req->body && json_object_is_type(req->body, json_type_object)) {
            json_object_object_foreach(req->body, key, val) {
                json_object *ack;
                const char *vs;

                if (!strcmp(key, "configVersion") || !strcmp(key, "sequenceId"))
                    continue;

                ack = json_object_new_object();
                /* packageCapture is acked with errCode; every other key
                 * uses the lowercase errcode spelling. */
                if (!strcmp(key, "packageCapture"))
                    json_object_object_add(ack, "errCode", json_object_new_int(0));
                else
                    json_object_object_add(ack, "errcode", json_object_new_int(0));
                json_object_object_add(resp, key, ack);

                vs = json_object_to_json_string_ext(val, JSON_C_TO_STRING_PLAIN);
                if (vs) {
                    bool stored = ora_state_set_blob(st, key, vs);

                    ora_log(ORA_LOG_DEBUG, "SET seq %" PRId64
                        " key %s %s", req->hdr.seq, key,
                        stored ? "stored" : "not stored");
                }
            }
        }

        /* aux Tools channels are driven by dedicated SET keys */
        {
            json_object *aux;

            if ((aux = ora_json_get_obj(req->body, "terminalSetting")))
                apply_terminal_setting(cfg, aux);
            if ((aux = ora_json_get_obj(req->body, "monitorServer")))
                apply_monitor_server(cfg, aux);
            if ((aux = ora_json_get_obj(req->body, "packageCapture")))
                ora_capture_handle_set(cfg, aux);
            /* the transfer channel must be connected and handshaked
             * BEFORE this SET_RESPONSE reaches the controller */
            if ((aux = ora_json_get_obj(req->body, "transferChannel")))
                ora_transfer_open(cfg, aux);
        }

        resp_body = resp;
        ora_state_record_cfg_result(st, resp);
        ora_state_save(st);
        break;
    }
    case ORA_MSG_GET_REQUEST: {
        /* GET: per-key answers are TOP-LEVEL members of the response
         * body. Dedicated keys are answered live, the rest echo the
         * config the controller pushed earlier. */
        json_object *seq_in;

        resp_body = json_object_new_object();
        json_object_object_add(resp_body, "errcode", json_object_new_int(0));
        seq_in = req->body ? json_object_object_get(req->body, "sequenceId") : NULL;
        if (seq_in)
            json_object_object_add(resp_body, "sequenceId", json_object_get(seq_in));

        if (req->body && json_object_is_type(req->body, json_type_object)) {
            json_object_object_foreach(req->body, key, val) {
                json_object *live, *b;

                (void)val;
                if (!strcmp(key, "sequenceId"))
                    continue;

                live = ora_inform_get_key(cfg, st, key);
                if (live) {
                    ora_log(ORA_LOG_DEBUG, "GET seq %" PRId64
                            " key %s source live", req->hdr.seq, key);
                    json_object_object_add(resp_body, key, live);
                    continue;
                }
                b = st->set_blobs ? json_object_object_get(st->set_blobs, key) : NULL;
                if (b) {
                    json_object *parsed = json_tokener_parse(
                        json_object_get_string(b));

                    ora_log(ORA_LOG_DEBUG, "GET seq %" PRId64
                            " key %s source stored", req->hdr.seq, key);
                    json_object_object_add(resp_body, key,
                        parsed ? parsed : json_object_new_object());
                } else {
                    ora_log(ORA_LOG_DEBUG, "GET seq %" PRId64
                            " key %s source missing", req->hdr.seq, key);
                }
            }
        }
        break;
    }
    case ORA_MSG_FORGET_REQUEST: {
        resp_body = json_object_new_object();
        json_object_object_add(resp_body, "result", json_object_new_int(0));
        stop_aux_services();
        ora_state_set_adopted(st, false);
        ora_state_set_omadac(st, "");
        ora_state_clear_blobs(st);
        ora_state_save(st);
        rc = 1; /* end session, return to discovery */
        ora_log(ORA_LOG_INFO, "FORGET received; reverting to factory state");
        break;
    }
    case ORA_MSG_UPGRADE_REQUEST: {
        /* decline firmware upgrades (we are not TP-L*nk firmware) */
        resp_body = json_object_new_object();
        json_object_object_add(resp_body, "sequenceId",
            json_object_new_int64(ora_json_get_int(req->body, "sequenceId", 0)));
        json_object_object_add(resp_body, "result", json_object_new_int(-1));
        json_object_object_add(resp_body, "msg",
            json_object_new_string("orouteragent does not accept firmware upgrades"));
        ora_log(ORA_LOG_INFO, "UPGRADE declined");
        break;
    }
    case ORA_MSG_FILE_TRANSFER_REQUEST_V2: {
        /* the capture service replies with the partition itself */
        if (ora_capture_handle_transfer_request(req->body, req->hdr.seq))
            return 0;
        resp_body = json_object_new_object();
        {
            json_object *ft = json_object_new_object();

            json_object_object_add(ft, "errCode", json_object_new_int(1));
            json_object_object_add(resp_body, "fileTransfer", ft);
        }
        break;
    }
    case ORA_MSG_NOTIFY_REQUEST:
    case ORA_MSG_NOTIFY_REQUEST_V2:
        resp_body = json_object_new_object();
        break;
    case ORA_MSG_INFORM_RESPONSE:
        return 0; /* nothing to answer */
    default:
        /* unknown type: ignore silently (controller may probe) */
        return 0;
    }

    {
        int64_t rtype;
        switch (req->hdr.type) {
        case ORA_MSG_SET_REQUEST: rtype = ORA_MSG_SET_RESPONSE; break;
        case ORA_MSG_GET_REQUEST: rtype = ORA_MSG_GET_RESPONSE; break;
        case ORA_MSG_FORGET_REQUEST: rtype = ORA_MSG_FORGET_RESPONSE; break;
        case ORA_MSG_UPGRADE_REQUEST: rtype = ORA_MSG_UPGRADE_RESPONSE; break;
        case ORA_MSG_FILE_TRANSFER_REQUEST_V2:
            rtype = ORA_MSG_FILE_TRANSFER_RESPONSE_V2;
            break;
        case ORA_MSG_NOTIFY_REQUEST: rtype = ORA_MSG_NOTIFY_REPLY; break;
        case ORA_MSG_NOTIFY_REQUEST_V2: rtype = ORA_MSG_NOTIFY_REPLY_V2; break;
        default: rtype = ORA_MSG_SET_RESPONSE; break;
        }
        ora_header_init(&hdr, cfg->mac, rtype, NULL);
        /* echo the controller's seq in the response header */
        hdr.seq = req->hdr.seq;
        if (!send_msg_sync(t, &hdr, resp_body)) {
            rc = -1;
        }
    }
    json_object_put(resp_body);
    return rc;
}

enum ora_manage_result ora_manage_run(const struct ora_config *cfg,
                                      struct ora_state *st,
                                      volatile bool *stop)
{
    struct ora_tls t;
    enum ora_manage_result res = ORA_MANAGE_RECONNECT;
    uint64_t last_inform = 0;

    if (!ora_tls_init(&t))
        return ORA_MANAGE_FATAL;

    if (!ora_tls_connect(&t, cfg->controller, cfg->adopt_port,
                         ORA_CONNECT_TIMEOUT_S * 1000, cfg->verify_tls)) {
        ora_tls_free(&t);
        return ORA_MANAGE_RECONNECT;
    }
    ora_tls_set_keepalive(&t, 30, 10, 3);

    if (handshake(&t, cfg, st, stop) != 0) {
        ora_tls_close(&t);
        ora_tls_free(&t);
        if (st->adopted)
            ora_log(ORA_LOG_WARN,
                    "resume handshake failed; retaining adoption state for retry");
        return ORA_MANAGE_RECONNECT;
    }

    /* publish the channel so capture/transfer threads can emit on it */
    mgmt_channel_set(&t, cfg->mac, st->omadac_id);

    /* steady state */
    while (!*stop) {
        uint64_t now = ora_now_ms();
        int rc;

        if (now - last_inform >= (uint64_t)cfg->inform_interval * 1000) {
            struct ora_header hdr;
            json_object *body = ora_inform_build_body(cfg, st);
            ora_header_init(&hdr, cfg->mac, ORA_MSG_INFORM_REQUEST,
                            st->omadac_id);
            if (!send_msg_sync(&t, &hdr, body)) {
                json_object_put(body);
                res = ORA_MANAGE_RECONNECT;
                goto out;
            }
            json_object_put(body);
            last_inform = now;
        }

        if (ora_tls_wait_readable(&t, 500)) {
            struct ora_message msg;
            if (!recv_msg(&t, &msg, ORA_IO_TIMEOUT_S * 1000)) {
                res = ORA_MANAGE_OK; /* controller closed */
                goto out;
            }
            rc = handle_request(&t, cfg, st, &msg);
            ora_msg_free(&msg);
            if (rc < 0) {
                res = ORA_MANAGE_RECONNECT;
                goto out;
            }
            if (rc == 1) {
                res = ORA_MANAGE_OK; /* FORGET -> rediscover */
                goto out;
            }
        }
    }
    res = ORA_MANAGE_OK; /* stop requested */
out:
    mgmt_channel_clear();
    stop_aux_services();
    ora_tls_close(&t);
    ora_tls_free(&t);
    return res;
}
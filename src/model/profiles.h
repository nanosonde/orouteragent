/* orouteragent - emulated gateway model profile data */
#ifndef ORA_PROFILES_H
#define ORA_PROFILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One components_v2 entry: component name -> protocol version string. */
struct ora_component {
    const char *name;
    const char *version;
};

/* One portInfos[] entry (devCap). */
struct ora_port_info {
    int port;
    const char *name;
    int type;                    /* 0 WAN, 1 WAN/LAN, 2 LAN */
    int mode;
    bool has_max_bandwidth;      /* ER605/ER706W LAN ports omit this field */
    int max_bandwidth;           /* Mbps; SFP ports use 10000 */
    const char *default_speed_duplex;
    const char *speed_duplex_list[8];
    int n_speed_duplex;
    int support_internet_vlan;
    int support_iptv;
    int support_mirror;
    int support_poe;
};

/* devCap.specification capacities. */
struct ora_spec {
    int acl_num;
    int bandwidth_ctrl_num;
    int client_ip_binding_num;
    int ddns_num;
    int ip_group_num;
    int ipv6_group_num;
    int ldap_class_rules_num;
    int nat_pf_num;
    int network_num;
    int policy_routing_num;
    int qos_class_rules_num;
    int service_type_num;
    int session_limit_num;
    int ssl_vpn_connections_num;
    int ssl_vpn_locks_num;
    int ssl_vpn_resource_groups_num;
    int ssl_vpn_resources_num;
    int ssl_vpn_user_groups_num;
    int ssl_vpn_users_num;
    int static_routing_num;
    int url_filtering_num;
    int vpn_ipsec_num;
    int vpn_l2tp_client_num;
    int vpn_openvpn_num;
    int vpn_pptp_client_num;
    int vpn_users_num;
    int wireguard_all_peer_num;
    int wireguard_num;
    int wireguard_peer_num;
};

/* Immutable model data shared for the lifetime of the process. Optional
 * has_* flags control wire-visible field presence and must remain consistent
 * with the selected model's management profile. */
struct ora_model_profile {
    const char *name;            /* "ER605" etc; UCI value */
    const char *model;
    const char *model_ver;
    const char *hw_ver;          /* "1.0" */
    const char *fw_ver;          /* "<x.y.z> Build <YYYYMMDD> Rel.<n>" */
    const char *hw_id;
    const char *encrypted_hw_id;
    const char *oem_id;
    const char *encrypted_oem_id;
    const char *lan_mac_template;/* base MAC for derived MACs */
    int wireless;                /* 0/1: model has WiFi (ER706W) */

    /* components_v2 */
    const struct ora_component *components;
    size_t n_components;

    /* devCap.portInfos */
    const struct ora_port_info *ports;
    size_t n_ports;

    const struct ora_spec *spec;

    /* deviceMisc */
    int port_num;
    int extra_port;
    int usb_lte_wan;

    /* devCap scalars */
    int ipsec_num;
    int max_ssl_vpn_user_concurrent_num;
    int max_vpn_user_concurrent_num;
    int default_igmp_wan;
    const int *mandatory_ports;  /* currently empty for all models */
    size_t n_mandatory_ports;
    const struct ora_port_info *extra_port_infos; /* currently empty */
    size_t n_extra_port_infos;

    int support_acl_disable;
    bool support_all_wan;
    /* The following devCap keys are omitted by some profiles;
     * has_* controls their wire-visible presence. */
    bool has_discrete_wan;
    int support_discrete_wan;
    bool has_wlb;
    bool support_wan_load_balance;
    bool has_lte;
    bool support_lte;
    bool has_sdwan;
    bool support_sdwan;
    int support_ipsec_failover;
    int support_routing_vpn_client;
    int support_vpn_usb;
    int support_vpn_verify;
    bool supports_ipv6;
    bool support_poe;
};

/* All four profiles, in order. */
const struct ora_model_profile *ora_profiles_all(size_t *n);

/* Case-insensitive lookup by UCI model name; unknown/empty -> ER707-M2. */
const struct ora_model_profile *ora_profile_lookup(const char *model);

#endif
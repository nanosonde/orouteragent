# Controller SET-to-UCI Mapping

This document assigns every controller SET payload from [gateway-config-reference.md](gateway-config-reference.md) a proposed native OpenWrt owner and apply strategy.

## Current State

Today, `src/config.c` reads only `/etc/config/orouteragent` bootstrap and identity options.

`src/services/manage.c` acknowledges every SET key and stores its JSON in `state.json`.

The initial bidirectional transform foundation is in `src/config/`. The shared
registry in `src/config/registry.c` classifies each key, one source file per
native domain holds that domain's rules, and `src/config/uci.c` is the only
boundary that reads or writes native packages. It covers the first native
mapping set (`wanBasicSetting`, `wanIpv4`, `network`, `lanDns`, `dnsCache`,
`natPf`, `staticRouting`, `ssh`, and `timeSetting`) and is used by the
controller SET path and the UCI ownership boundary. Native writes remain
disabled until a key-specific transformer is implemented; `ssh` is applied when
`dropbear` is explicitly opted in. See
[config-implementation-status.md](config-implementation-status.md).

Only `terminalSetting`, `monitorServer`, `transferChannel`, and `packageCapture` invoke live side effects.

No controller SET currently writes UCI, reloads a service, or changes router behavior.

**Current** means shipped behavior. **Proposal** means future work.

## Apply Contract

- Keep `/etc/config/orouteragent` for agent bootstrap, identity, and port mapping; do not mirror controller payloads into it.
- Parse and validate a whole SET body before modifying native packages. Replace authoritative entry lists atomically using stable controller IDs.
- Commit affected UCI packages only after every translation succeeds, then reload each owning service. On failure, retain previous UCI and `state.json` values and return a redacted non-zero `errcode`.
- Retain original JSON in `state.json` for GET replay, including permitted unknown `wanIpv4.settings[]` fields. Never log passwords, tokens, private keys, certificates, or auth codes; secret-bearing files must remain root-readable only.
- A named UCI package is a proposed native owner, not proof that it is installed. Missing optional dependencies require `Stub` or `Decline`, never a false applied acknowledgement.

## Inventory Rules

The source reference contains 163 distinct top-level SET names and 164 appearances because `system` occurs in Services and Auxiliary tools.

These tables use one row per distinct wire key, split grouped source rows, preserve exact spelling, exclude `dnsCache (GET)`, and retain `arptable` once despite its GET use.

All config rows advance `configVersion`; command rows do not. Unless noted, the ack key equals the SET key and uses `errcode`.

## WAN and Connectivity

| SET key | Kind / ack | Current | Proposal | Apply and comments |
|---|---|---|---|---|
| `wanBasicSetting` | config | State | UCI `network`: generated port-role devices/interfaces | Resolve only configured `portmap` entries; validate each port exists, commit `network`, then `ubus call network reload`. |
| `wanIpv4` | config, per-entry | State | UCI `network` interface per WAN port | Map `dhcp`, `static`, and `pppoe`; validate addresses, netmasks, DNS, MTU, VLAN, and port references. Preserve unknown `settings[]` fields for GET. |
| `wanIpv6` | config | State | UCI `network` WAN6 interfaces | Map DHCPv6, prefix delegation, static addresses, DNS, and RA flags; reload `network`. Protect PPP credentials. |
| `wanMac` | config | State | UCI `network` device `macaddr` | Normalize and validate MAC addresses; reload `network`; reject multicast/broadcast addresses. |
| `wanIpv4Usb` | config | Stub | UCI `network` QMI/MBIM interface when an LTE profile declares hardware | Otherwise Decline. Require installed modem protocol package; validate APN/PIN and keep credentials secret. |
| `wanLoadBalance` | config | State | `mwan3` generated members, policies, and rules | Require `mwan3`; validate referenced WANs and positive weights; commit `mwan3`, reload it after `network`. |
| `onlineDetection` | config | State | State | Controller polling hint, not a router health-check setting; validate positive interval and supported unit. |
| `connect` | config | State | Runtime `network.interface.*` ubus down/up/renew | Resolve `portId` through `portmap`; allow only connect/disconnect/reconnect; do not persist an action. |
| `speedTest` | command | State | Runtime DMP result producer | Require `cmdId` and valid ports; current implementation has no measurement engine, so Stub until a result producer exists. |
| `speedTestSchedule` | config | State | UCI `cron` generated job plus speed-test service | Validate schedule ranges and target ports; install only with a measurement engine, otherwise Stub. |
| `virtualWan` | config | Stub | UCI `network` DSL/ATM virtual interfaces | Model-gated; require compatible DSL/discrete-WAN hardware and protocol packages. Validate VPI/VCI and nested IPv4 mode. |
| `sdwan` | config | Stub | State plus dedicated SD-WAN service configuration | Model-gated; no native stock UCI owner. Require a supported backend; preserve `custom_network` spelling. |
| `lte` | config | Stub | UCI `network` modem interface plus modem manager settings | Model-gated; require modem hardware and packages. `5Gbands`, SIM2 suffixes, and quota floats are wire data. |
| `dsl` | config | Stub | UCI `network` DSL interface and driver settings | Model-gated; require DSL hardware. Validate VPI/VCI, modulation, and credentials before reload. |
| `iptv` | config | State | UCI `network` DSA bridge VLANs and multicast settings | Validate string port IDs and VLAN ranges; coordinate WAN role changes; reload `network` and multicast service if installed. |

## LAN, DHCP, and DNS

| SET key | Kind / ack | Current | Proposal | Apply and comments |
|---|---|---|---|---|
| `network` | config | State | UCI `network`, `dhcp`, and `odhcpd` generated sections | Translate LAN bridges, addresses, DHCP pools/options, IPv6, and RA. `interface` is a JSON key, not a C identifier. Validate CIDRs and pools before reload. |
| `lanDns` | config | State | UCI `dhcp` dnsmasq host/address/server entries | Validate domains and IP literals; commit `dhcp`, reload dnsmasq. Do not translate controller IDs into host names. |
| `dnsProxy` | config | State | UCI `dhcp` for plain forwarding; resolver-specific configuration for encrypted DNS | DoH/DoT/DNSSEC needs an installed resolver backend; Stub those modes if absent. Validate endpoints and network references. |
| `dnsCache` | config | State; GET is live | UCI `dhcp` dnsmasq cache/TTL settings | Validate TTL and operation; commit/reload dnsmasq. GET remains a live cache report, not replayed configuration. |
| `dpiTraffic` | command | State | Runtime DMP traffic collector | Require an installed DPI collector and a valid endpoint/token; otherwise Stub and redact token. |

## Firewall, Security, and QoS

| SET key | Kind / ack | Current | Proposal | Apply and comments |
|---|---|---|---|---|
| `firewallConfig` | config | State | UCI `system` sysctl entries | Validate timeout bounds and booleans; apply sysctls after commit. Keep conntrack timeout names explicitly mapped. |
| `attackDefense` | config | State | UCI `firewall` rules plus `system` sysctls | Generate fw4/nftables limits and relevant sysctls; validate rates/thresholds; reload firewall. |
| `acl` | config, per-entry | State | UCI `firewall` generated rules and nft sets | Resolve group/time references before replacing authoritative rule list; reload firewall. |
| `aclHit` | config | State | Runtime nft counter reset | `clear` must reset only generated ACL counters; GET is a live counter report. |
| `wirelessAcl` | config | State | UCI `firewall` generated rules | Apply only for WiFi-capable profile; otherwise Decline. Reuse ACL validation and reload firewall. |
| `customAcl` | config | State | UCI `firewall` generated rules | Validate ports, protocol, directions, and referenced service types; reload firewall. |
| `urlFiltering` | config | State | Resolver policy service configuration | Requires a DNS filtering backend; otherwise State/Stub. Validate URLs/categories without logging user policy data. |
| `wirelessUrlFiltering` | config | State | Resolver policy plus WiFi client classification | WiFi-gated and requires filtering backend; otherwise Decline or Stub. |
| `macFilter` | config | State | UCI `firewall` nft MAC sets/rules | Validate MACs and direction; replace generated set atomically; reload firewall. |
| `ipMacBinding` | config | State | UCI `dhcp` host entries plus ARP/firewall enforcement | Validate address/MAC/network relations; commit `dhcp` and `firewall`, reload both. |
| `sessionLimit` | config, per-entry | State; GET is live | UCI `firewall` nftables limits | Validate source references and positive limits; reload firewall. GET should report actual counters. |
| `bandwidthCtrl` | config, per-entry | State | Dedicated tc/qdisc service with generated state | UCI does not natively own full shaping. Validate rates and ports; apply with transactional tc replacement and rollback. |
| `qos` | config, per-entry | State | Dedicated tc/nft DSCP service with generated state | Validate DSCP, classes, and ports; require shaping backend; otherwise Stub. |
| `ips` | config | Stub | IPS engine policy configuration | Require an IPS backend; otherwise Stub with no false enforcement claim. |
| `ipsWhiteList` | config, per-entry | Stub | IPS engine allow-list | Require IPS engine; validate entity IDs as strings. |
| `ipsBlackList` | config, per-entry | Stub | IPS engine block-list | Require IPS engine; validate entity IDs as strings. |
| `signatureList` | config, per-entry | Stub | IPS engine signature policy | Require IPS engine; validate long `sid` values. |
| `blockCountry` | config, per-entry | Stub | Geo-IP firewall/IPS policy | Require a maintained Geo-IP dataset and backend; otherwise Stub. |
| `countryGroup` | config | Stub | Geo-IP group state for policy backend | Require Geo-IP backend; validate country codes. |
| `serviceType` | config, per-entry | State | Agent-owned reference data plus UCI `firewall` consumers | Validate protocol and port-range strings; apply only as referenced by ACL/NAT rules. |

## NAT, Routing, Groups, and Time

| SET key | Kind / ack | Current | Proposal | Apply and comments |
|---|---|---|---|---|
| `natPf` | config, per-entry | State | UCI `firewall` redirect rules | Validate IP/port/protocol/WAN references; `enable` is a string. Reload firewall; report live counters under INFORM `portforward`. |
| `oneToOneNat` | config, per-entry | State | UCI `firewall` SNAT/DNAT rules | Validate translated/original addresses and interface IDs; `status` is an integer; reload firewall. |
| `disableNat` | config, per-entry | State | UCI `firewall` NAT exemption rules | Validate network references; `status` is a string and `lan_network` uses snake_case; reload firewall. |
| `natAlg` | config | State | UCI `firewall` helper configuration | Enable only helpers supported by fw4/kernel modules; validate listed ports; reload firewall. |
| `staticRouting` | config, per-entry | State | UCI `network` route sections | Validate CIDR, metric, next hop, and interface; `interface` is wire spelling; reload network. |
| `policyRouting` | config, per-entry | State | UCI `network` rules/routes and a policy-routing backend | Validate referenced interfaces/VPNs and tables; require backend for complex policies; reload network. |
| `ipGroup` | config, per-entry | State | Agent-owned reference data and nft sets | Validate IPv4 CIDRs; materialize only when referenced; reload firewall. |
| `ipv6Group` | config | State | Agent-owned reference data and nft sets | Validate IPv6 CIDRs; materialize only when referenced; reload firewall. |
| `ipPortGroup` | config | State | Agent-owned reference data and nft sets | Validate CIDRs and ordered port-range pairs; reload firewall when consumed. |
| `ipv6PortGroup` | config | State | Agent-owned reference data and nft sets | Validate IPv6 CIDRs, ranges, and masks; reload firewall when consumed. |
| `macGroup` | config | State | Agent-owned reference data and nft sets | Normalize MACs; materialize only when referenced; reload firewall. |
| `domainGroup` | config | State | Resolver set configuration plus agent reference data | Validate FQDNs and string port ranges; require dnsmasq nftset support or a resolver backend. |
| `domainNoPortGroup` | config, ack `fqdnGroup` | State | Resolver set configuration plus agent reference data | Preserve request/ack name difference; validate FQDNs and reload resolver when consumed. |
| `timeRange` | config, per-entry | State | Agent-owned schedule data plus cron/nft time expressions | Validate day and time ranges; regenerate consumers atomically. |

## VPN

| SET key | Kind / ack | Current | Proposal | Apply and comments |
|---|---|---|---|---|
| `vpn` | config, per-entry | State | Dedicated strongSwan, OpenVPN, and xl2tpd/pppd adapters | Split by tunnel subtype; require installed backend and profile capability. Protect PSKs, passwords, certs, and private keys. Preserve mixed snake/camel wire names. |
| `vpnUser` | config, ack `vpnUsers` | State | VPN backend user database adapter | Preserve ack rename; validate user/server references and store passwords protected. |
| `ipsecFailover` | config | State | strongSwan failover policy adapter | Requires strongSwan and multiple valid candidates; no stock UCI equivalent. |
| `sslVpn` | config | Stub | Dedicated OpenVPN-based SSL-VPN adapter | No stock implementation is selected; Stub until a compatible service and user lifecycle exist. |
| `wireguard` | config | State | UCI `network` WireGuard interfaces and peers | Validate keys, endpoints, addresses, and peer/interface references; commit/reload network. Keep private and preshared keys secret. |
| `radiusProfile` | config | State | State plus VPN/RADIUS backend adapter | Do not write unused profiles to generic UCI; validate hosts/ports and protect shared secrets. |
| `ldap` | config | State | State plus VPN/LDAP backend adapter | Validate endpoint and TLS material; port is a wire string. Protect bind password, key, and certificate. |

## Services and System

| SET key | Kind / ack | Current | Proposal | Apply and comments |
|---|---|---|---|---|
| `snmp` | config | State | UCI `snmpd` | Require `snmpd`; validate location/contact and protect communities/v3 credentials; reload snmpd. |
| `ssh` | config | Applied when `dropbear` is opted in | UCI `dropbear` | Maps `global` to `enable` and `port` to `Port`, reloads Dropbear, rolls back UCI after reload failure, and preserves `l3_enable` in controller state. |
| `led` | config | State | UCI `system` LED sections plus Runtime locate trigger | Map persistent enable through LED configuration; implement locate as bounded runtime action. |
| `lldp` | config | State | UCI `lldpd` | Require `lldpd`; reload it and add live INFORM only after service status exists. |
| `hwOffload` | config | State | UCI `firewall` flow offloading option | Validate platform support; reload firewall. Do not enable hardware offload where unsupported. |
| `jumbo` | command | State | Runtime/UCI `network` device MTU | Validate MTU per device capability; update device sections and reload network only on supported hardware. |
| `upnp` | config | State | UCI `upnpd` | Require `miniupnpd`; validate selected interfaces/networks; reload miniupnpd. |
| `mdns` | config | State | UCI `avahi` or mDNS backend | Require selected mDNS package; validate service/rule names and network references; reload backend. |
| `ddns` | config, per-entry | State | UCI `ddns` generated sections | Validate provider/interface/domain; protect credentials; reload ddns. |
| `mail` | config | State | State plus notification service adapter | No generic OpenWrt mail UCI owner; protect auth codes and only apply with an installed sender. |
| `poe` | config | State | Platform-specific PoE runtime adapter | Require PoE-capable profile/driver; validate port ownership. Otherwise Decline. |
| `port` | config, per-entry | State | UCI `network` device/bridge VLAN settings | Validate PVID, port list, speed settings, and hardware features; use runtime switch API where UCI lacks control. |
| `speedDuplex` | config | State | Runtime ethtool/switch adapter | Validate advertised speed/duplex against port capability; persist only if platform supports it. |
| `mirror` | config | State | Runtime switch mirroring adapter | Require switch support; validate source/target distinctness; otherwise Decline. |
| `echoServer` | config | State | Dedicated echo service configuration | Require a selected echo daemon; validate address and reload that service. |
| `timeSetting` | config | State | UCI `system` and time service | Validate time zone/NTP endpoints; commit system configuration and restart time service. |
| `dstConfig` | config, ack `dst` | State | UCI `system` timezone/DST settings | Preserve ack rename; validate offsets and date rules; reload time service. |
| `remoteLog` | config, ack `logSetting` | State | UCI `system` logging or syslog adapter | Preserve ack rename; require remote logging backend, validate endpoint, protect mail credentials. |
| `logNotification` | config | State | State plus notification adapter | Enable only events a local notification backend can emit; otherwise State. |
| `rebootSchedule` | config | State | UCI `cron` generated jobs | Validate schedule values; generate uniquely tagged jobs; reload cron. |
| `system` | command | Declined no-op | Runtime reboot adapter | Keep current safe no-op unless an explicit policy permits reboot. Never allow arbitrary actions. |
| `common` | config | State | UCI `system` hostname/description policy | Map only a safe device label if configured; avoid changing hostname without explicit policy. |
| `standaloneMgmt` | config | State | State | Controller-specific management flags have no OpenWrt equivalent. |
| `controllerInfo` | config | State | State | Controller identity metadata; preserve for protocol behavior, do not write network settings. |
| `privacyPolicy` | config | State | State | Empty object is valid; no native router setting is implied. |
| `sideParams` | command | State | State | Auxiliary controller flag; validate boolean and do not alter router behavior. |
| `abnormalDetect` | command | State | Monitoring service adapter | Require a defined detector and report path; otherwise Stub. |
| `intervalConfig` | command | State | Agent runtime scheduling state | Validate bounded intervals per section; persist only agent-owned report preferences. |
| `informSubscribe` | command | State | Agent runtime subscription state | Validate known sections and dotted wire keys exactly; do not write unrelated UCI. |

## Clients

| SET key | Kind / ack | Current | Proposal | Apply and comments |
|---|---|---|---|---|
| `client` | command | State | Runtime firewall client block/unblock adapter | Validate MACs; add/remove only generated firewall rules; reload firewall. |
| `clientOpt` | command | State | Runtime client policy adapter | Validate operation and target client; use firewall/DHCP owner as appropriate. |
| `clientIpBinding` | config | State | UCI `dhcp` host sections | Validate MAC, IP, VLAN/network reference, and DHCP options; commit/reload dnsmasq. |
| `clientTrafficRequire` | command | State | Runtime traffic collector request | Validate MAC set; return data only if collector can provide it, otherwise Stub. |
| `clientRateConfig` | config | State | Dedicated tc/qdisc client policy service | Validate rate and action; require shaping backend; otherwise Stub. |
| `clientOperation` | config | State | Runtime DHCP/firewall client adapter | Validate MAC, operation, and VLAN before invoking owner. |
| `clearSettings` | command | State | Runtime generated-config reset policy | Require explicit allowlist of agent-generated sections; never reset operator-owned OpenWrt configuration. |
| `highAbility` | command, ack `backupConfig` | State | State | Preserve ack rename; backup configuration is controller metadata without a native target. |

## Wireless

All rows in this section apply only to a WiFi-capable profile such as ER706W. On other profiles, Decline rather than applying wireless settings to unrelated hardware. Where hostapd/UCI cannot represent a requested feature, retain State or Decline it explicitly.

| SET key | Kind / ack | Current | Proposal | Apply and comments |
|---|---|---|---|---|
| `wirelessBasic_2G` | config | State | UCI `wireless` radio | Map enable/channel/width/power; validate radio band and reload wireless. |
| `wirelessBasic_5G` | config | State | UCI `wireless` radio | Map enable/channel/width/power; validate radio band and reload wireless. |
| `wirelessBasic_5G2` | config | State | UCI `wireless` radio | Map only when a third radio exists; otherwise Decline. |
| `wirelessAdv_2G` | config | State | UCI `wireless`/hostapd adapter | Validate beacon/DTIM/RTS/fragment/rates; reload wireless. |
| `wirelessAdv_5G` | config | State | UCI `wireless`/hostapd adapter | Validate beacon/DTIM/RTS/fragment/rates; reload wireless. |
| `WIRELESS_ADV_5G2` | config, ack `wirelessAdv_5G2` | State | UCI `wireless`/hostapd adapter | Preserve uppercase request spelling and lowercase ack name; require third radio. |
| `ssid_2G` | config | State | UCI `wireless` wifi-iface sections | Validate SSID/security/VLAN/RADIUS references; protect PSKs and credentials; reload wireless. |
| `ssid_5G` | config | State | UCI `wireless` wifi-iface sections | Validate SSID/security/VLAN/RADIUS references; protect PSKs and credentials; reload wireless. |
| `ssid_5G2` | config | State | UCI `wireless` wifi-iface sections | Require third radio; validate and reload wireless. |
| `limits_2G` | config | State | Hostapd/tc rate-limit adapter | Require a supported shaping method; validate per-SSID limits. |
| `limits_5G` | config | State | Hostapd/tc rate-limit adapter | Require a supported shaping method; validate per-SSID limits. |
| `limits_5G2` | config | State | Hostapd/tc rate-limit adapter | Require third radio and shaping backend. |
| `loadBalance_2G` | config | State | Hostapd client-limit adapter | Validate radio and thresholds; apply only supported hostapd options. |
| `loadBalance_5G` | config | State | Hostapd client-limit adapter | Validate radio and thresholds; apply only supported hostapd options. |
| `loadBalance_5G2` | config | State | Hostapd client-limit adapter | Require third radio. |
| `rssi_2G` | config | State | Hostapd RSSI policy adapter | Validate threshold range and apply only if driver supports it. |
| `rssi_5G` | config | State | Hostapd RSSI policy adapter | Validate threshold range and apply only if driver supports it. |
| `rssi_5G2` | config | State | Hostapd RSSI policy adapter | Require third radio and driver support. |
| `roaming` | config | State | Hostapd/roaming backend adapter | Require supported steering backend; preserve state otherwise. |
| `roaming_cmd` | command, ack `roaming` | State | Runtime roaming backend command | Validate requested action and backend capability; no UCI persistence. |
| `mesh` | config | State | 802.11s/mesh backend adapter | Require compatible radios and mesh design; otherwise Decline. |
| `mesh_cmd` | command, ack `mesh` | State | Runtime mesh backend command | Validate action; no UCI persistence. |
| `bandSteering` | config | State | Hostapd steering backend adapter | Require multi-band capability; otherwise Stub. |
| `ppskV3` | config | State | Hostapd PPSK backend adapter | Require compatible hostapd; protect PSKs; otherwise Decline. |
| `macFilterGlobal` | config | State | UCI `wireless` access-control policy | Validate mode and reload wireless. |
| `macFilterList` | config | State | UCI `wireless` MAC list policy | Normalize MACs and reload wireless. |
| `macFilterAssoc` | config | State | UCI `wireless` SSID-to-list association | Validate referenced SSIDs/lists; reload wireless. |
| `schedulerGlobal` | config | State | Wireless scheduler backend | Require scheduler backend; otherwise Stub. |
| `schedulerList` | config | State | Wireless scheduler backend | Validate schedules; otherwise Stub. |
| `schedulerAssoc` | config | State | Wireless scheduler backend | Validate associations; otherwise Stub. |
| `qosConfig_2G` | config | State | Hostapd/tc QoS adapter | Validate radio and QoS capability; require backend. |
| `qosConfig_5G` | config | State | Hostapd/tc QoS adapter | Validate radio and QoS capability; require backend. |
| `qosConfig_5G2` | config | State | Hostapd/tc QoS adapter | Require third radio and QoS backend. |
| `rogueApScan` | command | State | Runtime wireless scan | Require driver scan support; return results through the correct monitor path. |
| `rfScan` | command | State | Runtime RF survey | Require driver survey support; otherwise Decline. |
| `channelDeploy` | command | State | Runtime wireless channel planner | Require planner backend; validate assignments; reload wireless on success. |
| `powerControl` | command | State | Runtime radio power control | Validate supported transmit powers; reload wireless on success. |

## VoIP

All VoIP rows are Decline on the shipped profiles because they expose no FXS hardware. A future profile must provide a dedicated telephony backend before any UCI mapping is enabled.

| SET key | Kind / ack | Current | Proposal | Apply and comments |
|---|---|---|---|---|
| `dnd` | config | Stub | Telephony backend configuration | Require FXS backend; validate schedule and mode. |
| `callLog` | config | Stub | Telephony backend configuration | Require FXS backend; protect call records. |
| `emergencyNumber` | config | Stub | Telephony backend configuration | Require FXS backend; validate number format. |
| `telephoneNumberIpv6Settings` | config | Stub | Telephony backend configuration | Require FXS backend and IPv6 SIP support. |
| `callBlocking` | config | Stub | Telephony backend configuration | Require FXS backend; validate rules. |
| `telephoneNumber` | config | Stub | Telephony backend configuration | Require FXS backend; protect credentials. |
| `voipDeviceSettings` | config | Stub | Telephony backend configuration | Require FXS backend; validate port ownership. |
| `telephoneBook` | config | Stub | Telephony backend configuration | Require FXS backend; retain private contacts securely. |
| `telephoneNumberAdvancedSettings` | config | Stub | Telephony backend configuration | Require FXS backend; validate bound interface. |
| `callForwarding` | config | Stub | Telephony backend configuration | Require FXS backend; validate destination numbers. |
| `voiceMail` | command | Stub | Telephony backend runtime command | Require FXS backend and protected storage. |
| `voiceMailSettings` | config | Stub | Telephony backend configuration | Require FXS backend; protect PIN and recordings. |
| `voiceMailDownload` | command | Stub | Telephony backend transfer command | Require FXS backend and authorization. |
| `installationType` | config | Stub | Telephony backend configuration | Require FXS backend; validate channel limit. |

## Auxiliary Channels and Tools

| SET key | Kind / ack | Current | Proposal | Apply and comments |
|---|---|---|---|---|
| `terminalSetting` | command | Existing live RTTY handler | Runtime RTTY service | Do not persist token; validate port, heartbeat, and exact uppercase `SSL`; stop service when disabled. |
| `monitorServer` | config | Existing live DMP handler | Runtime DMP service | Keep token, `aesKey`, and `iv` protected; validate protocol/domain/path; stop previous client before replacement. |
| `transferChannel` | command, `errCode` | Existing live transfer handler | Runtime transfer service | Connect and handshake before SET response; protect token and key material. |
| `packageCapture` | command, `errCode` | Existing live capture handler | Runtime capture service | Validate duration, total size, interface, filter, and port range; retain exact `errCode` capitalization. |
| `deviceDebugInfo` | command | State | Runtime support-bundle service | Decline until an authenticated, size-bounded bundle producer and transfer path exist. |
| `ping` | command | State; DMP probe support | Runtime DMP probe | Validate `cmdId`, destination, and `interface`; send result over DMP, not SET. |
| `traceroute` | command | State; DMP probe support | Runtime DMP probe | Validate hops/timeouts and send result over DMP, not SET. |
| `dnslookup` | command | State | Runtime DMP probe | Implement only with a bounded resolver request; send result over DMP, not SET. |
| `arptable` | command | State; GET is live | Runtime GET/DMP ARP query | Validate query selectors; do not create persistent UCI state. |
| `delayEffect` | command | State | Agent runtime apply-delay policy | Bound delay and effect; never delay security-critical changes without explicit policy. |
| `userAccount` | config | State account update | Agent managed-account state | Validate current/new account material; protect passwords and retain authentication representation across restart. |
| `controllerSetting` | config | State | Agent controller identity state | Preserve controller metadata only; do not map it to system networking. |
| `portalFreePolicyConfig` | config, ack `portalFreePolicy` | State | Portal backend policy adapter | Preserve ack rename; requires portal backend; validate URL policy. |
| `facebookV2` | config | State | Portal backend policy adapter | Requires portal backend; protect certificate/private key and validate redirect IPs. |

## Completion Checklist

- Compare the first column of these tables with the expanded section 3 inventory in [gateway-config-reference.md](gateway-config-reference.md): 163 distinct keys and no duplicates.
- Preserve renamed acknowledgements: `remoteLog`/`logSetting`, `dstConfig`/`dst`, `vpnUser`/`vpnUsers`, `domainNoPortGroup`/`fqdnGroup`, `highAbility`/`backupConfig`, and `portalFreePolicyConfig`/`portalFreePolicy`.
- Preserve protocol quirks: `packageCapture` and `transferChannel` use `errCode`; `WIRELESS_ADV_5G2` is uppercase; `SSL`, `5Gbands`, `custom_network`, `dhcp_opts`, and `dhcpOptions` are exact wire names.
- Add focused tests for each future implementation category: translation, validation failure with no mutation, generated-section replacement, reload failure rollback, GET replay, and redacted logging.
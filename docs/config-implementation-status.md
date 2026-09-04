# Controller Configuration Implementation Status

This document is the maintained implementation ledger for every controller
configuration key in [gateway-config-reference.md](gateway-config-reference.md).
It is updated with each mapping implementation. Wire spellings, acknowledgement
aliases, and error-key casing are protocol contracts.

## Status Terms

- **Registered, staged only**: parsed by the shared transform boundary when
  applicable and retained in `state.json`; no native UCI package is changed.
- **Applied forward only**: writes native UCI when its domain is opted in; GET
  still returns stored controller state because the wire representation cannot
  yet be reconstructed without loss.
- **Live command**: invokes its documented runtime service rather than writing
  persistent UCI configuration.
- **Stored only**: retained as controller state pending a transform.
- **Stub**: a native dependency or hardware backend is absent.
- **Declined**: intentionally not performed.

## Foundation

The shared transform registry is used by controller SET handling and the native
UCI ownership boundary. `managed_domains` is an opt-in UCI list, empty by
default. It accepts `network`, `dhcp`, `firewall`, `system`, and `dropbear`.
Until a key has an applied transformer, opting in keeps valid payloads staged;
it does not alter operator configuration.

| Key | Status | Native domain | Reverse source | Notes |
|---|---|---|---|---|
| `wanBasicSetting` | Registered, staged only | `network` | Stored state | Array shape validated. |
| `wanIpv4` | Applied forward only | `network` | Stored state | DHCP, static, and PPPoE settings apply to netifd when opted in; secrets are not exported. |
| `network` | Registered, staged only | `network`, `dhcp` | Stored state | Top-level list shape validated. |
| `lanDns` | Applied bidirectionally | `dhcp` | UCI plus stored wire-only fields | Agent-owned DNS records use deterministic sections and leave operator records unchanged. |
| `dnsCache` | Applied forward only | `dhcp` | Live GET remains unchanged | dnsmasq cache settings apply when opted in; GET reports live cache data. |
| `natPf` | Applied bidirectionally | `firewall` | UCI plus stored wire-only fields | Agent-owned redirect sections use deterministic names and leave operator redirects unchanged. |
| `staticRouting` | Applied bidirectionally | `network` | UCI plus stored wire-only fields | Agent-owned route sections use deterministic names. |
| `ssh` | Applied bidirectionally | `dropbear` | UCI plus stored `l3_enable` passthrough | Opt in with `list managed_domains 'dropbear'`; applies `global` and `port`, rolls UCI back after a reload failure. |
| `timeSetting` | Applied bidirectionally | `system` | UCI plus stored date/time fields | Persistent timezone and NTP settings apply when opted in; date/time remain runtime fields. |

## Runtime Services

| Key | Status | Notes |
|---|---|---|
| `terminalSetting` | Live command | RTTY channel. |
| `monitorServer` | Live command | DMP channel. |
| `transferChannel` | Live command | Transfer handshake. |
| `packageCapture` | Live command | Uses `errCode`, not `errcode`. |

## Remaining Ledger

Every key below is **Stored only** unless its inline status says otherwise. This
is intentionally explicit: stored state is not enforcement and must not be
represented as applied native configuration.

| Area | Keys and status |
|---|---|
| WAN and connectivity | `wanIpv6`, `wanIpv4Usb` (Stub), `wanLoadBalance` (Stub), `onlineDetection`, `connect`, `speedTest` (Stub), `speedTestSchedule` (Stub), `virtualWan` (Stub), `sdwan` (Stub), `lte` (Stub), `dsl` (Stub), `iptv` (Stub); `wanMac` is Applied forward only for primary WAN |
| LAN and DNS | `dnsProxy` (Stub), `dpiTraffic` (Stub) |
| Firewall and QoS | `firewallConfig`, `attackDefense`, `acl`, `aclHit`, `wirelessAcl` (Stub), `customAcl` (Stub), `urlFiltering` (Stub), `wirelessUrlFiltering` (Stub), `macFilter`, `ipMacBinding`, `sessionLimit`, `bandwidthCtrl` (Stub), `qos` (Stub), `ips` (Stub), `ipsWhiteList` (Stub), `ipsBlackList` (Stub), `signatureList` (Stub), `blockCountry` (Stub), `countryGroup` (Stub), `serviceType` |
| NAT, routing, and groups | `oneToOneNat`, `disableNat`, `natAlg`, `policyRouting` (Stub), `ipGroup`, `ipv6Group`, `ipPortGroup`, `ipv6PortGroup`, `macGroup`, `domainGroup`, `domainNoPortGroup` (ack `fqdnGroup`), `timeRange` |
| VPN | `vpn` (Stub), `vpnUser` (ack `vpnUsers`, Stub), `ipsecFailover` (Stub), `sslVpn` (Stub), `wireguard` (Stub), `radiusProfile` (Stub), `ldap` (Stub) |
| Services and system | `snmp` (Stub), `led` (Stub), `lldp` (Stub), `hwOffload` (Stub), `jumbo` (Stub), `upnp` (Stub), `mdns` (Stub), `ddns` (Stub), `mail` (Stub), `poe` (Declined without capable hardware), `port` (Stub), `speedDuplex` (Stub), `mirror` (Declined without switch support), `echoServer` (Stub), `dstConfig` (ack `dst`), `remoteLog` (ack `logSetting`, Stub), `logNotification` (Stub), `rebootSchedule` (Stub), `system` (Declined), `common`, `standaloneMgmt`, `controllerInfo`, `privacyPolicy`, `sideParams`, `abnormalDetect` (Stub), `intervalConfig`, `informSubscribe` |
| Clients | `client`, `clientOpt`, `clientIpBinding`, `clientTrafficRequire` (Stub), `clientRateConfig` (Stub), `clientOperation`, `clearSettings` (Declined until generated-section reset exists), `highAbility` (ack `backupConfig`) |
| Wireless | `wirelessBasic_2G`, `wirelessBasic_5G`, `wirelessBasic_5G2`, `wirelessAdv_2G`, `wirelessAdv_5G`, `WIRELESS_ADV_5G2` (ack `wirelessAdv_5G2`), `ssid_2G`, `ssid_5G`, `ssid_5G2`, `limits_2G`, `limits_5G`, `limits_5G2`, `loadBalance_2G`, `loadBalance_5G`, `loadBalance_5G2`, `rssi_2G`, `rssi_5G`, `rssi_5G2`, `roaming`, `roaming_cmd`, `mesh`, `mesh_cmd`, `bandSteering`, `ppskV3`, `macFilterGlobal`, `macFilterList`, `macFilterAssoc`, `schedulerGlobal`, `schedulerList`, `schedulerAssoc`, `qosConfig_2G`, `qosConfig_5G`, `qosConfig_5G2`, `rogueApScan`, `rfScan`, `channelDeploy`, `powerControl` (all Stub on non-WiFi profiles) |
| VoIP | `dnd`, `callLog`, `emergencyNumber`, `telephoneNumberIpv6Settings`, `callBlocking`, `telephoneNumber`, `voipDeviceSettings`, `telephoneBook`, `telephoneNumberAdvancedSettings`, `callForwarding`, `voiceMail`, `voiceMailSettings`, `voiceMailDownload`, `installationType` (all Stub: no FXS backend) |
| Auxiliary tools | `deviceDebugInfo` (Stub), `ping` (Live command through DMP), `traceroute` (Live command through DMP), `dnslookup` (Stub), `arptable` (live GET), `delayEffect`, `userAccount`, `controllerSetting`, `portalFreePolicyConfig` (ack `portalFreePolicy`, Stub), `facebookV2` (Stub) |

## Compatibility Checks

- `remoteLog` -> `logSetting`, `dstConfig` -> `dst`, `vpnUser` -> `vpnUsers`,
  `domainNoPortGroup` -> `fqdnGroup`, `highAbility` -> `backupConfig`, and
  `portalFreePolicyConfig` -> `portalFreePolicy` are request-to-ack aliases.
- `packageCapture` uses integer `errCode`; ordinary config acknowledgements use
  `errcode`.
- Unknown future keys remain stored and acknowledged for forward compatibility.
- Passwords, tokens, private keys, certificates, and similar fields remain
  redacted in logs and must never be emitted in transform errors.
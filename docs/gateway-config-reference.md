# Gateway Config Reference

This document is the complete inventory of configuration that a gateway-type
device exchanges with a network controller over the management channel. It
exists so that the gateway profile implementation can be completed without
any missing pieces: every key the controller can push (SET), query (GET), or
expect in device reports (INFORM) is listed with its exact wire payload,
expected response, and the target behavior chosen for this agent.

Companion documents: `README.md` (usage), `CHANGELOG.md` (history), and
[uci-config-mapping.md](uci-config-mapping.md) (per-key OpenWrt implementation
plan). Current implementation status per key is summarized in the coverage
matrix at the end.

## 1. Conventions

### 1.1 Channels and message types

| Purpose | Port | Types |
|---|---|---|
| Discovery | UDP 29810 | discovery announce (device → broadcast) |
| Management channel | TCP 29814 (TLS) | SET_REQUEST 4096 / SET_RESPONSE 8192, GET_REQUEST 24576 / GET_RESPONSE 28672, INFORM_REQUEST 256 / INFORM_RESPONSE 512, FORGET 16384/20480, UPGRADE 32768/65536, adoption handshake 0x100000–0x10000A, NOTIFY 80/144 |
| File transfer | TCP 29815 (TLS) | file-transfer handshake; results are sent on 29814 (FILE_TRANSFER_REQUEST_V2 0x160000 / RESPONSE_V2 0x170000) |
| Terminal (RTTY) | TCP 29816 (TLS) | terminalSetting-configured |
| Device monitor (DMP) | TCP 29817 (TLS) | monitorServer-configured |

Envelope: `{"header":{...},"body":{...}}`; 4-byte big-endian length prefix;
compact UTF-8 JSON. ECSP fit version for gateways is `2.2` ⇒ header version
`"2.2.0"`, capability level 3.

### 1.2 SET mechanics

- A SET body carries `sequenceId`, `configVersion`, and **one or more named
  config objects**. The first SET after the handshake is the full-config
  sync; later pushes are usually single-key.
- The response body carries top-level `errcode` (0), the echoed
  `sequenceId` and `configVersion`, and **one ack object per config key, as a
  top-level body member** (no `results` wrapper).
- Ack object shape: `{"errcode":"0"}` plus optional `errmsg` (both are
  JSON strings). The lone exception is `packageCapture`, which acks
  `{"errCode":0}` (capital C, integer).
- Ack key renames: the ack object for a pushed key is sometimes stored under
  a different name than the request key: `remoteLog` → `logSetting`,
  `dstConfig` → `dst`, `vpnUser` → `vpnUsers`,
  `domainNoPortGroup` → `fqdnGroup`, `highAbility` → `backupConfig`. Unknown
  extra ack keys are tolerated; missing acks for command keys are fine.
- `configVersion` semantics: keys marked **config** below expect the device
  to adopt the pushed `configVersion` and echo it back. Keys marked
  **command** do not advance the config version.
- An **empty response body makes the controller drop the device**. Every SET
  must be answered with at least `sequenceId`, `errcode`, `configVersion`.
- List-based configs use entry CRUD: every entry has `id` and `operation`
  (`operation` 1/2/3: add, delete, update; delete entries null all other
  fields). The pushed list is authoritative; unlisted entries are removed.
- Inclusion style is "omit when empty": empty arrays/objects and absent
  optional fields are not on the wire. Unknown keys inside objects that have
  passthrough must be echoed back unchanged.

### 1.3 GET mechanics

- GET body: `sequenceId` plus one or more typed request objects (below).
- Response body: top-level `errcode`, echoed `sequenceId`, and one answer
  object per requested key, top-level. Keys may be answered from live state
  or from the last stored push; a requested key with no answer may be
  omitted (controller tolerates it, but answers are preferred).

### 1.4 Report (INFORM) mechanics

- The device reports periodically (default 10 s; per-section intervals can
  be set in the initial sync `informInterval` / `informSubscribe`).
- Known section keys are decoded strictly (type errors can abort decoding).
  **Unknown section keys are tolerated**: they are collected into a generic
  map and ignored. This makes it safe to add sections beyond the controller
  version's expectations, but not to mistype known keys.

## 2. Negotiation and capabilities

DEVICE_NEGOTIATION body keys: `key`, `configVersion`, `deviceInfo`,
`controllerSetting{controllerId}`, `components`, `components_v2`,
`channelInfo`, `radioCap`, `devCap`, `deviceMisc{extraPortNum{extraPort,
usbLteWan}, portNum, customizeRegion}`, `monitorCapabilities{protocols,
outTypes, inTypes, compressMethods}`.

### 2.1 devCap (full surface)

The full capability dictionary the controller can consume. Keys currently
sent by the agent are marked ●; keys known but not sent are marked ○. All
keys are safe to include when the value matches the emulated model; keys the
model does not support are simply absent (absence is wire-visible).

| devCap key | Type | Meaning |
|---|---|---|
| portInfos | list | per-port capability (§2.3) |
| extraPortInfos | list | extra (USB/secondary) port capabilities |
| poePortsEntrys | list | PoE port caps |
| ipsecNum | int | IPsec policy capacity |
| supportAclDisable | int | ACL toggle (0/1) |
| supportVpnVerify | int | VPN peer verify |
| supportRoutingVpnClient | int | VPN client routed mode |
| supportDiscreteWan | int | virtual WAN (discrete WAN) support |
| supportReduceUsbRfi | int | USB LTE RFI reduction |
| supportHubCustomSubnet | int | hub custom subnet |
| supportIPsecFailover | int | IPsec failover |
| supportPfAlias | int | port-forward aliases |
| supportIgmp | int | IGMP setting |
| supportLanClientStats | int | LAN client stats |
| supportPortForwardingStatus | int | per-rule NAT counters |
| supportSessionLimitStatus | int | per-rule session counts |
| supportChannelUtilizationStatus | int | channel utilization |
| supportNetworkSearch | int | network search |
| supportBandScan | int | band scan |
| supportV6plusDslite | int | IPv6+ / DS-Lite |
| v6plusDsliteUnsupportList | list of str | firmware restrictions |
| supportDroppedPacketsStatus | int | drop counters |
| supportRetriedPacketsStatus | int | retry counters |
| supportLanIpv6 | int | LAN IPv6 |
| supportLanIpv6PassThrough | int | LAN IPv6 passthrough |
| supportAirtimeFairness | int | wireless airtime fairness |
| supportGuestNetwork | int | guest network |
| supportGIK | int | GIK |
| support11r | int | 802.11r |
| supportWirelessAdvanced | int | wireless advanced |
| supportWpaEnterprise | int | WPA enterprise |
| ipsecCap | object | IPsec capability detail |
| dhcpReservationCap | object | DHCP reservation capability |
| ctMax | long | conntrack capacity |
| arpMax | long | ARP table capacity |
| specification | object | capacity dictionary (§2.2) |
| maxTemp | int | max temperature |
| ips | object | IPS capability detail |
| netCap | object | LTE network capability |
| supportFqdn | int | FQDN address groups |
| lteIntervalLowThreshold | int | LTE report interval threshold |
| jumbo | object | jumbo frame capability |
| logNotification | object | log notification capability |
| supportPa | int | PA region |
| meshChainNum | int | mesh chain count |
| supportAIRoaming | int | AI roaming |
| supportPPSK | int / ppskNum / ppskNumV3 | PPSK capacities |
| supportRadioMode2G / 5G / 6G | int | radio modes |
| supportSyslog | int | remote syslog |
| supportDhcpRangePool | int | multiple DHCP ranges |
| protocolVer | str | protocol version string |
| supportOwe | bool | OWE |
| supportBackground | int | background |
| multiChipGateway | bool + multiChipInfos | multi-chip layout |
| supportServerOpenVpnGoogleLdap | int | OpenVPN Google/LDAP auth |
| supportCustomDhcpOpt | int | custom DHCP options |
| supportBridgeVlan | int | bridge VLAN |
| supportL2TP | int | L2TP |
| supportIPSec | int | IPsec |
| supportIpPortGroup | int | IP+port groups |
| supportDualSimCard | int | dual SIM |
| supportPingLargeThreshold | int | large-ping threshold setting |
| supportMcastMgmt | int | multicast management |
| supportVoip | bool | VoIP |
| supportWanSameVlan | bool | WAN same VLAN |
| defaultIgmpWan | int | default IGMP WAN port |
| supportAllWan | bool | all-port WAN |
| supportNatTraversal | bool | NAT traversal |
| mandatoryPorts | list | ports that must be WAN |
| supportArpDetection | int | ARP detection |
| supportSdWan | int | SD-WAN |
| wlanMacFilteringMacAddrNum | int | wireless MAC filter capacity |
| wlanMulticastFilterMacAddrNum | int | multicast filter capacity |
| supportMssClamping | bool | MSS clamping |
| supportAntennaCalibrate | int | antenna calibration |
| supportPpskWpa3 | bool | PPSK WPA3 |
| speedTestLimit | long | speed test limit |
| wpa3Ppsk | int | WPA3 PPSK |

### 2.2 devCap.specification (full surface)

Current agent sends 29 keys. Full dictionary adds: `note`, `interfaceNum`,
`vpnOpenVPNServerNum`, `vpnOpenvpnUsersNum`, `vpnMaxConUserNum`,
`vpnSubnetsNum`, `sslVpnMaxConUserNum`, `supportMaxWanNum`,
`supportMaxVlanNum`, `oneToOneNatNum`, `ldapProfileNum`, `poePortNum`,
`poePortLimit`, `dhcpRangePoolNum`, `supportCustomAclNum`,
`supportCustomAclPerNum`, `statesTimeoutMax`, `sdWanCapNum`,
`sdWanTunnelLimitNum`, `voipPhoneNumberPasswordLimit`, `dohNum`, `dotNum`.
Unknown extra keys are tolerated (passthrough), so additions are safe.

### 2.3 devCap.portInfos entry

`port`, `name`, `type` (0 WAN / 1 WAN-LAN / 2 LAN), `mac`, `mode`,
`maxBandwidth`, `physicalType`, `supportIptv`, `supportMirror`,
`supportInternetVlan`, `speedDuplexList[]`, `defaultSpeedDuplex`,
`supportPoe`, `flowControl`, `supportPortControl`,
`dslSettings{modulation,annex,bitSwap,sra}`, `supportLoopbackControl`,
`supportPortIsolation`, `supportRateLimit`, `supportStormControl`.
The agent currently sends the subset: `port, name, type, mode,
[maxBandwidth], defaultSpeedDuplex, speedDuplexList, supportInternetVlan,
supportIptv, supportMirror, supportPoe`.

### 2.4 Initial full-config sync (first SET body)

`sequenceId`, `configVersion`, `timeSetting`, `dst`, `userAccount`,
`controllerSetting`, `components`, `components_v2`, `informInterval`
(map section name → interval seconds), `informSubscribe` (map section name →
`{enable, intv}`), `monitorServer`, `monitorInterval`, `monitorComponents`
plus the feature keys of §3. Unknown keys in the sync body must still be
acked individually.

## 3. SET keys (controller → gateway)

Legend: **kind** = config (advances configVersion) / command (does not).
**Ack** = ack-key name in the response (default = same name;
`errcode` unless noted). **Gate** = when the controller pushes the key.
Target = behavior chosen for the agent (see §7).

### 3.1 WAN and connectivity

| Key | Kind | Ack | Gate | Payload fields |
|---|---|---|---|---|
| wanBasicSetting | config | wanBasicSetting | all | `wanPorts[]` (int list; empty ⇒ build omitted) |
| wanIpv4 | config | wanIpv4 (per-entry resp) | all | `settings[]{portId, proto, username, password, ipFromIsp, ipaddr, netmask, gateway, unicast, dns1, dns2, hostname, linkType, redialInterval, startTime, endTime, service, mtu, mru, vlanId, qosTag, connection2{mainProto, proto, server, ipaddr, netmask, gateway, dns1, dns2}, mssClampingType, mssClampingValue, multipleIps[]{ipaddr}, dhcp_opts[]{code, type, value}, dslite, aftrName, mape}`; proto ∈ dhcp/static/pppoe; unknown extra keys round-trip |
| wanIpv6 | config | wanIpv6 | all | `settings[]{portId, enable, proto, getIpv6, prefix, pdSize, dns, priDns, sndDns, addr, pfLen, gw, pppShare, userName, password, specificIp}` |
| wanMac | config | wanMac | all | `settings[]{portId, method, mac}` |
| wanIpv4Usb | config | wanIpv4Usb | usb-lte models | `settings[]{port, configType, reduceUsbRfi, autoConfig{location, mobileISP}, manuallyConfig{dialNumber, apn, username, password}, connectionMode, pin, authType, mtuSize, dnsEnable, dnsConfig{primary, secondary}, dhcp_opts[]}` |
| wanLoadBalance | config | wanLoadBalance | multi-WAN | `{method, mode, primaryWan[]int, backupWan, linkBackup, weight[]{portId, virtualWanId, weight}, appOptRouting, timeId}` |
| onlineDetection | config | onlineDetection | all | `{interval, unit}` |
| connect | config | connect | all | `{portId, operation, virtualWanId}` (connect/disconnect/reconnect request) |
| speedTest | command | speedTest | all | `{cmdId (long), portIds[]int, virtualWanEntryIds[]}` |
| speedTestSchedule | config | speedTestSchedule | all | `{autoSpeedTest (bool), timingType, hour, minute, dayOfWeek[]int, dayOfMonth[]int, portIds[]int, virtualWanEntryIds[]}` |
| virtualWan | config | virtualWan | discrete-WAN models | `virtualWans[]{id, operation, name, status, physicalPortId, isp, modulation, vpi, vci, encapMode, location, enableMer, merUsername, merPassword, ipv4{proto, vlanId, qosTag, static{ipaddr, netmask, gateway, mtu, dns1, dns2}, dhcp{unicast, dns1, dns2, hostname, mtu, dhcpOptions[]}, pppoe{…}, pppoa{…}, ipoa{ipaddr, netmask, gateway, dns1, dns2, mtu, multipleIps[]}}, mac{method, mac}, balanceWeight}` — note `dhcpOptions` here vs `dhcp_opts` in wanIpv4 |
| sdwan | config | sdwan | sdwan models | `{role, wanPortIds[]int, sdwanIp, hubWanIp, netId[]int, custom_network[]str (snake_case), operation, sdWanKey, linkedSpokes[]{sdwanIp1, sdwanIp2}, natNetworks[]{netId, natNetwork}, natRoutes[]{route, natRoute}}` |
| lte | config | lte | LTE models | `{selectedApns[]{port, apns[], cleanDefaultProfiles, supportSMS}, lteSettings[]{port, mobileData, dataRoaming, nat, netMode, bandMode, bands[], 5Gbands[] (digit-leading key), netSearchMode, isp, ispNum, apnMode, apn (int profile id), simPriority, timeout, + SIM2 fields with suffix 1, simCardUsed}, apns[]{id, type, name, operation, pdpType, apnType, apn, username, password, authType, applyToSim}, quota/quota1{dataSetting{enable, type, startDate, limit, credit (float), alert, usage, phone}, smsSetting{…credit int…}, correct{data (float), sms}}, sendMessage/1{type, receiver, content, test}, operateMessage/1{operation, type, ids[]}, smsPolicy{inboxPolicy, mailServer}, routerCommand{rebootEnable, reboot{}, statusEnable, status{}, accessEnable, access{}}, pin{lock, autoUnlock, autoUnlock1, pin, newPin, simCard}, puk{puk, pin, simCard}, ispFile{content}, bandScan{cmdId (long), portId, simCard, mode}, ispScan{cmdId, portId, simCard}}` |
| dsl | config | dsl | DSL models | `{settings[]{port, location, isp, modulation, vpi, vci, encapMode, dslSettings{modulation, annex, bitSwap, sra}, enableMer, merUsername, merPassword}}` |
| iptv | config | iptv | all | `{igmpEnable (bool), igmpVersion (str), wanPortId (str!), virtualWanId, mldEnable (bool), mldVersion (str), mldWanPortId (str), iptvSetting{enable (bool), mode, wanPortId, customConfig{ipPhoneVId, ipPhoneVPri, iptvVId, iptvVPri}, portConfig[]{portId (str), type (str)}, dsl{modulation, ipPhoneVpi, ipPhoneVci, iptvVpi, iptvVci}}}` |

### 3.2 LAN, DHCP, DNS

| Key | Kind | Ack | Payload fields |
|---|---|---|---|
| network | config | network | `{networks[]{id, name, default, operation, isolation, purpose, interface[]int (reserved word), vlan, vlanType, vlans, gateway, netmask, dhcpServer{enable, ipaddrStart, ipaddrEnd, ipRangePool[]{start, end}, dns, priDns, sndDns, leasetime, gateway, hostIP, option60, option66, option138, dhcpNextServer, dhcp_opts[]{code, type, value}}, domain, igmpSnoop, dhcpGuard{enable, dhcpSvr1, dhcpSvr2}, portal, authType, httpsRedirect, fbv2, ipv6{proto, enable, gw, subnet, ipStart, ipEnd, leasetime, dns, priDns, sndDns, prefix, preType, portId, preId, ra{enable, preference, validLifetime, preferredLifetime}}, arpDetectionEnable}, isolations{networks[]int, isolation}}` |
| lanDns | config | lanDns | `{dnsList[]{id, operation, enable, index, name, domain, aliases[]str, type, ipAddrs[]str, ipv6Addrs[]str, cname, dnsServers[]str, lanNetworks[]int, ttl}}` |
| dnsProxy | config | dnsProxy | `{type, dnsSecConfig{servers[]str, replyPolicy}, dohConfig{servers[]{name, servers[]str}}, dotConfig{servers[]{name, servers[]str}}, dnsOverrideConfig{primaryDnsServer, secondaryDnsServer, applyNetwork[]}}` |
| dnsCache | config | dnsCache | `{operation, config{enable, ttl}}` |
| dnsCache (GET) | — | — | see §4 |
| dpiTraffic | command | dpiTraffic | `{token, port, interval}` (DPI traffic-report target) |

### 3.3 Firewall, security, QoS

| Key | Kind | Ack | Payload fields |
|---|---|---|---|
| firewallConfig | config | firewallConfig | `{icmp, other, tcpClose, tcpCloseWait, tcpEstablished, tcpFinWait, tcpLastAck, tcpSynRecv, tcpSynSent, tcpTimeWait, udpOther, udpStream, broadcastPing, receiveRedirects, sendRedirects, synCookies}` |
| attackDefense | config | attackDefense | `{tcpConnEn, tcpConnLimit, udpConnEn, udpConnLimit, icmpConnEn, icmpConnLimit, tcpSrcEn, tcpSrcLimit, udpSrcEn, udpSrcLimit, icmpSrcEn, icmpSrcLimit, ipFrag, tcpNoflag, tcpScanReject, pingDeath, pingLarge, pingLargeThreshold, pingWan, tcpWinnuke, tcpFinSyn, tcpFinNoack, ipOption, ipoptSecure, ipoptLooseRoute, ipoptStrictRoute, ipoptRecordRoute, ipoptStream, ipoptTimestamp, ipoptNoop, icmpTimestampRequestReject}` |
| acl | config | acl (per-entry resp) | `{acls[]{id, operation, index, status, policy, protocols[]int, sourceType, source[], destinationType, destination[], ipSec, direction{all, lanToWan, lanToLan, wanInIds[]int, vpnInIds[]int}, states{stateNew, established, related, invalid}, timeRangeId, syslogEnable (bool)}}` |
| aclHit | config | aclHit | `{clear (bool)}` |
| wirelessAcl | config | wirelessAcl | same shape as `acl` |
| customAcl | config | customAcl | `{acls[]{id, operation, position (index), status, policy, service[]int, directions[]int, source[]str, sourcePort, destination[]str, destinationPort, description, syslogEnable}}` |
| urlFiltering | config | urlFiltering | `{globalConfig{blockPage (bool), blockPageMessage, safeSearch (bool)}, rules[]{id, operation, index, status, policy, sourceType, sources[]int, mode, urls[]str, keywords, filterMode, scenarioMode, categories (map str → list str), timeRangeId}}` |
| wirelessUrlFiltering | config | wirelessUrlFiltering | same shape as `urlFiltering` |
| macFilter | config | macFilter | `{enable, reset, filterMode, direction{…}, macFilters[]{id, operation, type, macAddresses[]str, macGroupIds[]int}}` |
| ipMacBinding | config | ipMacBinding | `{enable (bool), lanIds[]int, wanIds[]int, imbPass (bool), garp (bool), interval, ipMacBindings[]{id, operation, mac, ip, status (bool), wanId, lanId}}` (no Jackson annotations — bean names) |
| sessionLimit | config | sessionLimit (per-entry resp) | `{global{enable}, rules[]{id, operation, enable, sourceType, sources[]int, maxSession, index, ip}, ipRules[]{…same…}}` |
| bandwidthCtrl | config | bandwidthCtrl (per-entry resp) | `{global{enable, threshold, thresholdLimit, bandwidthPortSettings[]{portId, up, down}}, rules[]{id, operation, index, enable, sourceType, sources[]int, wanPortIds[]int, up, down, mode}}` |
| qos | config | qos (per-entry resp) | `{bwcRules[]{port, operation, status, direction, inbound, outbound, udpBandwidthCtrl, udpLimitedRatio, outboundAckPrior, classRatio[]int}, classRules[]{id, operation, enable, ipVersion, localIpId, remoteIpId, dscp, serviceTypeId, class}, voipPriority{enable, sipPort}, tagPriorities[]{class, enable, dscp}}` |
| ips | config | ips | `{enable (bool), mode, geoEnable (bool), categoryIds[]int, timeRangeId}` |
| ipsWhiteList / ipsBlackList | config | ipsWhiteList / ipsBlackList (per-entry resp) | `{entities[]{id (str), operation, + whitelist: trafficType, direction, trafficSource / blacklist: blockMode, source, name, destination}}` |
| signatureList | config | signatureList (per-entry resp) | `{entities[]{id (str), operation, trafficScope, trafficType, direction, trafficSource, sid (long)}}` |
| blockCountry | config | blockCountry (per-entry resp) | `{entities[]{id (str), operation, country}}` |
| countryGroup | config | countryGroup | `{countryGroups[]{id, operation, countries[]str}}` (name is not wire-visible) |
| serviceType | config | serviceType (per-entry resp) | `{serviceTypes[]{id, operation, protocol, sourcePorts (str), destinationPorts (str), type, code, protocolNum}}` |

### 3.4 NAT and routing

| Key | Kind | Ack | Payload fields |
|---|---|---|---|
| natPf (port forwarding) | config | natPf (per-entry resp) | `{settings[]{id, operation, enable (str!), from, fromAddr[]str, dmz, interface[]int, virtualWan[]int, interfaceIps (map str → list of str), ipaddr, externalPort (str), internalPort (str), protocol}}` |
| oneToOneNat | config | oneToOneNat (per-entry resp) | `{oneToOneNats[]{id, operation, status, interfaceIds[]int, originalIp, translatedIp, dmz}}` |
| disableNat | config | disableNat (per-entry resp) | `{disableNats[]{id, operation, status (str!), interface, lan_network[]int, comment}}` |
| natAlg | config | natAlg | `{ftp, h323, pptp, sip, ipsec, ftpPorts[]int, sipTcp, sipUdp, sipPorts[]int, sipDirectSignaling, sipDirectMedia, sipTimeout, sipSignalingTimeout, sipMediaTimeout}` |
| staticRouting | config | staticRouting (per-entry resp) | `{staticRoutings[]{id, operation, name, status, destinations[]str (CIDR), routeType, nextHopIp, interfaceType, interface, metric, virtualWanId}}` |
| policyRouting | config | policyRouting (per-entry resp) | `{policyRoutings[]{id, operation, index, status, protocols[]int, interfaceType, interface, interfaceIds[]int, vpnIds[]int, virtualWanIds[]int, backupInterface, sourceType, source[]int, destinationType, destination[]int}}` |

### 3.5 Groups and time ranges

| Key | Kind | Ack | Payload fields |
|---|---|---|---|
| ipGroup | config | ipGroup (per-entry resp) | `{ipGroups[]{id, operation, ipSubnets[]str}}` |
| ipv6Group | config | ipv6Group | `{ipv6Groups[]{id, operation, ipSubnets[]str}}` |
| ipPortGroup | config | ipPortGroup | `{ipPortGroups[]{id, operation, ipSubnets[]str, portRanges (list of [start, end] int pairs)}}` |
| ipv6PortGroup | config | ipv6PortGroup | `{ipv6PortGroups[]{id, operation, ipv6Subnets[]str, portType, portRanges, portMasks}}` |
| macGroup | config | macGroup | `{macGroups[]{id, operation, macs[]str}}` |
| domainGroup | config | domainGroup | `{groupEntry[]{id, operation, domainNames[]str, domainNamesEntry[]{domainName, domainPorts (str)}}}` |
| domainNoPortGroup | config | fqdnGroup (renamed!) | `{domainEntry[]{id, operation, domainNames[]str}}` |
| timeRange | config | timeRange (per-entry resp) | `{timeRanges[]{id, operation, rules[]{type, day, startH, startM, endH, endM}}}` |

### 3.6 VPN

| Key | Kind | Ack | Payload fields |
|---|---|---|---|
| vpn | config | vpn (per-entry resp) | `{autoIPSecs[]{id, operation, enable (bool), name, ips[]str}, manualIPSecs[]{…}, server_IPSecs[]{id, operation, enable (bool), name, remote_peer, remote_network[]str, local_binding[]int, local_network[]int, net_id[]int, customNetwork[]str, ipPoolType, ipPoolStart, ipPoolEnd, ippool, dnsStatus (bool), dns1, dns2, psk, ike_version, ike_proposal_1..3, exchange_mode, connection_type, local_id_type, local_id_value, remote_id_type, remote_id_value, ikelifetime, dpd_enable (bool), dpd_interval, encapsulation_mode, ph2_proposal_1..3, pfs, lifetime, nat (bool)}, server_L2TPs[]{…ipsecenc, presharekey, users[] (deprecated), authType, ldapProfile…}, server_PPTPs[]{…mppeenc…}, server_OpenVPNs[]{…service_type, service_port, accAuth, tunnelMode, serverIpEnable (bool), serverIp…}, client_L2TPs[]{workmode, username, password, ipsecenc, remote_server, remote_network[]str, local_network[]int, net_id[]int, customNetwork[]str, psk, local_binding}, client_PPTPs[]{…mppeenc…}, client_OpenVPNs[]{service_ip, service_port, cert, cfg, username, password…}, tunnelTerminate{vpnId, userId, terminate, remoteIp}, server_Wireguards[]{mtu, listenPort, privateKey, localIp, ipPool*, ippool, serverIp, dnsStatus, dns1, dns2, local_binding[], local_network[], net_id[], customNetwork[], peers[]{interfaceIp, allowAddresses[], publicKey, presharedKey, keepalive}}, client_Wireguards[]{mtu, listenPort, privateKey, localIp, dns1, dns2, local_network[], customNetwork[], net_id[], peers[]{endPoint, endPointPort, publicKey, allowAddresses[], presharedKey, keepalive}}}` (snake_case keys `remote_peer`, `local_binding`, `net_id`, `remote_network`, `local_network`, `custom_network`-style inside VPN entries) |
| vpnUser | config | vpnUsers (renamed!) | `{users[]{id, operation, name, password, protocol, mode, maxsessions, remotesubnet[]str, servers[]int, localIp}}` |
| ipsecFailover | config | ipsecFailover | `{groups[]{id, name, operation, primary, candidates[]int, autoFailback, failbackTime}}` |
| sslVpn | config | sslVpn | `{sslVpnServer{status (bool), wanPort[]int, ipPoolStart, ipPoolEnd, dnsStatus (bool), dns1, dns2, serverIpEnable (bool), serverIp, servicePort, authType, radiusSetting{profileId, name, authServers[]{ip, port, password}, accounting (bool), interimUpdate (bool), iuInterval, accServers[], authType, repeatTime, overTime, nasIp, defaultGroup, proxyId, proxyPort, updateRadiusServerIp (bool, bean name)}, ldapSetting{profileId, defaultGroup}, nameLockSetting{status (bool), times, duration}, ipLockSetting{…}, exitAtIdle (bool), exitTime, totalTraffic (bool)}, resources[]{id, name, operation, type, ip, mask, domain, service, srcPortStart, srcPortEnd, dstPortStart, dstPortEnd, icmpType, icmpCode, otherProtocol}, resourceGroups[]{id, name, operation, resourceIds[]int}, users[]{id, name, operation, status (bool), password, validity (str), concurrentNum, groupId}, userGroups[]{id, name, radiusAttribute, ldapAttribute, operation, resourceGroupIds[]int}, locks[]{username, oldUsername, operation, type, oldType, ip, oldIp, totalLockTime, leftLockTime}, terminate{vIp}}` |
| wireguard | config | wireguard | `{interfaces[]{id, operation, enable (bool), mtu, listenPort, privateKey, localIp, local_network[]int, customNetwork[]str, net_id[]int}, peers[]{id, operation, enable (bool), interface (int), publicKey, endPoint, endPointPort, allowAddresses[]str, presharedKey, keepalive}}` |
| radiusProfile | config | radiusProfile | `{profiles[]{profileId (str), requireMessageAuthenticator, authServers[]{ip, port, password}, accounting (bool), interimUpdate (bool), iuInterval, accServers[]{ip, port, password}, operation}}` |
| ldap | config | ldap | `{profiles[]{name, status, type, host, port (str!), ssl, dn, password, cn, base, filter, group, operation, serverType, cert, key}}` |

### 3.7 Services and system

| Key | Kind | Ack | Payload fields |
|---|---|---|---|
| snmp | config | snmp | `{location, contact, v1v2cEnable, community, v3Enable, v3Username, v3Password}` |
| ssh | config | ssh | `{global, port, l3_enable}` (snake_case key) |
| led | config | led | `{enable, locate}` |
| lldp | config | lldp | `{enable}` |
| hwOffload | config | hwOffload | `{enable}` |
| jumbo | command | jumbo | `{size}` |
| upnp | config | upnp | `{global, interface[]int, network[]int}` |
| mdns | config | mdns | `{enable, network[]int, customServices[]{name, domainName[]str}, rules[]{name, status, client{…}, service{…}, defaultServices[]int, customServices[]int}}` |
| ddns | config | ddns (per-entry resp) | `{rules[]{id, operation, service, status, interface, username, password, domain, interval, intervalUnit, updateUrl}}` |
| mail | config | mail | `{servers[]{id, operation, sender, receiver, ssl, smtpServer, smtpPort, auth, username, authCode}}` |
| poe | config | poe | `{ports[]{port, enable, restart}}` |
| port | config | port (per-entry resp) | `{ports[]{portList (set of int), port, pvid, flowControl, status, loopback{enable, mod}, portIsolation, bandCtrl{egressLimit, ingressLimit}, stormCtrl{unknownUnicast, multicast, broadcast, action, recoverTime}, dslSettings{modulation, annex, bitSwap, sra}}}` |
| speedDuplex | config | speedDuplex | `{speedDuplexes[]{port, linkSpeed, duplex}}` |
| mirror | config | mirror | `{mirrors[]{port, enable, mode, mirroredPorts (set of int)}}` |
| echoServer | config | echoServer | `{ip}` |
| timeSetting | config | timeSetting | `{timeZone, date, time, ntpServer1, ntpServer2, ntpEnable, ntpServers}` |
| dstConfig | config | dst | `{enable, start, end, offset}` |
| remoteLog | config | logSetting (renamed!) | `{mailEnable, mailFrom, mailTo, smtpServerIp, mailAuth, mailUsername, mailPassword, timeMode, fixationTimeHour, fixationTimeMin, periodTimeHour, everyDayTime, logClientDetailAMF, logServerEnable, logServerIp, logServerPort, logClientDetailLS, nvramEnable}` |
| logNotification | config | logNotification | `{enableEventKey, enableAlertKey}` |
| rebootSchedule | config | rebootSchedule | `{rebootSchedules[]{id, operation, status, timingType, dayOfWeek, dayOfMonth, hour, minute}}` |
| system | command | system | `{action, param{rebootDelay}}` |
| common | config | common | `{name}` (device name) |
| standaloneMgmt | config | standaloneMgmt | `{webHttp, webHttps, appDisc}` |
| controllerInfo | config | controllerInfo | `{ip, discoverPort, destOmadacId, managePort, rollback}` |
| privacyPolicy | config | privacyPolicy | (empty object tolerated) |
| sideParams | command | sideParams | `{enable}` |
| abnormalDetect | command | abnormalDetect | `{enable}` |
| intervalConfig | command | intervalConfig | map section → interval (see §2.4) |
| informSubscribe | command | informSubscribe | `{<section>{enable, intv}, …, mesh.status, mesh.isolatedAPs, mesh.childAPs, mesh.candidateParents, roaming.probeClients, roaming.neighborList (dotted keys)}` |

### 3.8 Clients

| Key | Kind | Ack | Payload fields |
|---|---|---|---|
| client | command | client | `{clients[]{mac}}` (block/unblock list request) |
| clientOpt | command | clientOpt | operation objects (block/allow client operations) |
| clientIpBinding | config | clientIpBinding | `{clientIpBindings[]{mac, status, ip, vid, netId, options[]{code, type, value}}}` |
| clientTrafficRequire | command | clientTrafficRequire | `{clients (set of str)}` |
| clientRateConfig | config | clientRateConfig | `{action, clientRateLimit}` |
| clientOperation | config | clientOperation | `{opts[]{mac, opt, vid}}` |
| clearSettings | command | clearSettings | integer (reset request) |
| highAbility | command | backupConfig (renamed!) | `{mod, destIp, ips[]}` |

### 3.9 Wireless (WiFi models only; e.g. the WiFi gateway model)

| Key | Kind | Ack | Payload fields |
|---|---|---|---|
| wirelessBasic_2G / wirelessBasic_5G / wirelessBasic_5G2 | config | wirelessBasic_2G / wirelessBasic_5G / wirelessBasic_5G2 | `{radioId, radioEnable (bool), channelRange[]int, chanWidth, channel, txPower, channelLimit (bool), freq, wirelessMode, aiDeploy (bool), roamingOpt (bool), autoSwitchOffWifi (bool), autoSwitchOffWifiInterval, rrmType, interTimeStamp (long)}` |
| wirelessAdv_2G / wirelessAdv_5G / WIRELESS_ADV_5G2 (uppercase!) | config | wirelessAdv_2G / wirelessAdv_5G / wirelessAdv_5G2 | `{radioId, beaconInterval, beaconIntvMode, dtimPeriod, rtsThreshold, fragThreshold, airtimeFairness (bool), maxProbeRespTimes, probeRespMode, probeRespThres, rate_54..rate_1, bRate_54..bRate_1, mcsIndex, ofdma}` |
| ssid_2G / ssid_5G / ssid_5G2 | config | ssid_2G / ssid_5G / ssid_5G2 | `{radioId, ssid[]{id, index (long), operation, ssidName, oldSsidName, vlanId, vlanPoolIds[]str, ssidBcast (bool), securityMode, portal (bool), authType, fbv2{portalUrl}, ssidIsolation (bool), wpaVer, wpaCipher, wpaServer, wpaPort, wpaKey, wpaKeyUpdate, pskVer, pskCipher, pskKey, pskKeyUpdate, override (bool), httpsRedirectEnable (bool), wpaRadiusProfileId, radiusAuth{}, radiusAccounting{}, macAuth{}, rateCtl, ppsk{}, dyVlanMode, fastTransition{}, radiusCoa{}, pmfMode, multiCast{}, hotspotV2{}, greEn (bool), mlo, dhcpOp82{}, wpaEntNasid{}, prohibitWifiShare (bool), manageRateCtl, wanAccess (bool), loadBalance, bandSteerMode}}` |
| limits_2G / limits_5G / limits_5G2 | config | limits_2G / limits_5G / limits_5G2 | per-SSID rate-limit list |
| loadBalance_2G / loadBalance_5G / loadBalance_5G2 | config | loadBalance_2G / loadBalance_5G / loadBalance_5G2 | per-radio load balance |
| rssi_2G / rssi_5G / rssi_5G2 | config | rssi_2G / rssi_5G / rssi_5G2 | `{radioId, rssi thresholds}` |
| roaming | config | roaming | issued roaming info |
| roaming_cmd | command | roaming | roaming command variant |
| mesh / mesh_cmd | config / command | mesh | mesh info (candidate parents, isolated/child APs) |
| bandSteering | config | bandSteering | `{enable}` |
| ppskV3 | config | ppskV3 | `{ppsks[]}` |
| macFilterGlobal / macFilterList / macFilterAssoc | config | same | wireless MAC filtering |
| schedulerGlobal / schedulerList / schedulerAssoc | config | same | wireless schedulers |
| qosConfig_2G / qosConfig_5G / qosConfig_5G2 | config | qosConfig_2G / qosConfig_5G / qosConfig_5G2 | per-radio QoS |
| rogueApScan | command | rogueApScan | rogue AP scan trigger |
| rfScan | command | rfScan | RF scan trigger |
| channelDeploy | command | channelDeploy | channel deployment |
| powerControl | command | powerControl | power control |

### 3.10 VoIP (VoIP-capable models)

| Key | Kind | Ack | Payload fields |
|---|---|---|---|
| dnd | config | dnd | `{enable, mode, timeBegin, timeEnd}` |
| callLog | config | callLog | call-log enable |
| emergencyNumber | config | emergencyNumber | emergency numbers |
| telephoneNumberIpv6Settings | config | telephoneNumberIpv6Settings | `{viaIpv6}` |
| callBlocking | config | callBlocking | call blocking rules |
| telephoneNumber | config | telephoneNumber | per-port telephone numbers |
| voipDeviceSettings | config | voipDeviceSettings | `{portSettings[]{port, numberForOutgoingCalls, numbersForIncomingCalls, vadSupportEnable (bool), speakerGain, micGain}}` |
| telephoneBook | config | telephoneBook | telephone book entries |
| telephoneNumberAdvancedSettings | config | telephoneNumberAdvancedSettings | `{localeSelection, boundInterface{type, id, virtualWanId}, noAnswerTime, t38Support}` |
| callForwarding | config | callForwarding | `{ruleList[]{operation, ruleId, enable, condition, type, toNumbers[]str, toFxsPorts[]str, fromPersonNumbers, fromContactBook[]str, forwardVia, forwardToNumber}}` |
| voiceMail | command | voiceMail | `{operation, all (bool), voiceMailList[]{id}}` |
| voiceMailSettings | config | voiceMailSettings | `{enable, noAnswerTime, remoteAccessEnable, remoteAccessPin, voiceMailInUsb, greetingForVoiceMailMode, greetingName, duration, uuid, capacity}` |
| voiceMailDownload | command | voiceMailDownload | `{operation (str), fileName, nid}` |
| installationType | config | installationType | channel limit type |

### 3.11 Auxiliary channels and tools

| Key | Kind | Ack | Payload fields | Target |
|---|---|---|---|---|
| terminalSetting | command | terminalSetting | `{enable, token, port, heartbeatFrequency, SSL}` (uppercase `SSL` key) | start/stop RTTY client on 29816 |
| monitorServer | config | monitorServer | `{token, port, path, protocol, domain, aesKey, iv, compress, content}` | start/stop DMP client on 29817 |
| transferChannel | command | transferChannel | `{port, token, aesKey, iv}` | connect 29815 + handshake **before** sending the SET_RESPONSE; ack `{transferChannel:{errCode:0}}` |
| packageCapture | command | packageCapture | `{operation ("start"/"stop"), nid, captureInfo{duration, totalSize, packageSize, interface (str), vlanId, channel, filterRules, srcMac, destMac, srcPort, destPort, srcIp, destIp, protocol (str)}}` | run capture; NOTIFY file; serve 29815 transfer; ack `{packageCapture:{errCode:0}}` |
| deviceDebugInfo | command | deviceDebugInfo | `{nid, operation}` | debug bundle request |
| ping / traceroute / dnslookup | command | ping / traceroute / dnslookup | `{cmdId, dest, interface, network, additional}` | Network Check probes (answered on the DMP channel, not SET) |
| arptable | command | arptable | `{cmdId, dest, interface, network, additional}` | see §4 (GET) |
| delayEffect | command | delayEffect | `{delay, effect}` | delay config application |
| system | command | system | `{action, param{rebootDelay}}` | reboot/reboot-delay requests (declined: reply errcode 0 without action) |
| userAccount | config | userAccount | `{curUsername, curPassword, newUsername, newPassword}` | device account update (also arrives in the handshake) |
| controllerSetting | config | controllerSetting | controller identity block | echo/ack |
| portalFreePolicyConfig | config | portalFreePolicy | `{portalFreePolicy, urlPortalFreePolicy}` |
| facebookV2 | config | facebookV2 | `{enable, hostRedirectIps[]str, httpsCert, httpsPriKey}` |

## 4. GET keys (controller → device queries)

| Key | Request body | Response body |
|---|---|---|
| wanIpv4 | (may be empty/null) | last applied `wanIpv4` config (same shape as §3.1) |
| vpn | `{vpnId}` | `{vpnId, status, cert}` |
| sslVpn | `{ip}` | `{status, cert}` |
| sessionLimit | string marker | `{sessions[]{mac, ip, source, type, currentSession, limitNum, percentage}}` |
| dnsCache | request object | `{dnsCacheInfos[]{domain, ipList[]str, ipVersion, ttl}}` |
| lte | request object | LTE status (SIM/PIN/messages) |
| dpiProtocols | request object | DPI protocol list |
| urlFiltering | `{protocolVer}` | `{protocolVer, categories}` |
| telephoneNumber | request object | per-port number status |
| ddnsStats | request object | `{ddnss[]}` (per-rule DDNS status) |
| voiceMailSettings | request object | USB general info |
| voiceMail | request object | `{voiceMail[]}` |
| radioStatus | request object | `{radios[]}` (WiFi models) |
| dhcpClient | request object | `{clients[]{name, ip, mac, leaseTime}}` |
| aclHit | request object | `{hits[]{id, hitCount}}` |
| arptable | request object | `{arps[]{mac, ip, port, vlan}}` |

The response body also carries top-level `errcode` and the echoed
`sequenceId`. Unrequested-but-known keys must not be included.

## 5. INFORM sections (device → controller reports)

Typed sections (decoded strictly; unknown sections are ignored):

| Section | Key | Fields |
|---|---|---|
| deviceInfo | deviceInfo | `model, modelVer, fwVer, hwVer, sm, cerVer, time, ip, ipv6List, fac, cu, mu, temp, fan, rps, txRate, rxRate` |
| portInfo | portInfo | per entry: `port, physicalType, name, mode, mac, status, internetState, internetV6, ip, netmask, ip2, netmask2, speed, duplex, publicWanIp, ip4{gw, gw2, priDns, sndDns, priDns2, sndDns2}, ip6{addr, gw, priDns, sndDns, prefix}, latency`; `internetState`+`internetV6` required on every entry |
| trafficStat | trafficStat | `{trafficStats[]{port, physicalType, rx, tx, rxP, txP, rxR, txR, rxErrPkt, txErrPkt, errPkt, lossPkt}}` |
| networkTraffic | networkTraffic | `{networkTraffics[]{ip, ip6, rx, tx, vlan, dhcpsUtil, dhcps6Util, dhcpsOffer, dhcps6Offer}}` |
| client | client | `{clients[]{mac, name, ip, vid, time, rx, rxP, tx, txP, txT, firstSeen, authed, port}}` |
| dhcpClient | dhcpClient | `{clients[]{name, ip, mac, leaseTime}}` |
| arp | arp | `{arps[]{mac, ip, port, vlan}}` |
| routingTable | routingTable | `{routingTables[]{id, destIp[]str, nextHop, interfaceName, metric}}` |
| log | log | system log entries |
| clientTraffic | clientTraffic | `{traffic[]{mac, tx, rx, txP, rxP}}` |
| portforward | portforward | `{users[]{id, name, proto, infa[]int, export, inip, inport, bts, pkts, dura}, upnps[]{…}}` |
| ddns | ddns | `{ddnss[]{id, domain[]str, interface, ip, status, statusMsg, lastUpdated}}` |
| vpn | vpn | `{ipSecs[]{id, direct, protocol, spi, localTun, peerTun, localSa, remoteSa, espEncry, espAuth, ahAuth}, openvpn[]{id, userId, userName, localIp, remoteIp, infa, dns, up, down, upP, downP, uptime}, tuns[]{id, user, userId, authType, mode, localIp, remoteIp, infa, dns, up, down, upP, downP, uptime, loginTime}, wireguard}` |
| sslVpn | sslVpn | `{connections[]{id, user, vIp, lIp, up, down, authType, time}, locks[]{user, ip, type, rTime, tTime}}` |
| wireguard | wireguard | `{connections[]{id, ip, port, up, upp, down, downp, hshake, status}, interfaces[]{id, activePeers, totalPeers}}` |
| portalDuration | portalDuration | `{portalDurations[]{client, start, dura}}` |
| ctTable | ctTable | `{ctMax, ctNum}` |
| abnormalDt | abnormalDt | `{access[]{eventId, reason, usr, psw, ip, mac}, dev[]{devTemp}}` |
| poe | poe | `{limit, remain, percent, fan, ports[]{port, state, p, u, i}}` |
| qos | qos | `{data[]{port, throughputs[]{class, inbound, outbound}, voip{inbound, outbound}}}` |
| lldp | lldp | `{lldps[]{port, standardPort, neighbors[]{chassisId, portId, …}}}` |
| ipsThreat | ipsThreat | `{data[]{time, severity, threatDescription, categoryId, classDescription, dataUsage, srcIp, dstIp, srcCountry, dstCountry, protocol, sid, classification}}` |
| lte | lte | `{selectedApns[]{port, apns[], cleanDefaultProfiles, supportSMS}, selectedApns1{…}}` |
| dhcpClient | dhcpClient | `{clients[]{name, ip, mac, leaseTime}}` |
| applicationsTraffic | applicationsTraffic | `{traffic[], block[]}` |
| sdwan | sdwan | `{tuns[]{remoteTun}}` |
| lastCfgResult | lastCfgResult | last SET response per-feature acks |
| cfgResults | cfgResults | `{setResults[]}` rolling history (cap ≈ 10) |
| callLogInform | callLogInform | VoIP call log entries |
| voiceMailUsb | voiceMailUsb | voicemail USB state |
| needReply | needReply | integer marker |

Carried as unknown/extra sections (tolerated, ignored by decode):
`eventInform[]{eid, timestamp, data}`, `virtualWanInfo{virtualWans[…]}`,
`monitor{link}`, `ssidStats_2G/5G/5G2`, `radioTraffic_2G/5G/5G2`,
`wSettings_2G/5G/5G2`, `mesh{…}`, `roaming{…}`, `clients_wireless`,
`portalAuthClients`, `rogueApList`, `clientConnection`, `aclHit[]`.

## 6. Actions and auxiliary flows

- **Reboot**: arrives as `system` SET (`{action, param{rebootDelay}}`) or as
  the FORGET message; no separate gateway reboot SET key exists. Upgrade
  requests (UPGRADE 32768) are declined by the agent with
  `{sequenceId, result:-1, msg}`.
- **Packet capture** (full flow): `packageCapture` SET (`operation "start"`)
  → ack `{packageCapture:{errCode:0}}` → device captures for `duration`
  → NOTIFY (type 80, subject 6, `ctnt{errCode, cmdId, type:1,
  fileInfos[]{fileName, filePath, fileSize, md5}}`, header needs `dest` +
  `timestamp`) → controller pushes `transferChannel` SET → device connects
  to 29815, pre-connects with the token, expects `{errCode:0}` → controller
  sends FILE_TRANSFER_REQUEST_V2 byte-range requests on 29814 → device
  answers FILE_TRANSFER_RESPONSE_V2 (512 KiB partitions, base64 data,
  `fileType` "pcap", `compression` "none") on 29814.
- **Terminal**: `terminalSetting` SET → RTTY client to 29816; registration
  payload splits into exactly 4 NUL segments; heartbeat payload is a
  non-empty uint32 uptime.
- **Network Check**: `monitorServer` SET → DMP client to 29817 (TLS if
  `protocol` is tls); serves ping/traceroute/DNS probes keyed by `cmdId`.
- **Forget**: FORGET_REQUEST → `{result:0}`; clears adoption, stored config,
  and stops auxiliary services.
- **Speed test**: `speedTest` SET carries `cmdId`; results are reported via
  the monitoring channel, not SET.

## 7. Target behavior per SET key

Classification used below: **APPLY** = translate to OpenWrt config (uci +
reload); **ECHO** = store and return verbatim (state only); **LIVE** = not
stored; computed on GET; **STUB** = accept and ack, well-formed empty state;
**DECLINE** = ack with non-zero errcode.

| Key group | Keys | Target | OpenWrt mapping / notes |
|---|---|---|---|
| WAN IPv4 | wanIpv4 | APPLY | `network.interface.wan` (proto dhcp/static/pppoe, dns, mtu, vlan `network.@switch_*`/8021q), plus conntrack-relevant options; changes trigger `network` reload; ip4 fields then feed INFORM `portInfo.ip4` |
| WAN basic | wanBasicSetting | APPLY | port role mapping (`portmap` → `network` device ports) |
| WAN MAC | wanMac | APPLY | `network.@device` macaddr per WAN port |
| WAN IPv6 | wanIpv6 | APPLY | `network.interface.wan6` (dhcpv6/pd/static), ra/dhcpv6 flags |
| WAN USB | wanIpv4Usb | DECLINE (no modem) unless usb-lte profile | qmi/mbim proto otherwise |
| Load balance | wanLoadBalance | APPLY | multipath-policy routing via `network` + `mwan3`-style policy (or static weights) |
| Online detection | onlineDetection | ECHO | controller-side polling hints only |
| Connect | connect | LIVE | force WAN dial/renew on the port (ubus `network.interface.wan` down/up) |
| Speed test / schedule | speedTest, speedTestSchedule | LIVE/STUB | cmdId-ack; run/measure in monitoring channel |
| Virtual WAN / SD-WAN | virtualWan, sdwan | STUB (models without) / APPLY (if profile gate) | needs discrete-WAN capability flag |
| LTE / DSL | lte, dsl | STUB (hardware-specific) | only on LTE/DSL profiles |
| IPTV | iptv | APPLY | bridge/port VLANs for IPTV (swconfig/DSA) |
| LAN networks | network | APPLY | `network` sections (bridge, addresses, DHCP `dhcp.@dhcp[n]`, DHCP options, ra/dhcpv6 in `odhcpd`), incl. `interface[]` key mapping |
| LAN DNS | lanDns | APPLY | dnsmasq server=/domain/… + host records |
| DNS proxy/cache | dnsProxy, dnsCache | APPLY/ECHO | dnsmasq (DoH/DoT not native: STUB unless stub-resolver installed) |
| Firewall | firewallConfig, attackDefense | APPLY | `firewall4` conntrack timeouts (`net.netfilter.nf_conntrack_tcp_timeout_*`), syncookies, etc. |
| ACL / custom ACL | acl, wirelessAcl, customAcl | APPLY | fw4 rules per entry (source/dest groups → ipsets) |
| URL filtering | urlFiltering | STUB/ECHO | needs DNS-based enforcement; counters via dnsmasq logs |
| MAC filter / IP-MAC | macFilter, ipMacBinding | APPLY | fw4 macset rules / static ARP entries |
| Session limit | sessionLimit | APPLY | conntrack per-source limits (iptables hashlimit / nft limit) |
| Bandwidth control | bandwidthCtrl | APPLY | tc/qdisc per port (egress/ingress) |
| QoS | qos | APPLY | tc qdisc + DSCP marks |
| IPS | ips, ipsWhiteList/BlackList, signatureList, blockCountry, countryGroup | STUB (no IPS engine) | ack only; ipsThreat INFORM stays empty |
| Service types / groups | serviceType, ipGroup, ipv6Group, ipPortGroup, ipv6PortGroup, macGroup, domainGroup, domainNoPortGroup | APPLY | store as reference data used by other APPLY keys (fw4/ipset) |
| Time ranges | timeRange | APPLY | used to schedule other rules |
| NAT | natPf, oneToOneNat, disableNat, natAlg | APPLY | fw4 redirects + SNAT rules; counters feed INFORM `portforward` |
| Routing | staticRouting, policyRouting | APPLY | `network.static_route` / policy routing rules+tables; echoes feed INFORM `routingTable` |
| VPN | vpn, vpnUser, ipsecFailover | APPLY (per profile gate) | strongswan (IPsec), xl2tpd/pppd (L2TP/PPTP), openvpn; Wireguard native |
| SSL-VPN | sslVpn | STUB | no OpenVPN-based SSL VPN in stock OpenWrt |
| Wireguard | wireguard | APPLY | `network.interface.wg*` + peers |
| RADIUS/LDAP | radiusProfile, ldap | ECHO | auth backends for VPN users |
| Services | snmp, ssh, led, lldp, hwOffload, jumbo, upnp, mdns, echoServer | APPLY | uci (dropbear, miniupnpd, snmpd, led, avahi); hwOffload/jumbo = offload flags + MTU |
| Log | remoteLog, logNotification | APPLY | syslog-ng/logd remotes |
| Time | timeSetting, dstConfig | APPLY | system.ntp + sysntpd |
| Reboot schedule | rebootSchedule | APPLY | cron entries |
| Clients | client, clientOpt, clientIpBinding, clientTrafficRequire, clientRateConfig, clientOperation | APPLY/ECHO | DHCP static leases + fw4 blocks |
| Device misc | common, controllerInfo, controllerSetting, privacyPolicy, standaloneMgmt, sideParams, abnormalDetect, delayEffect, highAbility | ECHO | stored; most are no-ops on this platform |
| Wireless | wirelessBasic_*, wirelessAdv_*, ssid_*, limits_*, loadBalance_*, rssi_*, roaming, mesh, bandSteering, ppskV3, macFilter*, scheduler*, qosConfig_*, rogueApScan, rfScan, channelDeploy, powerControl | APPLY (er706w profile only; otherwise DECLINE) | hostapd/uci wireless; full radio reporting is a later phase |
| VoIP | all voip keys | DECLINE on non-VoIP models | no FXS hardware |
| Tools | terminalSetting, monitorServer, transferChannel, packageCapture, deviceDebugInfo | LIVE (implemented) | existing side effects in manage.c |
| Account | userAccount | LIVE (implemented) | state account update |

## 8. Coverage matrix (current agent status)

| Area | Status |
|---|---|
| SET ack (all keys, per-key acks, echo of sequenceId/configVersion) | done |
| SET storage (state blobs, replay on GET) | done |
| Tools channels (terminal/monitor/transfer/capture) | done |
| GET live: arptable, dhcpClient, sessionLimit, dnsCache, dpiProtocols, radioStatus | done |
| GET live: vpn, sslVpn, ddnsStats, urlFiltering, telephoneNumber, voiceMail, voiceMailSettings, aclHit | missing (stored echo only) |
| INFORM: deviceInfo, portInfo(+ip4/ip6), trafficStat, client, dhcpClient, arp, routingTable, ctTable, networkTraffic, monitor, lastCfgResult, cfgResults | done (live) |
| INFORM: ddns, portforward, aclHit, qos | echo-only (no real counters) |
| INFORM: vpn, sslVpn, wireguard, sdwan, lte, clientTraffic, lldp, poe, abnormalDt, eventInform, applicationsTraffic, portalDuration | stub/empty |
| INFORM: wifi sections (wSettings, radioTraffic, ssidStats, mesh, roaming) | stub (radioEnable fixed false) |
| Config application (APPLY targets above) | not started |
| UCI write path | not started (config.c is read-only today) |
| ubus reload calls (network/dhcp/firewall) | not started (helper pattern exists) |
| devCap/specification full surface | partial (subset sent) |

## 9. Wire quirks checklist

- SET key for port forwarding is `natPf`; the INFORM stats section for it is
  `portforward`.
- VPN user config key is `vpnUser`, but its ack (and config body field) is
  `vpnUsers`.
- The third 5 GHz wireless-advanced key is uppercase on the wire:
  `WIRELESS_ADV_5G2`.
- DHCP option lists: `dhcp_opts` in WAN/LAN configs, `dhcpOptions` inside
  virtual WAN entries.
- `custom_network` is the one snake_case key inside the SD-WAN config; VPN
  entries mix snake_case (`net_id`, `local_binding`, `remote_peer`,
  `remote_network`, `local_network`, `customNetwork`) with camelCase.
- `interface` (a reserved word in C) is a JSON key in `network`, `upnp`,
  `ddns` rules, NAT entries, static/policy routing, wireguard peers, and
  package-capture info.
- `5Gbands` starts with a digit.
- Second-SIM fields are suffixed `1` (`quota1`, `bands1`, `autoUnlock1`).
- Types are not uniform: `natPf.settings[].enable` is a string; `disableNat`
  `status` is a string (one-to-one NAT uses int); LDAP `port` is a string;
  IPTV `wanPortId` is a string; `credit`/`data` in LTE quota are floats;
  IPS entity ids are strings; `sid` is a long; `cmdId` is a long.
- `@JsonPropertyOrder`-style ordering hints in reference material are often
  stale; treat the actual key list as the truth.
- Empty objects/arrays are omitted rather than emitted (NON_EMPTY style),
  except where explicitly set to configured-empty.
- Unknown keys inside WAN `settings[]` round-trip (passthrough); keep them
  when echoing stored config back on GET.
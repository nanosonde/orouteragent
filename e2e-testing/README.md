# e2e-testing

A self-contained lab for exercising the agent against an **Om\*d\***
controller, with OpenWrt running as a virtual machine rather than a
mock. Four containers provide separate LAN and WAN segments:

| Container | What it is |
|---|---|
| `ora-e2e-controller` | TP-L\*nk Om\*d\* Software Controller 6.2 (`mbentley/omada-controller`) |
| `ora-e2e-openwrt` | OpenWrt 25.12.5 x86-64 booted under qemu with KVM |
| `ora-e2e-proxy` | plain-HTTP reverse proxy to the controller UI |
| `ora-e2e-isp-gateway` | IPv4 DHCP server and local next hop for the WAN |

The VM's first interface is bridged onto the same Docker network as the
controller, so the two are layer-2 adjacent: UDP broadcast discovery works
exactly as it would on a real LAN, and the controller can reach the device
directly. Its second interface is bridged onto an isolated WAN segment and
obtains an IPv4 address and default route from the ISP gateway.

```
  host :8043 ─┐
  host :8090 ─┼─► proxy ──► controller 172.28.0.10
  host :2323 ─┘                         ▲
              (serial console)          │ LAN 172.28.0.0/24
                                        ▼
                   orouteragentd ── OpenWrt VM
                                        ▲
                                        │ WAN 192.0.2.0/24 (IPv4 DHCP)
                                        ▼
                             ISP gateway 192.0.2.2
```

## Quick start

```sh
just e2e-up                # start everything and wait until it is ready
just e2e-controller-setup  # one-time: complete the controller wizard
just e2e-deploy            # build the agent for x86-64 and install it in the VM
just e2e-agent-logs        # watch the agent talk to the controller
```

Then adopt the device in the controller UI (Devices → Pending).

## Why a second architecture

The package installed here is built for **x86-64**, not the aarch64 of
the real GL-MT2500 target, because the VM is x86-64 so KVM can be used.
`just e2e-build` builds it with the official OpenWrt SDK container for
`x86/64`; nothing extra is installed on the host. The OpenWrt version is
pinned to the same 25.12.5 as the router target so the library ABIs
match.

The resulting `.apk` is staged in `e2e-testing/artifacts/`, separate
from the normal `dist/<target>/` build output. Docker mounts this directory
read-only at `/artifacts` so a package can be copied into the VM image on
first boot. `just e2e-install` and `just e2e-deploy` also read the same
directory to reinstall a newly built package in an already running VM.
Generated packages in both output locations are ignored by Git.

## Useful tasks

| Task | Purpose |
|---|---|
| `just e2e-status` | container status and controller `/api/info` |
| `just e2e-ssh` | shell on the OpenWrt VM |
| `just e2e-console` | VM serial console (telnet, ctrl-] to leave) |
| `just e2e-agent-status` | agent process, UCI config, state file, recent log |
| `just e2e-devices` | what the controller thinks it manages |
| `just e2e-controller-logs` | controller log (its decode errors show up here) |
| `just e2e-isp-logs` | DHCP requests and leases from the simulated ISP |
| `just e2e-down` / `e2e-destroy` | stop, keeping / discarding all state |

## WAN configuration

The WAN defaults use the RFC 5737 documentation network `192.0.2.0/24`.
The gateway is `192.0.2.2`, and dnsmasq leases addresses from
`192.0.2.100` through `192.0.2.199` for 12 hours. The Docker network is
internal: the gateway supplies DHCP and a reachable local next hop, but it
does not provide DNS, forwarding, NAT or Internet access. DHCPv6 and router
advertisements are also disabled.

These defaults can be changed together when a host network conflicts:

| Variable | Default |
|---|---|
| `E2E_WAN_SUBNET` | `192.0.2.0/24` |
| `E2E_WAN_PREFIX` | `24` |
| `E2E_WAN_DOCKER_GATEWAY` | `192.0.2.1` |
| `E2E_WAN_DOCKER_POOL` | `192.0.2.224/27` |
| `E2E_ISP_ADDRESS` | `192.0.2.2` |
| `E2E_OPENWRT_WAN_CONTAINER_IP` | `192.0.2.3` |
| `E2E_OPENWRT_WAN_MAC` | `52:54:00:12:34:57` |
| `E2E_WAN_DHCP_START` / `E2E_WAN_DHCP_END` | `192.0.2.100` / `192.0.2.199` |
| `E2E_WAN_NETMASK` | `255.255.255.0` |
| `E2E_WAN_LEASE_TIME` | `12h` |

The ISP service shares the OpenWrt host container's network namespace so
dnsmasq can bind directly to the QEMU-facing WAN bridge. This avoids host
Docker firewall rules filtering DHCP broadcasts while keeping dnsmasq in a
separate container and image.

## Notes

- The controller wizard is a genuine one-time step. Its database lives in
  a docker volume, so it survives `e2e-down` and is only needed again
  after `e2e-destroy`.
- The OpenWrt container is privileged: it needs `/dev/kvm`, and it
  rewrites its own networking into two bridges so the guest can have its own
  LAN and WAN interfaces.
- The guest disk and the downloaded image are cached in a volume, so a
  restart does not re-download OpenWrt. Network seed revisions regenerate
  the guest disk once while retaining the cached download and controller
  state.
- `proxy/controller_proxy.py` is adapted test support; it strips
  `Secure` from the session cookie, decompresses
  responses and tunnels WebSockets so the UI is usable over plain HTTP.
- This lab is for local testing only. It uses fixed credentials, a
  generated throwaway ssh key and self-signed TLS; do not expose it.

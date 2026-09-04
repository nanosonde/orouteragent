# orouteragent

**Make an OpenWrt router appear in a TP-L\*nk Om\*d\* controller as if it
were a TP-L\*nk gateway.**

TP-L\*nk's [Om\*d\*](https://www.tp-link.com/us/omada-sdn/) is a
controller-based system for managing a fleet of network devices from one
place. It only manages TP-L\*nk hardware. If your network is otherwise
Om\*d\*-managed but your router runs OpenWrt, that router is invisible to
the controller.

`orouteragent` is a small daemon for OpenWrt that speaks the Om\*d\*
device protocol, so the controller adopts your router and lists it
alongside everything else — with real status from the actual router:
port link and speed, throughput, connected clients, DHCP leases, routes.
The controller's tools work too: opening a terminal gives you a real
shell on the router, and a packet capture returns real traffic.

You choose which model to present (ER605, ER706W, ER7206, ER707-M2 or
ER8411) with one config option; the default is the ER707-M2.

> **Status:** discovery, adoption, restart resume, and status reporting are
> verified end to end against an Om\*d\* Software Controller. Configuration
> pushed from the controller is acknowledged but **not** applied to the router.
> See [Limitations](#limitations) before relying on it.

## How it works

The controller thinks it is talking to a TP-L\*nk gateway. The daemon
implements the device side of that conversation:

```
OpenWrt router                                     Om*d* controller
     │  UDP  29810   announce, "I am here"      ──►
     │  TLS  29814   adopt, then status every 10s ─►   device appears, stays online
     │  TLS  29816   remote terminal            ──►   Tools -> Terminal
     │  TLS  29817   ping / traceroute          ──►   Tools -> Network Check
     │  TLS  29815   capture file transfer      ──►   Tools -> Packet Capture
```

It is one C daemon (`/usr/sbin/orouteragentd`) with no scripting runtime,
using only libraries already in the OpenWrt feeds (mbedTLS, json-c,
ubus/ubox, uci, mnl). Measured resident size while idle and connected is
about 1.5 MB.

## Requirements

- A router running **OpenWrt 25.x** (built and tested against 25.12.5)
- A **TP-L\*nk Om\*d\* Software Controller**, v5.15 or v6.2
- To build: **Docker** and [`just`](https://github.com/casey/just). The
  build runs inside the official OpenWrt SDK container, so no toolchain
  or SDK is installed on your machine.

## Quickstart

### Try it with no hardware

The repository ships a self-contained lab: an Om\*d\* controller, a real
OpenWrt VM and a simulated IPv4 ISP gateway, all in Docker. The VM has a
controller-facing LAN and a DHCP-configured WAN. This is the fastest way to
see what the project does. It needs Docker and KVM.

```sh
just e2e-up                # controller + OpenWrt VM (~5 min first time)
just e2e-controller-setup  # one-time controller wizard, in your browser
just e2e-deploy            # build the agent and install it in the VM
just e2e-agent-logs        # watch it talk to the controller
```

Then open the controller at <http://127.0.0.1:8090>, go to Devices, and
adopt the router that appears. See
[e2e-testing/README.md](e2e-testing/README.md) for the details.

### Install on a real router

```sh
just build                 # cross-build for mediatek/filogic (aarch64)
```

This runs the official [OpenWrt SDK
container](https://github.com/openwrt/docker) — nothing is installed on
the host. The package lands in `dist/<target>/orouteragent-<version>.apk`.
The default target is `mediatek/filogic`.

Pass another OpenWrt target/subtarget as the argument to `build`:

```sh
just build mediatek/filogic  # dist/mediatek-filogic/ (default)
just build x86/64            # dist/x86-64/
just build ramips/mt7621     # dist/ramips-mt7621/
```

Use the exact `target/subtarget` name from the [OpenWrt target
list](https://downloads.openwrt.org/releases/25.12.5/targets/). The build
recipe passes that value to the matching SDK container and replaces `/`
with `-` only when naming the output directory and SDK cache. Each target
therefore has independent cached SDK state and packages. `just analyze
<target/subtarget>` selects the static-analysis toolchain in the same way.

`just e2e-build` is the dedicated x86-64 lab build. It uses the same package
builder but stages its result in `e2e-testing/artifacts/` instead of `dist/`
so the E2E VM can install it.

The SDK is cached in a docker volume, so only the first build pays for
the image pull and feed setup. `just sdk-clean` discards those volumes.

Copy it to the router and install:

```sh
apk add --allow-untrusted ./orouteragent-0.1.0-r1.apk

uci set orouteragent.agent.enabled=1
# model defaults to ER707-M2; hw_version to 1.0
uci set orouteragent.agent.model=ER707-M2    # ER605|ER706W|ER7206|ER707-M2|ER8411
uci set orouteragent.agent.hw_version=1.0    # hardware revision
uci commit orouteragent

service orouteragent enable
service orouteragent start
```

The router appears in the controller as **Pending** within about ten
seconds, ready to adopt. If the controller is on a different subnet,
broadcast discovery cannot reach it — point the agent at it directly:

```sh
uci set orouteragent.agent.controller=192.168.1.10; uci commit
service orouteragent restart
```

Check what it is doing with `logread -e orouteragent`.

## Configuration

Everything lives in `/etc/config/orouteragent`:

| Option | Default | Meaning |
|---|---|---|
| `enabled` | `0` | master switch; the daemon exits when 0 |
| `model` | `ER707-M2` | which gateway to present (unknown value falls back to ER707-M2) |
| `controller` | *(empty)* | controller address; empty means find it by broadcast |
| `hw_version` | `1.0` | reported hardware revision |
| `fw_version` | *(per model)* | reported firmware string |
| `mac` | *(the LAN MAC)* | identity reported to the controller |
| `wireless` | *(from model)* | force WiFi-capable reporting |
| `controller_port` | `8043` | controller HTTPS port |
| `discovery_mode` | `auto` | `auto`, `unicast` or `broadcast` |
| `verify_tls` | `0` | verify the controller certificate (needs a CA bundle) |
| `device_username` / `device_password` | `admin` / `admin` | credentials used at adoption |
| `adopt_port` | `29814` | controller management port |
| `inform_interval` | `10` | seconds between status reports |
| `state_file` | `/etc/orouteragent/state.json` | where adoption state is kept |
| `max_config_kb` | `512` | cap on stored controller config |
| `log_level` | `2` | 0 error, 1 warning, 2 info, 3 debug (frames plus redacted GET/SET details) |

`config portmap` sections map the emulated ports onto real interfaces.
By default port 1 is the WAN and the rest are LAN:

```
config portmap
	option index 2
	option interface 'eth1'
```

## What the controller sees

- **Discovery**: an announce on UDP 29810 every 10 s until a controller
  answers; the device then shows up as *Pending*.
- **INFORM** every 10 s carrying real state: port link/speed/addresses
  (`/sys`, netlink), traffic counters (`/proc/net/dev`), routing table
  (netlink), DHCP leases, ARP neighbours, conntrack usage, CPU and
  memory.
- **SET**: acknowledged properly (per-key acks, echoed `configVersion`)
  and stored, then replayed on `GET`. See the limitation below: the
  config is remembered, not applied.
- **FORGET**: returns the device to *Pending*.
- **UPGRADE**: declined — this is not TP-L\*nk firmware.
- **Tools**: Terminal opens a real shell on the router, Network Check
  runs real `ping`/`traceroute`, Packet Capture captures real traffic
  and hands the `.pcap` back through the controller's transfer channel.

## Limitations

Worth knowing before you rely on this:

- **Controller config is not applied.** A `SET` from the controller is
  acknowledged and stored so the UI stays consistent, but it is not
  translated into OpenWrt UCI. Changing a setting in the controller will
  *not* change your router's behaviour yet.
- Wireless models report their radios as disabled; OpenWrt's own WiFi is
  not exposed to the controller.
- Per-client traffic accounting and LLDP are reported empty.
- The emulated identity is cosmetic: TP-L\*nk firmware features that do
  not exist on OpenWrt are not implemented.

## Development

```sh
just                # list every task
just build          # cross-build for the router, in the SDK container
just check          # decoder unit tests + fuzz pass, under ASan/UBSan
just analyze        # gcc -fanalyzer over every source file
just fuzz 200000    # longer fuzz campaign
just sdk-clean      # discard the cached SDK volumes
```

Build or analyze another target by passing its OpenWrt target/subtarget
through, for example `just build x86/64` or `just analyze x86/64`. See
[Install on a real router](#install-on-a-real-router) for output paths and
additional examples.

The SDK containers come from `ghcr.io` by default. Point the build at
another mirror or pin a different release with:

| Variable | Default | Purpose |
|---|---|---|
| `ORA_SDK_REGISTRY` | `ghcr.io` | registry holding the images (`docker.io`, `quay.io`) |
| `ORA_SDK_IMAGE` | *(derived)* | full image reference, overriding registry and tag |
| `OPENWRT_VERSION` | `25.12.5` | release to build against |
| `ORA_BUILD_JOBS` | *(host cpu count)* | parallel make jobs |

```sh
ORA_SDK_REGISTRY=docker.io just build
ORA_SDK_IMAGE=my.registry/openwrt/sdk:mediatek-filogic-25.12.5 just build
```

### Tests

The protocol decoders parse untrusted bytes off the network and depend
only on libc, so they are tested on the host even though the daemon
itself cannot be built there (it needs the OpenWrt-only libraries).

`test/test_protocol.c` covers the framing reader (split feeds, several
frames per read, oversized and truncated lengths), the RTTY V1/V2 codec
— including the two properties the controller enforces, a REGISTER
payload that splits into exactly four NUL-separated segments and a
heartbeat that is never empty — the DMP protobuf codec, base64 against
the RFC 4648 vectors, and the growable buffer.

`test/fuzz_protocol.c` drives the same decoders with random and mutated
frames. It found a real bug on its first run: a `uint8_t` promoted to
`int` before a 24-bit shift in the UDP datagram decoder, i.e. undefined
behaviour reachable from any unauthenticated discovery packet.

The rest of the daemon is covered by the SDK cross-build and
`just analyze`. There is deliberately **no host build** of the agent:
libuci/libubus/libubox are OpenWrt-only, so the cross-build is the
compile gate.

### Layout

| Path | Contents |
|---|---|
| `src/protocol/` | wire format: framing, JSON envelope, auth, RTTY, DMP protobuf, pcap |
| `src/services/` | one file per channel: discovery, manage, rtty, dmp, capture, transfer, tls |
| `src/model/` | the five gateway profiles (protocol-visible data) |
| `src/` | config (UCI), state, system info, INFORM builder, netlink |
| `test/` | host tests and the fuzz driver |
| `e2e-testing/` | the docker lab |
| `dist/<target>/` | ignored output from `just build`, grouped by target (for example `mediatek-filogic`) |
| `e2e-testing/artifacts/` | ignored x86-64 package staging used by the E2E VM seed and reinstall scripts |

### Debugging

Set `log_level=3`. The agent is the TLS endpoint for every channel, so
its own log is the definitive wire capture — there is nothing to sniff.
At this level, GET and SET requests and successful responses include their
JSON bodies with credentials, tokens, authentication material and keys
redacted. Large bodies are emitted as numbered chunks. Per-key lines also
show whether SET data was stored and whether a GET result was generated live,
read from stored controller configuration or missing. These diagnostics do
not mean that stored controller configuration was applied to OpenWrt.

```sh
uci set orouteragent.agent.log_level=3; uci commit
service orouteragent restart
logread -f -e orouteragent
```

## Roadmap

- Translate pushed controller config into UCI so settings actually apply
- Real radio reporting on WiFi models; per-client traffic accounting and LLDP

## License

GPL-2.0-or-later (see [LICENSE](LICENSE)).

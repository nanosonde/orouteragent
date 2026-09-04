# Changelog

Development history of **orouteragent**, newest first.

The repository history is consolidated into one initial commit. Before that
rewrite, every commit reachable from `main` was recorded below with its
committer timestamp and full message. Vendor names and implementation-source
details are normalized for publication, so this is a content-preserving record
rather than a byte-for-byte copy of the original messages.

## Unreleased

- Add a second QEMU network interface to the E2E OpenWrt VM on an isolated
  WAN segment, configured as the guest's DHCP client and default route.
- Add a dedicated dnsmasq-based ISP gateway container that serves IPv4 leases
  and a local next hop without DNS, DHCPv6, forwarding, NAT or Internet access.
- Make E2E readiness wait for the WAN lease and route, add ISP DHCP logging,
  and automatically reseed cached guest disks for the new network layout.
- Add DEBUG-level, recursively redacted GET/SET request and response bodies,
  split into numbered syslog chunks for large controller configurations.
- Report whether each SET key was stored and whether each GET value came from
  live device state, stored controller configuration or was unavailable.

## fix: complete ER707-M2 controller integration

`6117347808d7b4110be7c1f92fa7fe8a47bcbc55` - 2026-09-06T16:30:05+02:00

## fix(config): compare the model name the way the lookup normalizes it

`08d3573592601f51e1fadad126c15179b72c43a8` - 2026-09-06T14:30:52+02:00

config.c warned "unknown model 'ER707-M2'" because it compared the raw
UCI value against the profile key with strcasecmp - but the lookup
strips separators and lower-cases first, so the profile was in fact
found. Compare with the same normalization so the warning only fires
when the profile really fell back to the default.

## fix(inform): send latency on the WAN port like managed gateways

`a38008d9f464a6f2ac992e4d294f865453bb4333` - 2026-09-06T14:22:43+02:00

WAN health handling requires latency on every configured WAN port. The
ER707-M2 profile configures two WAN ports (1 and 3), and latency was only
sent on port 1, which prevented Internet status from being processed.

latency now goes in the WAN-port block of portInfo only, alongside
ip/netmask/publicWanIp.

## fix(manage): send monitorCapabilities in the negotiation body

`c0ab4b98d43c5dd08fdacad9b3d5933eca1959a8` - 2026-09-06T13:14:38+02:00

Adoption requires monitorCapabilities.protocols in the negotiation body.
Send the section expected for the gateway monitor channel: protocols
[TLS], in/out types [protobuf2], compressMethods [lzo-2.07].

Also normalize the UCI model name when looking up the profile
("ER707-M2" - separators stripped, lower cased - matches the profile
key er707m2), so the misleading "unknown model" warning is gone.

## feat(model): add the ER707-M2 and make it the default

`ffe543496753eed6523153bc1707a69a000e1ced` - 2026-09-06T13:08:03+02:00

New profile for the ER707-M2 multi-gig VPN gateway, using the model's
management capability data:

- 7 ports: 2x 2.5G RJ45 (WAN1 type=3, WAN/LAN2), 4x Gigabit WAN/LAN,
  1x Gigabit SFP WAN/LAN, with the required port names, types, modes and
  speed-duplex lists.
- hwId/oemId/fwVer aligned with the model profile (fw 1.4.0 Build 20251208
  Rel.72306), fixing the identity mismatch during adoption.
- 119 components_v2 entries, including sdwan 1.1 and wanLoadBalance 1.1.
- devCap.specification capacities (acl 64, staticRouting 128,
  sslVpnUsers 512, vpnUsers 270, wireguard 20 peers, ...).

Empty or unknown UCI model now falls back to this profile; ER7206 and
the others remain selectable. The e2e lab defaults were switched to
match, with hw_version pinned to 1.0.

## fix(inform): correct mesh schema and wireless capability of profiles

`92de0fa7f84dbb2671fc3a2dd65be9aa67a33053` - 2026-09-06T12:33:25+02:00

Three related defects kept the controller's Internet page blank:

- The mesh INFORM section sent candidateParents as a bare array, but the
  management schema requires a CandidateParents object ({status,
  parentList}). Send the object form with an empty parentList.
- The ER7206 and ER8411 profiles declared wireless=1. Both are wired
  routers (no on-board radio); only the ER706W has one. Corrected to
  wireless=0 for both.
- An adopted agent resumed by announcing over discovery and waiting for
  a pre-adopt reply, which only comes when an operator adopts an unmanaged
  device. It now resumes the management session directly from the
  configured controller, and if the provisioned account is rejected after
  a reset or forget, the agent returns to factory state and can be adopted
  again.

## fix(discovery): send customizeRegion in deviceMisc

`7ae2e42cc56b23b9408acfaa7733a1884f2c1d72` - 2026-09-06T12:13:35+02:00

The discovery contract requires deviceMisc.customizeRegion. Announcements
without the field could not be persisted by the discovery service.

All four deviceMisc builders (discovery announce, PRE_CONNECT_INFO,
negotiation device info, devCap) now send customizeRegion: 0, the
region-unset value for a factory device.

## feat(model): default to an ER7206 v2

`4e635f77862a32b84d21bbcf2ee497211bebe466` - 2026-09-06T12:08:52+02:00

An ER7206 with hardware revision 2.0 becomes the identity the agent
presents out of the box; ER605 remains available via
'option model ER605'.

- profiles.c: the default profile returned for empty or unknown UCI
  model names is now the ER7206 entry, selected by index so the table
  order does not silently own that choice.
- files/orouteragent.config: model ER7206, hw_version 2.0.
- e2e lab (compose, seed-image.sh, install-agent.sh): same defaults,
  so the lab device matches what a fresh install presents. hw_version
  is pinned there because the installed config predates the change.
- README/Makefile package description updated.

The inform path already renders hwVer as "<model> v<hw_version>", so the
controller sees "ER7206 v2.0"; discovery and negotiation report the bare
"2.0".

## fix(manage): frame TLS messages with the ECSP length prefix

`e285af578f6f034925878aceca3f74b1b05ff850` - 2026-09-06T11:59:55+02:00

Adoption reached the TLS connection but the first message was rejected
because the opening JSON bytes were interpreted as a big-endian frame
length. This was the same missing-prefix bug as the UDP announce, one
layer up.

send_msg in manage.c and transfer.c wrote the bare JSON from
ora_msg_encode, while the receive paths on the same sockets use
ora_frame_reader and expect the prefix; dmp.c and rtty.c already framed
correctly. With the prefix in place the full adoption handshake completes:
the device verifies, receives its configuration, and reports connected.

## fix(discovery): frame the UDP announce like every other ECSP message

`669e68259d15069ef3b621fd02e544a5b9c67056` - 2026-09-06T11:55:31+02:00

The controller did not accept the agent announcement because the first
four bytes of bare JSON were interpreted as the frame length.

ECSP frames every message, including the plaintext UDP discovery
datagram, with a 4-byte big-endian length prefix ahead of the JSON. The
TCP paths already did this via ora_frame_encode; only the discovery
datagram was sent bare.

With the frame in place the device becomes visible for adoption. The
decoder already accepted both forms, so nothing changed on the receive
side.

Also make 'just e2e-ssh' quote its command argument: shell operators in
it used to run on the host instead of the VM, and 'just e2e-agent-logs'
gained an optional line count to print the tail without following.

## fix(e2e): use the controller image's own healthcheck

`c51414cd9e74f578b9306ab731ee828a3f3a2c99` - 2026-09-06T11:20:39+02:00

The healthcheck called curl, which the controller image does not ship,
so the container could never become healthy and anything waiting on it
would wait forever. The image provides /healthcheck.sh for this.

## perf(build): compile the package sources in parallel

`49a1386eaf9c23f59ce75cc025110facc2f5a592` - 2026-09-06T11:20:39+02:00

Without PKG_BUILD_PARALLEL the build system hands Build/Compile a -j1
job server, so every source file was compiled one at a time regardless
of how many cores the machine has.

On its own this is worth about two seconds here, since the package is
small; it is the packaging step that dominated, and that is addressed
separately.

## refactor(build): build with the official OpenWrt SDK containers

`c66069c7ad151da0c1bec6a0f90a6e3c3ce67425` - 2026-09-06T11:20:33+02:00

Building used to require unpacking a ~4 GB SDK into the home directory
and keeping it in step with the release by hand. Use the images from
ghcr.io/openwrt/sdk instead, so a checkout plus docker and just is all
anyone needs.

scripts/sdk-run.sh runs a script inside the SDK for a target; the SDK
itself is cached in a named docker volume so feeds and object files
survive between runs. The registry, image, release, output directory
and job count are all overridable, for mirrors and air-gapped setups.

Two wrinkles worth recording:

- The SDK's default config enables the lua bindings of uci, ubox and
  ubus, which drag in lua, whose makefile fails under parallel make.
  Nothing here links against it, so every lua package is switched off.
- The container runs as root purely so the packaging step can skip
  fakeroot. OpenWrt uses fakeroot to have apk record files as owned by
  root; when the build really is root that is unnecessary. Intercepting
  every syscall of 'apk mkpkg' costs ~110s for this 49 KB package against
  ~5s without, which was over 90% of a rebuild. Artifacts are chowned
  back to the invoking user on exit.

A rebuild after touching every source file now takes ~5s, down from
~128s.

## docs: rewrite the README for people new to the project

`1323abe1c8da8c8bc4302fa31e0863bf62eca4ec` - 2026-09-05T11:41:16+02:00

The README opened straight into a protocol diagram and assumed the
reader knew what Om\*d\* is, while the build steps predated the justfile
and the e2e lab was not mentioned at all.

It now starts by explaining the problem being solved in plain terms,
then gives two quickstarts: the docker lab, which needs no hardware and
is the fastest way to see the thing work, and installing on a real
router. Requirements, task list, layout and debugging are documented,
and the status is stated honestly up front: adoption is not yet verified
and pushed config is stored rather than applied, both of which a reader
would otherwise discover the hard way.

Supporting change: scripts/build-package.sh now fetches and configures
the SDK for any target, so 'just build' works on a clean machine instead
of assuming a hand-prepared SDK. The e2e x86-64 build is a thin wrapper
around it, and the package name in the feeds install list is corrected
to libjson-c. Output moves to dist/<target>/.

Every command and link in the README was executed or checked; the 1.5 MB
footprint is measured from the running agent, not estimated.

## fix(justfile): make analyze match the real build and fail on findings

`9b6c37bb937e12a26b47fd2b22a490e27f63b258` - 2026-09-05T11:35:27+02:00

Two problems with the recipe. It passed the SDK headers with -I while
the package build uses -isystem, so warnings from inside libubox's own
macros were reported as if they were ours. And gcc exits 0 on warnings,
so findings scrolled past while the task still reported success.

The SDK include is now -isystem, the warning set matches the package
build, and any warning or error fails the task. Verified by planting a
use-after-free: it is reported and the recipe exits non-zero.

## test(e2e): add a local controller and OpenWrt VM lab

`95c4daa8f98aed0c00bc89f4ea30fa65eaad0e89` - 2026-09-05T11:28:19+02:00

Adds e2e-testing/, a Docker lab for exercising the agent end to end
against an Om\*d\* controller, plus a justfile at the repository root.

- controller: `mbentley/omada-controller` 6.2 with the required ports
  and persistent volumes.
- openwrt: OpenWrt 25.12.5 x86-64 booted under qemu with KVM. The
  container bridges its own eth0 with a tap device so the guest gets an
  address on the Docker network and is layer-2 adjacent to the controller,
  allowing broadcast discovery to behave as it would on a LAN. The disk
  image is seeded before boot with an SSH key, static address, and package.
- proxy: adapted test support, containerized so the UI is usable over
  plain HTTP.
- scripts: x86-64 SDK bootstrap and package build, package installation,
  readiness checks, and device listing.

The justfile also covers the existing build, check, fuzz, and analyze
workflows.

Verified: the guest boots, is reachable over SSH, and the agent runs and
announces discovery to the controller.

## fix(log): log through syslog with real severities

`b3c1434dc5d48089a8876033b2fefa7f15c32761` - 2026-09-05T11:28:04+02:00

Everything went to stderr, which procd forwards to syslog at the error
level, so routine INFO and DEBUG lines were recorded as daemon.err and
drowned out actual errors. The daemon now calls syslog(3) directly and
maps its levels onto LOG_ERR/WARNING/INFO/DEBUG, and procd no longer
mirrors stderr. Output is still echoed to the terminal when stderr is a
tty, for running the daemon by hand.

## fix(config): resolve the board MAC from the LAN device, not the interface name

`a87beaea4fc5f058f1b2fb7c1d067c47453e91c4` - 2026-09-05T11:28:04+02:00

The agent asked ubus for network.device status with name "lan", but
"lan" is a logical interface, not a device, so the call failed with
EINVAL and devices fell back to the synthetic 02:00:00:00:00:01. Since
the controller keys a device on its MAC, multiple agents could have
presented the same identity.

The device name now comes from network.interface.lan (l3_device, then
device), the MAC from network.device status for that name, with sysfs
reads of the resolved device and then br-lan/eth0/lan as fallbacks so
the lookup still works without ubus.

## chore(threads): give the service threads an explicit 128 KB stack

`9b4e4e9731d8938b6e7a3849101a7027e0e5f080` - 2026-09-05T10:58:19+02:00

The RTTY, DMP, capture and transfer threads only hold small frame
buffers. Setting the stack size explicitly rather than inheriting the
libc default keeps the daemon's memory footprint predictable and
independent of which libc it is built against (musl already defaults to
128 KB; glibc would give each thread 8 MB of address space).

## test: add decoder unit tests and a fuzz driver

`3cbb335a1d8f8e614f772d0331a7fca9d7972738` - 2026-09-05T10:58:08+02:00

The protocol decoders parse untrusted bytes and depend only on libc, so
they can be tested on the host even though the daemon cannot be built
there. Both binaries run under ASan and UBSan.

test_protocol covers the framing reader (byte-at-a-time reassembly,
several frames per read, oversized and truncated declared lengths, the
exact max-payload boundary), the RTTY V1/V2 codec including the two
properties the controller enforces (a REGISTER payload that splits into
exactly four NUL-separated segments, and a heartbeat that is never
empty), the DMP protobuf codec, base64 against the RFC 4648 vectors
plus its malformed-input rejections, and the growable buffer.

fuzz_protocol mixes random buffers with mutated valid frames and feeds
them to the framing, RTTY, DMP and base64 decoders in random-sized
slices to mimic TCP segmentation. It is seeded deterministically so a
failure reproduces; FUZZ_SEED=0 randomizes.

cmocka and libFuzzer, which the plan called for, are not available in
this toolchain, hence the small self-contained harness and driver.

## fix(framing): avoid signed overflow decoding the datagram length

`6ebf9593c069aa15a2979b15fa13fa383a013846` - 2026-09-05T10:57:55+02:00

The four length bytes were promoted from uint8_t to int before being
shifted, so a first byte of 0x80 or higher overflowed a signed int while
computing <<24. That is undefined behavior on the UDP discovery path,
which parses unauthenticated packets from any host on the network. The
stream reader already cast correctly; the datagram decoder now does too.

Found by the new fuzz driver on its first run.

## feat: initial commit

`f3ec45dff3675a49b42615a268145957de63548e` - 2026-09-04T19:06:07+02:00

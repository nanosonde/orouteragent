#!/bin/sh
# Seed a freshly downloaded OpenWrt disk image with everything the e2e
# run needs: a static address on the lab network, an SSH key, the agent
# package and its UCI configuration.
#
# The image is a partitioned disk, so the rootfs is reached through a
# partition-scanning loop device. Doing this offline avoids having to
# drive the serial console interactively on first boot.
set -eu

IMAGE="$1"

VM_IP="${VM_IP:-172.28.0.50}"
VM_PREFIX="${VM_PREFIX:-24}"
CONTROLLER_IP="${CONTROLLER_IP:-172.28.0.10}"
AGENT_MODEL="${AGENT_MODEL:-ER707-M2}"

log() { echo "[seed] $*"; }

# Partition device nodes (loop0p2) are not created inside a container, so
# map the rootfs partition directly by byte offset instead.
start_sector="$(partx -g -o START -n 2 "$IMAGE" | tr -d ' ')"
sector_count="$(partx -g -o SECTORS -n 2 "$IMAGE" | tr -d ' ')"
if [ -z "$start_sector" ] || [ -z "$sector_count" ]; then
    log "could not read the rootfs partition table entry"
    exit 1
fi

LOOP="$(losetup --find --show \
    --offset "$((start_sector * 512))" \
    --sizelimit "$((sector_count * 512))" "$IMAGE")"
trap 'umount /mnt/rootfs 2>/dev/null || true; losetup -d "$LOOP" 2>/dev/null || true' EXIT

e2fsck -p -f "$LOOP" >/dev/null 2>&1 || true

mkdir -p /mnt/rootfs
mount -t ext4 "$LOOP" /mnt/rootfs

mkdir -p /mnt/rootfs/etc/uci-defaults /mnt/rootfs/root

# uci-defaults scripts run once on first boot, before services start.
cat > /mnt/rootfs/etc/uci-defaults/98-e2e-network <<EOF
#!/bin/sh
# Static address on the lab bridge: the controller has to be able to
# reach this device directly for the pre-adopt reply.
uci -q batch <<UCI
set network.lan.proto='static'
set network.lan.ipaddr='${VM_IP}'
set network.lan.netmask='255.255.255.0'
delete network.lan.gateway
delete network.lan.dns
delete network.lan.ip6assign
set network.wan='interface'
set network.wan.device='eth1'
set network.wan.proto='dhcp'
delete network.wan6
commit network
UCI

# The lab network is trusted; allow the test harness to reach ssh/luci.
uci -q batch <<UCI
set firewall.@zone[0].input='ACCEPT'
commit firewall
UCI
exit 0
EOF

cat > /mnt/rootfs/etc/uci-defaults/99-e2e-agent <<EOF
#!/bin/sh
# Install and configure orouteragent if a package was provided.
pkg=\$(ls /root/orouteragent-*.apk 2>/dev/null | head -n1)
if [ -n "\$pkg" ]; then
    apk add --allow-untrusted "\$pkg" >/var/log/orouteragent-install.log 2>&1 || \
        logger -t e2e "orouteragent package install failed"
fi

if [ -f /etc/config/orouteragent ]; then
    uci -q batch <<UCI
set orouteragent.agent.enabled='1'
set orouteragent.agent.model='${AGENT_MODEL}'
set orouteragent.agent.hw_version='1.0'
set orouteragent.agent.controller='${CONTROLLER_IP}'
set orouteragent.agent.log_level='3'
commit orouteragent
UCI
    # Left stopped on purpose: the harness starts it once the
    # controller is confirmed up, so the first announce is not wasted.
    /etc/init.d/orouteragent enable 2>/dev/null || true
fi
exit 0
EOF

chmod +x /mnt/rootfs/etc/uci-defaults/98-e2e-network \
         /mnt/rootfs/etc/uci-defaults/99-e2e-agent

# Passwordless key login for the harness.
if [ -f /ssh/id_ed25519.pub ]; then
    mkdir -p /mnt/rootfs/etc/dropbear
    cp /ssh/id_ed25519.pub /mnt/rootfs/etc/dropbear/authorized_keys
    chmod 600 /mnt/rootfs/etc/dropbear/authorized_keys
    log "installed ssh authorized key"
else
    log "WARNING: no ssh public key at /ssh/id_ed25519.pub"
fi

# Any package built by the harness.
if ls /artifacts/orouteragent-*.apk >/dev/null 2>&1; then
    cp /artifacts/orouteragent-*.apk /mnt/rootfs/root/
    log "staged $(ls /artifacts/orouteragent-*.apk | wc -l) agent package(s)"
else
    log "no agent package in /artifacts (VM will boot without it)"
fi

sync
umount /mnt/rootfs
losetup -d "$LOOP"
trap - EXIT
log "image seeded"
#!/bin/sh
# Boot OpenWrt x86-64 as a KVM guest bridged onto the container's docker
# network, so the guest is layer-2 adjacent to the lab controller.
set -eu

OPENWRT_VERSION="${OPENWRT_VERSION:-25.12.5}"
VM_IP="${VM_IP:-172.28.0.50}"
LAN_CONTAINER_IP="${LAN_CONTAINER_IP:-172.28.0.20}"
LAN_GATEWAY="${LAN_GATEWAY:-172.28.0.1}"
WAN_CONTAINER_IP="${WAN_CONTAINER_IP:-192.0.2.3}"
VM_MEMORY="${VM_MEMORY:-512}"
VM_CPUS="${VM_CPUS:-2}"
DATA_DIR=/data
DISK="${DATA_DIR}/openwrt.img"
STAMP="${DATA_DIR}/.seeded-${OPENWRT_VERSION}-network-v2"
IMAGE_NAME="openwrt-${OPENWRT_VERSION}-x86-64-generic-ext4-combined.img"
IMAGE_URL="https://downloads.openwrt.org/releases/${OPENWRT_VERSION}/targets/x86/64/${IMAGE_NAME}.gz"

log() { echo "[openwrt] $*"; }

if [ ! -c /dev/kvm ]; then
    log "FATAL: /dev/kvm is missing; the host must expose KVM"
    exit 1
fi

mkdir -p "$DATA_DIR"

# ---- disk image -------------------------------------------------------

if [ ! -f "$STAMP" ]; then
    CACHE="${DATA_DIR}/${IMAGE_NAME}.gz"
    if [ ! -s "$CACHE" ]; then
        log "fetching OpenWrt ${OPENWRT_VERSION} x86-64"
        curl -fL --retry 3 -o "${CACHE}.part" "$IMAGE_URL"
        mv "${CACHE}.part" "$CACHE"
    else
        log "using cached image download"
    fi
    rm -f "$DISK" "${DATA_DIR}/.seeded-"*
    gunzip -c "$CACHE" > "$DISK"
    # The partition table is left alone, so the rootfs keeps the size the
    # image ships with; that is ample for the agent package.
    log "seeding image"
    seed-image.sh "$DISK"
    touch "$STAMP"
else
    log "reusing existing disk (delete the openwrt-data volume to reset)"
fi

# ---- networking -------------------------------------------------------
#
# Move each container address onto a bridge and attach a guest tap.
# Resolve interfaces by address because Compose does not guarantee the
# ethX order when a service joins multiple networks.

bridge_interface() {
    address="$1"
    bridge="$2"
    tap="$3"
    interface="$(ip -4 -o addr show | awk -v address="$address" '
        { split($4, parts, "/"); if (parts[1] == address) { print $2; exit } }
    ')"
    if [ -z "$interface" ]; then
        log "FATAL: no interface has address ${address}"
        exit 1
    fi
    cidr="$(ip -4 -o addr show dev "$interface" | awk '{print $4; exit}')"

    log "bridging ${interface} (${cidr}) with ${tap} on ${bridge}"
    ip link add name "$bridge" type bridge
    ip link set "$interface" master "$bridge"
    ip addr flush dev "$interface"
    ip addr add "$cidr" dev "$bridge"
    ip link set "$interface" up
    ip link set "$bridge" up

    ip tuntap add dev "$tap" mode tap
    ip link set "$tap" master "$bridge"
    ip link set "$tap" up
}

bridge_interface "$LAN_CONTAINER_IP" br0 tap0
bridge_interface "$WAN_CONTAINER_IP" br1 tap1
ip route replace default via "$LAN_GATEWAY" dev br0
sysctl -w net.ipv4.ip_forward=0 >/dev/null

# Stable MACs keep both guest interfaces consistent across restarts.
VM_MAC="${VM_MAC:-52:54:00:12:34:56}"
VM_WAN_MAC="${VM_WAN_MAC:-52:54:00:12:34:57}"

log "starting VM: ${VM_CPUS} cpu, ${VM_MEMORY}M, lan ${VM_IP}/${VM_MAC}, wan dhcp/${VM_WAN_MAC}"
log "serial console: telnet <host> 2323   (also logged to /data/serial.log)"

exec qemu-system-x86_64 \
    -name orouteragent-e2e \
    -machine q35,accel=kvm \
    -cpu host \
    -smp "$VM_CPUS" \
    -m "$VM_MEMORY" \
    -nographic \
    -drive file="$DISK",format=raw,if=virtio,cache=writeback \
    -netdev tap,id=lan0,ifname=tap0,script=no,downscript=no \
    -device virtio-net-pci,netdev=lan0,mac="$VM_MAC" \
    -netdev tap,id=wan0,ifname=tap1,script=no,downscript=no \
    -device virtio-net-pci,netdev=wan0,mac="$VM_WAN_MAC" \
    -chardev socket,id=serial0,host=0.0.0.0,port=2323,telnet=on,server=on,wait=off,logfile=/data/serial.log \
    -serial chardev:serial0 \
    -monitor none \
    -no-reboot
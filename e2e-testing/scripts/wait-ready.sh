#!/usr/bin/env bash
# Wait until the lab is ready: controller API answering and the OpenWrt
# VM reachable over ssh.
set -euo pipefail

CONTROLLER_URL="${E2E_CONTROLLER_URL:-https://127.0.0.1:8043/api/info}"
VM_IP="${E2E_OPENWRT_IP:-172.28.0.50}"
ISP_ADDRESS="${E2E_ISP_ADDRESS:-192.0.2.2}"
SSH_KEY="${E2E_SSH_KEY:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/openwrt/ssh/id_ed25519}"
TIMEOUT="${E2E_WAIT_TIMEOUT:-600}"

log() { echo "[wait] $*"; }

deadline=$((SECONDS + TIMEOUT))

log "waiting for controller at ${CONTROLLER_URL}"
until curl -skf "$CONTROLLER_URL" >/dev/null 2>&1; do
    if (( SECONDS > deadline )); then
        log "TIMEOUT: controller did not come up"
        exit 1
    fi
    sleep 5
done
omadac_id="$(curl -sk "$CONTROLLER_URL" | sed -n 's/.*"omadacId":"\([^"]*\)".*/\1/p')"
log "controller up (omadacId=${omadac_id:-unknown})"

configured="$(curl -sk "$CONTROLLER_URL" | sed -n 's/.*"configured":\([a-z]*\).*/\1/p')"
if [[ "$configured" != "true" ]]; then
    log "NOTE: the controller has not been through its setup wizard yet."
    log "      Adoption stays unavailable until that is done once:"
    log "      run 'just e2e-controller-setup' for the steps."
fi

log "waiting for OpenWrt VM at ${VM_IP}"
until ssh -q -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=5 -i "$SSH_KEY" "root@${VM_IP}" true 2>/dev/null; do
    if (( SECONDS > deadline )); then
        log "TIMEOUT: VM did not become reachable over ssh"
        log "hint: check the serial console with 'just e2e-console'"
        exit 1
    fi
    sleep 5
done
log "VM reachable"

log "waiting for an IPv4 DHCP lease on WAN"
until wan_status="$(ssh -q -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=5 -i "$SSH_KEY" "root@${VM_IP}" \
        'ubus call network.interface.wan status 2>/dev/null; ip -4 route show default' 2>/dev/null)" &&
        grep -q '"up": true' <<<"$wan_status" &&
        grep -q '"address":' <<<"$wan_status" &&
        grep -q "default via ${ISP_ADDRESS} dev eth1" <<<"$wan_status"; do
    if (( SECONDS > deadline )); then
        log "TIMEOUT: WAN did not acquire an IPv4 lease and default route"
        exit 1
    fi
    sleep 2
done

wan_address="$(ssh -q -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -i "$SSH_KEY" "root@${VM_IP}" \
    "ubus call network.interface.wan status | jsonfilter -e '@[\"ipv4-address\"][0].address'")"
wan_route="$(ssh -q -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -i "$SSH_KEY" "root@${VM_IP}" 'ip -4 route show default dev eth1')"
log "WAN ready (${wan_address}; ${wan_route})"

ssh -q -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -i "$SSH_KEY" "root@${VM_IP}" \
    '. /etc/openwrt_release 2>/dev/null; echo "  OpenWrt: $DISTRIB_DESCRIPTION"; \
     echo "  agent:   $(apk list -I 2>/dev/null | grep -c orouteragent || echo 0) package(s) installed"'
log "lab ready"
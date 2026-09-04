#!/usr/bin/env bash
# Run the target-only transform smoke binary against the real OpenWrt UCI.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VM_IP="${E2E_CONTROL_IP:-198.18.0.50}"
SSH_KEY="${E2E_SSH_KEY:-${HERE}/openwrt/ssh/id_ed25519}"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i "$SSH_KEY")
SMOKE="${HERE}/artifacts/orouteragent-config-smoke"

[[ -x "$SMOKE" ]] || { echo "missing $SMOKE; run just e2e-build" >&2; exit 1; }

ssh "${SSH_OPTS[@]}" "root@${VM_IP}" 'uci export network >/tmp/ora-network.uci; uci export dhcp >/tmp/ora-dhcp.uci; uci export firewall >/tmp/ora-firewall.uci; uci export system >/tmp/ora-system.uci; uci export dropbear >/tmp/ora-dropbear.uci; uci export orouteragent >/tmp/ora-agent.uci'
trap 'ssh "${SSH_OPTS[@]}" "root@${VM_IP}" "/etc/init.d/orouteragent stop || true; uci import network </tmp/ora-network.uci; uci commit network; uci import dhcp </tmp/ora-dhcp.uci; uci commit dhcp; uci import firewall </tmp/ora-firewall.uci; uci commit firewall; uci import system </tmp/ora-system.uci; uci commit system; uci import dropbear </tmp/ora-dropbear.uci; uci commit dropbear; uci import orouteragent </tmp/ora-agent.uci; uci commit orouteragent; /etc/init.d/network reload; /etc/init.d/dnsmasq reload; /etc/init.d/firewall reload; /etc/init.d/sysntpd reload; /etc/init.d/dropbear reload; /etc/init.d/orouteragent start || true"' EXIT

ssh "${SSH_OPTS[@]}" "root@${VM_IP}" 'uci add_list orouteragent.agent.managed_domains=network; uci add_list orouteragent.agent.managed_domains=dhcp; uci add_list orouteragent.agent.managed_domains=firewall; uci add_list orouteragent.agent.managed_domains=system; uci commit orouteragent'
ssh "${SSH_OPTS[@]}" "root@${VM_IP}" 'cat >/tmp/orouteragent-config-smoke; chmod 700 /tmp/orouteragent-config-smoke; /tmp/orouteragent-config-smoke network dhcp firewall system' < "$SMOKE"
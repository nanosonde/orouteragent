#!/usr/bin/env bash
# Install (or reinstall) the agent package in the running VM and restart
# it. Used after rebuilding to iterate without recreating the VM.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VM_IP="${E2E_OPENWRT_IP:-172.28.0.50}"
CONTROLLER_IP="${E2E_CONTROLLER_IP:-172.28.0.10}"
AGENT_MODEL="${E2E_AGENT_MODEL:-ER707-M2}"
SSH_KEY="${E2E_SSH_KEY:-${HERE}/openwrt/ssh/id_ed25519}"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i "$SSH_KEY")

apk_file="$(ls -1 "${HERE}"/artifacts/orouteragent-*.apk 2>/dev/null | head -n1 || true)"
if [[ -z "$apk_file" ]]; then
    echo "[install] no package in ${HERE}/artifacts - run 'just e2e-build' first" >&2
    exit 1
fi

echo "[install] copying $(basename "$apk_file") to ${VM_IP}"
# dropbear ships no sftp-server, so stream the file over ssh instead of scp
ssh "${SSH_OPTS[@]}" "root@${VM_IP}" "cat > /tmp/$(basename "$apk_file")" < "$apk_file"

ssh "${SSH_OPTS[@]}" "root@${VM_IP}" AGENT_MODEL="$AGENT_MODEL" \
    CONTROLLER_IP="$CONTROLLER_IP" APK="/tmp/$(basename "$apk_file")" 'sh -s' <<'REMOTE'
set -e
/etc/init.d/orouteragent stop 2>/dev/null || true
apk del orouteragent 2>/dev/null || true
apk add --allow-untrusted "$APK"

uci -q batch <<UCI
set orouteragent.agent.enabled='1'
set orouteragent.agent.model='${AGENT_MODEL}'
set orouteragent.agent.hw_version='1.0'
set orouteragent.agent.controller='${CONTROLLER_IP}'
set orouteragent.agent.log_level='3'
commit orouteragent
UCI

/etc/init.d/orouteragent enable
/etc/init.d/orouteragent restart
sleep 2
echo "--- agent status ---"
pgrep -a orouteragentd || echo "orouteragentd is NOT running"
echo "--- uci ---"
uci show orouteragent | sed 's/^/  /'
REMOTE

echo "[install] done - follow logs with 'just e2e-agent-logs'"
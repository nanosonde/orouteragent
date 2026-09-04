#!/bin/sh
# Provide the isolated E2E WAN with IPv4 addressing and a local next hop.
set -eu

ISP_ADDRESS="${ISP_ADDRESS:-192.0.2.2}"
ISP_PREFIX="${ISP_PREFIX:-24}"
WAN_INTERFACE="${WAN_INTERFACE:-br1}"
WAN_CONTAINER_IP="${WAN_CONTAINER_IP:-192.0.2.3}"
DHCP_START="${DHCP_START:-192.0.2.100}"
DHCP_END="${DHCP_END:-192.0.2.199}"
DHCP_NETMASK="${DHCP_NETMASK:-255.255.255.0}"
DHCP_LEASE_TIME="${DHCP_LEASE_TIME:-12h}"

for address in "$ISP_ADDRESS" "$DHCP_START" "$DHCP_END"; do
    case "$address" in
        *[!0-9.]* | .* | *.)
            echo "[isp-gateway] invalid IPv4 address: $address" >&2
            exit 1
            ;;
    esac
done

echo "[isp-gateway] waiting for ${WAN_INTERFACE}"
until ip link show dev "$WAN_INTERFACE" >/dev/null 2>&1; do
    sleep 1
done
ip address del "${WAN_CONTAINER_IP}/${ISP_PREFIX}" dev "$WAN_INTERFACE" 2>/dev/null || true
ip address replace "${ISP_ADDRESS}/${ISP_PREFIX}" dev "$WAN_INTERFACE"
ip address add "${WAN_CONTAINER_IP}/${ISP_PREFIX}" dev "$WAN_INTERFACE"

cat > /etc/dnsmasq.conf <<EOF
port=0
interface=${WAN_INTERFACE}
bind-interfaces
dhcp-authoritative
dhcp-range=${DHCP_START},${DHCP_END},${DHCP_NETMASK},${DHCP_LEASE_TIME}
dhcp-option=option:router,${ISP_ADDRESS}
dhcp-option=option:dns-server
log-dhcp
log-facility=-
pid-file=/run/dnsmasq.pid
dhcp-leasefile=/var/lib/misc/dnsmasq.leases
EOF

echo "[isp-gateway] serving IPv4 DHCP ${DHCP_START}-${DHCP_END} via ${ISP_ADDRESS}"
exec dnsmasq --keep-in-foreground --conf-file=/etc/dnsmasq.conf

# OpenWrt x86-64 in a container, running as a real VM under qemu/KVM.
#
# Approach borrowed from AlbrechtL/openwrt-docker: full system
# virtualisation with OpenWrt's own kernel rather than a userspace
# container, so procd, uci, ubus and netlink all behave like they do on
# real hardware. Networking differs deliberately: instead of macvtap on
# a physical NIC, the VM is bridged onto the container's docker network
# so it sits on the same L2 segment as the lab controller.
FROM alpine:3.20

RUN apk add --no-cache \
        qemu-system-x86_64 \
        qemu-img \
        iproute2 \
        bridge-utils \
        e2fsprogs \
        e2fsprogs-extra \
        util-linux \
        curl \
        gzip \
        openssh-client \
        socat \
        tini

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
COPY seed-image.sh /usr/local/bin/seed-image.sh
RUN chmod +x /usr/local/bin/entrypoint.sh /usr/local/bin/seed-image.sh

VOLUME /data
EXPOSE 2323

ENTRYPOINT ["/sbin/tini", "--", "/usr/local/bin/entrypoint.sh"]
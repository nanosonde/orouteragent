# orouteragent - source build
#
# Built by the OpenWrt SDK package build (make package/orouteragent/compile).
# There is intentionally NO host build: libuci/libubus/libubox/libblobmsg-json
# are OpenWrt-only libraries, so host compilation is not representative.
# The SDK provides the toolchain, staging-area includes and target libs.
#
# Target binary: orouteragentd

CC       ?= gcc
CFLAGS   ?= -O2 -g
CSTD      = -std=c11 -D_GNU_SOURCE
WARN      = -Wall -Wextra -Wshadow -Wpointer-arith -Wno-unused-parameter

LDLIBS = -lmbedtls -lmbedx509 -lmbedcrypto -ljson-c -lubus -lubox -lblobmsg_json -luci -lmnl -lpthread

INC  = -I. -Iprotocol -Imodel -Iservices

SRCS_C = main.c config.c state.c util.c system_info.c inform.c netlink.c \
         config/registry.c config/common.c config/uci.c config/uci_util.c \
         config/network.c config/dhcp.c config/firewall.c config/system.c \
         config/dropbear.c \
         protocol/buf.c protocol/framing.c protocol/base64.c \
         protocol/auth.c protocol/message.c protocol/rtty_proto.c \
         protocol/dmp_proto.c protocol/pcap.c \
         model/profiles.c \
         services/tls.c services/controller_info.c services/discovery.c \
         services/manage.c services/rtty.c services/dmp.c \
         services/capture.c services/transfer.c

OBJS = $(SRCS_C:.c=.o)
CONFIG_SMOKE_SRCS = config_smoke.c config.c util.c \
                    config/registry.c config/common.c config/uci.c \
                    config/uci_util.c config/network.c config/dhcp.c \
                    config/firewall.c config/system.c config/dropbear.c \
                    protocol/buf.c protocol/message.c model/profiles.c

all: orouteragentd

orouteragentd: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

orouteragent-config-smoke: $(CONFIG_SMOKE_SRCS)
	$(CC) $(CFLAGS) $(CSTD) $(WARN) $(INC) -o $@ $(CONFIG_SMOKE_SRCS) $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(CSTD) $(WARN) $(INC) -c -o $@ $<

clean:
	rm -f orouteragentd orouteragent-config-smoke $(OBJS)

.PHONY: all clean
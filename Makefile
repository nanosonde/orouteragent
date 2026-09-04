include $(TOPDIR)/rules.mk

PKG_NAME:=orouteragent
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
PKG_MAINTAINER:=orouteragent project

PKG_BUILD_DEPENDS:=

# Opt in to the build's job server; without this every source file in
# Build/Compile below would be compiled one at a time.
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/orouteragent
  SECTION:=net
  CATEGORY:=Network
	TITLE:=TP-L*nk Om*d* gateway agent for OpenWrt
  DEPENDS:=+libmbedtls +libjson-c +libubus +libubox +libblobmsg-json +libuci +libmnl
endef

define Package/orouteragent/description
	Presents this OpenWrt router to a TP-L*nk Om*d* Software Controller
	(v5.15/v6.2, ECSP v2) as a managed Om*d* gateway/router. Implements
  UDP discovery, the TLS adoption handshake, periodic INFORM with real
  router state, SET/GET handling, and the controller Tools (remote
  terminal, network check, packet capture). The emulated model (default
  ER707-M2; ER605/ER706W/ER7206/ER707-M2/ER8411) is selectable via UCI.
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) $(PKG_JOBS) -C $(PKG_BUILD_DIR) \
		$(TARGET_CONFIGURE_OPTS) \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		OPENWRT=1
endef

define Package/orouteragent/conffiles
/etc/config/orouteragent
endef

define Package/orouteragent/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/orouteragentd $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/orouteragent.config $(1)/etc/config/orouteragent
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/orouteragent.init $(1)/etc/init.d/orouteragent
	$(INSTALL_DIR) $(1)/etc/orouteragent
endef

$(eval $(call BuildPackage,orouteragent))
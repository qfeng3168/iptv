# OpenWrt 包 Makefile(放入 SDK package/ 目录后: make package/iptv-helper/compile V=s)
include $(TOPDIR)/rules.mk

PKG_NAME:=iptv-helper
PKG_VERSION:=1.1.0
PKG_RELEASE:=5
PKG_MAINTAINER:=qfeng3168
PKG_LICENSE:=MIT

include $(INCLUDE_DIR)/package.mk

define Package/iptv-helper
  SECTION:=net
  CATEGORY:=Network
  TITLE:=IPTV EPG/M3U generator and RTSP replay proxy
  DEPENDS:=+libstdcpp +libc
endef

define Package/luci-app-iptv-helper
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI config page for iptv-helper
  DEPENDS:=+luci-base +iptv-helper
endef

define Package/iptv-helper/description
  Fetch EPG (auth + channel list + playbill), generate M3U/TXT/EPG files for
  uhttpd download, and provide an RTSP catch-up (playseek) replay proxy.
  All parameters come from /etc/config/iptv-helper (no hardcoding).
endef

# 配置文件必须声明为 conffiles:否则 opkg 升级时会用包内默认值覆盖它,
# 用户填的鉴权表单 / EPG 地址 / 边缘服务器等全部丢失。
# 声明后升级保留原配置,新默认值落在 /etc/config/iptv-helper-opkg 供对照合并。
define Package/iptv-helper/conffiles
/etc/config/iptv-helper
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) $(CURDIR)/src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CXX) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -std=c++17 -O2 \
		-o $(PKG_BUILD_DIR)/iptv-helper $(PKG_BUILD_DIR)/iptv-helper.cpp -pthread
endef

define Package/iptv-helper/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/iptv-helper $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) $(CURDIR)/files/iptv-helper.config $(1)/etc/config/iptv-helper
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) $(CURDIR)/files/iptv-helper.init $(1)/etc/init.d/iptv-helper
	$(INSTALL_DIR) $(1)/www/iptv/logo
endef

# 安装即自启并启动服务
define Package/iptv-helper/postinst
#!/bin/sh
[ -n "$$IPKG_INSTROOT" ] || {
	/etc/init.d/iptv-helper enable
	/etc/init.d/iptv-helper start
}
exit 0
endef

define Package/luci-app-iptv-helper/install
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci
	$(CP) $(CURDIR)/luci/usr/lib/lua/luci/* $(1)/usr/lib/lua/luci/
endef

$(eval $(call BuildPackage,iptv-helper))
$(eval $(call BuildPackage,luci-app-iptv-helper))

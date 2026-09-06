-- LuCI 配置页: /cgi-bin/luci/admin/services/iptv-helper
local m, s, o
local sys = require "luci.sys"

m = Map("iptv-helper", translate("IPTV Helper"),
	translate("EPG 抓取生成 M3U/节目单 + RTSP 回看代理。所有参数保存于 /etc/config/iptv-helper。生成物在 /www/iptv,由 uhttpd 提供下载。"))

s = m:section(NamedSection, "main", "iptv-helper", translate("IPTV Helper"))
s.addremove = false
s.tab("svc", translate("服务"))
s.tab("gen", translate("基本与生成"))
s.tab("auth", translate("鉴权(抓包填写)"))
s.tab("net", translate("服务器与发布地址"))
s.tab("files", translate("输出文件"))

-- ============ 服务 ============
local autostart = sys.init.enabled("iptv-helper")
local running = (sys.exec("pidof iptv-helper 2>/dev/null") ~= "")

o = s:taboption("svc", DummyValue, "_status", translate("当前状态"))
o.value = running and "● 运行中" or "○ 未运行(开机自启:" .. (autostart and "已开启" or "未开启") .. ")"

o = s:taboption("svc", Flag, "_autostart", translate("开机自启"))
o.cfgvalue = function(self, section)
	return autostart and "1" or "0"
end
o.write = function(self, section, value)
	if value == "1" then
		sys.init.enable("iptv-helper")
	else
		sys.init.disable("iptv-helper")
	end
end
o.remove = function(self, section) end

btn = s:taboption("svc", Button, "_start", translate("启动服务"))
btn.inputtitle = translate("启动")
btn.write = function(self, section)
	sys.call("/etc/init.d/iptv-helper start >/dev/null 2>&1")
end

btn = s:taboption("svc", Button, "_stop", translate("停止服务"))
btn.inputtitle = translate("停止")
btn.write = function(self, section)
	sys.call("/etc/init.d/iptv-helper stop >/dev/null 2>&1")
end

btn = s:taboption("svc", Button, "_restart", translate("重启服务"))
btn.inputtitle = translate("重启")
btn.write = function(self, section)
	sys.call("/etc/init.d/iptv-helper restart >/dev/null 2>&1")
end

-- ============ 基本与生成 ============
o = s:taboption("gen", ListValue, "mode", translate("运行模式(procd 服务)"))
o:value("daemon", translate("代理 + 定时生成(推荐)"))
o:value("proxy", translate("仅 RTSP 代理"))

o = s:taboption("gen", DynamicList, "schedule", translate("定时生成时间"))
o.placeholder = "01:00"

o = s:taboption("gen", Value, "out_dir", translate("输出目录(uhttpd 根下)"))
o.default = "/www/iptv"

o = s:taboption("gen", Flag, "fetch_logo", translate("抓取台标"))
o.default = "1"

o = s:taboption("gen", Value, "history_days", translate("节目单回看天数"))
o.datatype = "uinteger"
o.default = "6"

o = s:taboption("gen", Value, "playbill_rounds", translate("节目单请求轮数"))
o.datatype = "uinteger"
o.default = "8"

o = s:taboption("gen", Flag, "lan_catchup", translate("LanReplay 带回看时间戳"))
o.default = "1"

o = s:taboption("gen", ListValue, "replay_via", translate("内网回看地址"))
o:value("proxy", translate("走本机代理(推荐)"))
o:value("direct", translate("直连边缘服务器"))

btn = s:taboption("gen", Button, "_run_generate", translate("立即生成"))
btn.inputtitle = translate("生成(后台执行,需数分钟)")
btn.write = function(self, section)
	sys.call("/usr/bin/iptv-helper -c /etc/config/iptv-helper generate >/tmp/iptv-helper-generate.log 2>&1 &")
end

-- ============ 鉴权 ============
local auth_keys = {
	{"UserID", "UserID"}, {"Authenticator", "Authenticator"},
	{"STBType", "STBType"}, {"STBVersion", "STBVersion"},
	{"STBID", "STBID"}, {"templateName", "templateName"},
	{"areaId", "areaId"}, {"userToken", "userToken"},
	{"mac", "mac"}, {"SoftwareVersion", "SoftwareVersion"},
	{"NetUserID", "NetUserID"}, {"desktopId", "desktopId"},
	{"stbmaker", "stbmaker"}, {"ChipID", "ChipID"}, {"VIP", "VIP"},
}
for _, kv in ipairs(auth_keys) do
	o = s:taboption("auth", Value, kv[1], translate(kv[2]))
	o.rmempty = true
end

-- ============ 服务器与发布地址 ============
o = s:taboption("net", Value, "epg_host", translate("EPG 服务器地址"))
o.datatype = "host"
o.rmempty = false

o = s:taboption("net", Value, "epg_port", translate("EPG 服务器端口"))
o.datatype = "port"
o.default = "80"

o = s:taboption("net", Value, "edge_host", translate("RTSP 边缘服务器"))
o.datatype = "host"

o = s:taboption("net", Value, "edge_port", translate("RTSP 边缘端口"))
o.datatype = "port"
o.default = "554"

o = s:taboption("net", Value, "rtsp_port", translate("本机回看代理端口"))
o.datatype = "port"
o.default = "554"

o = s:taboption("net", Value, "udpxy_lan", translate("内网直播基址(udpxy)"))
o.placeholder = "http://127.0.0.1:4022"

o = s:taboption("net", Value, "udpxy_pub", translate("外网直播基址(udpxy)"))

o = s:taboption("net", Value, "replay_pub", translate("外网回看基址"))
o.placeholder = "rtsp://your.domain:554"

o = s:taboption("net", Value, "http_pub_base", translate("文件对外访问基址"))
o.placeholder = "http://your.domain"

o = s:taboption("net", Value, "lan_ip", translate("本机 LAN IP(留空自动探测)"))
o.datatype = "ipaddr"

-- ============ 输出文件 ============
local file_keys = {
	{"lanlive_file", "LanLive 文件名", "LanLive.m3u"},
	{"lanreplay_file", "LanReplay 文件名", "LanReplay.m3u"},
	{"netlive_file", "NetLive 文件名", "NetLive.m3u"},
	{"netreplay_file", "NetReplay 文件名", "NetReplay.m3u"},
	{"txt_file", "TXT 文件名", "channels.txt"},
	{"epg_file", "EPG 文件名", "PL.xml"},
}
for _, kv in ipairs(file_keys) do
	o = s:taboption("files", Value, kv[1], translate(kv[2]))
	o.default = kv[3]
end

o = s:taboption("files", Value, "epg_name", translate("EPG 名称"))
o.default = "IPTV EPG"

o = s:taboption("files", Value, "catchup_fmt", translate("catchup 时间格式"))
o.default = "yyyyMMddHHmmss"

return m

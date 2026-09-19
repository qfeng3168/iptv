-- LuCI 配置页: /cgi-bin/luci/admin/services/iptv-helper
local m, s, o
local sys = require "luci.sys"

m = Map("iptv-helper", translate("IPTV Helper"),
	translate("EPG 抓取生成 M3U/节目单 + RTSP 回看代理。所有参数保存于 /etc/config/iptv-helper。生成物在 /www/iptv,由 uhttpd 提供下载。"))

s = m:section(NamedSection, "main", "iptv-helper", translate("IPTV Helper"))
s.addremove = false
s:tab("svc", translate("服务"))
s:tab("gen", translate("基本与生成"))
s:tab("auth", translate("鉴权(抓包填写)"))
s:tab("net", translate("服务器与发布地址"))
s:tab("files", translate("输出文件"))

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

o = s:taboption("gen", Flag, "cache_logo", translate("台标缓存到本地"),
	translate("下载到 <输出目录>/<台标子目录>/,m3u 的 tvg-logo 指向本地缓存;失败时回退远端地址"))
o.default = "1"

o = s:taboption("gen", Value, "logo_dir", translate("台标子目录"))
o.default = "logo"
o.datatype = "maxlength(64)"
o:depends("cache_logo", "1")
o:depends("fetch_logo", "1")

o = s:taboption("gen", Flag, "logo_reuse", translate("复用已缓存台标"),
	translate("已存在则不重复下载(关闭后每次生成都重新下载)"))
o.default = "1"
o:depends("cache_logo", "1")
o:depends("fetch_logo", "1")

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
-- 注意:NetUserID / desktopId / stbmaker / ChipID / VIP 不再在本页显示,取值固定为空。
--       它们在 auth_body() 中仍以空值随鉴权表单提交(字段存在但值为空)。
--       若某平台确实需要非空值,请改代码;手工 uci set 会在本页保存时被重写覆盖。
local auth_keys = {
	{"UserID", "UserID"}, {"Authenticator", "Authenticator"},
	{"STBType", "STBType"}, {"STBVersion", "STBVersion"},
	{"STBID", "STBID"}, {"templateName", "templateName"},
	{"areaId", "areaId"}, {"userToken", "userToken"},
	{"mac", "mac"}, {"SoftwareVersion", "SoftwareVersion"},
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
local http = require "luci.http"
local out_dir = m.uci:get("iptv-helper", "main", "out_dir") or "/www/iptv"
-- web 路径 = out_dir 去掉 uhttpd 根(/www)前缀
local webpath = out_dir:gsub("^/www", "", 1)
if webpath == "" then webpath = "/" end
local pub = m.uci:get("iptv-helper", "main", "http_pub_base")
local base
if pub and pub ~= "" then
	base = pub:gsub("/$", "")
else
	local host = http.getenv("HTTP_HOST") or (m.uci:get("iptv-helper", "main", "lan_ip") or "")
	base = "http://" .. host
end
local function file_url(name)
	return base .. webpath .. "/" .. name
end

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
	local dv = s:taboption("files", DummyValue, "_" .. kv[1], translate("下载地址"))
	dv.cfgvalue = function(self, section)
		local name = m.uci:get("iptv-helper", section, kv[1]) or kv[3]
		local url = file_url(name)
		if kv[1] == "epg_file" then url = url .. ".gz" end
		return url
	end
end

o = s:taboption("files", Value, "epg_name", translate("EPG 名称"))
o.default = "IPTV EPG"

o = s:taboption("gen", ListValue, "catchup_type", translate("catchup 类型(旧式裸模板用)"),
	translate("append = 回看(参数追加在频道 URL 后,RTSP PLTV 用);shift = 时移(秒级时间戳);" ..
		"default = HTTP/HLS(UTC + T);flussonic = 开始 + 时长;custom = 完全按模板原样输出。" ..
		"仅用于包装旧式裸模板与留空时的兜底;片段自带属性名时以片段为准"))
o:value("append", "append(回看,推荐)")
o:value("shift", "shift(时移)")
o:value("default", "default(HTTP/HLS)")
o:value("flussonic", "flussonic(开始+时长)")
o:value("custom", "custom(完全按模板)")
o.default = "append"

o = s:taboption("gen", Value, "catchup_fmt", translate("catchup 时间格式(兜底)"),
	translate("仅当 catchup 属性片段留空、回落到类型默认模板时生效;" ..
		"仅 append / shift / custom 会用到该格式(它们的时间串由本项拼出)," ..
		"default / flussonic 的默认模板是固定写法,不读此项。" ..
		"切换 catchup 类型后旧值仍保留在此项,如需重置请手动清空"))
o.default = "yyyyMMddHHmmss"
o.datatype = "maxlength(64)"
o:depends("catchup_type", "append")
o:depends("catchup_type", "shift")
o:depends("catchup_type", "custom")

o = s:taboption("gen", DynamicList, "catchup_params", translate("回看 / 时移 属性片段"),
	translate("每项原样写入 #EXTINF,片段之间用单个空格连接。" ..
		"回看与时移是两套并列语义,属性名必须分开写(catchup / shift),否则同一行出现重名属性会互相顶掉:" ..
		"catchup=\"append\" catchup-source=\"?playseek=${(b)yyyyMMddHHmmss}-${(e)yyyyMMddHHmmss}\" ;" ..
		"shift=\"append\" shift-source=\"?starttime=${(b)yyyyMMddHHmmss}-${(e)yyyyMMddHHmmss}\"。" ..
		"两项同时保留即可让播放器同时提供回看与时移;想只留一种,删掉对应那一项。" ..
		"也可只写裸模板(如 playseek=${(b)yyyyMMddHHmmss}-${(e)yyyyMMddHHmmss}),按 catchup 类型自动包装。" ..
		"常用占位符: ${(b)格式}/${(e)格式}(结尾加 |UTC 转 UTC)、" ..
		"{utc:格式}/{utcend:格式}、{start}/{end}、${timestamp}/${end_timestamp}/${duration}(秒)。" ..
		"留空则按类型取默认片段"))
o.datatype = "maxlength(512)"

o = s:taboption("gen", Value, "catchup_days", translate("可回看天数声明 catchup-days"),
	translate("0~365;留空或填 0 则不写入"))
o.datatype = "uinteger, range(0,365)"

o = s:taboption("gen", Value, "timeshift_days", translate("可时移天数声明 timeshift"),
	translate("0~365;留空或填 0 则不写入。部分播放器读此字段"))
o.datatype = "uinteger, range(0,365)"

o = s:taboption("gen", Value, "catchup_correction", translate("时间偏移 catchup-correction"),
	translate("如 -10800 表示减 3 小时;仅接受整数,留空不写入"))
o.datatype = "integer, maxlength(16)"

return m

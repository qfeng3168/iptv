-- LuCI 控制器: admin/services/iptv-helper
module("luci.controller.iptv-helper", package.seeall)

function index()
	if not nixio.fs.access("/etc/config/iptv-helper") then
		return
	end
	local page = entry({"admin", "services", "iptv-helper"}, cbi("iptv-helper"), _("IPTV Helper"), 60)
	page.dependent = true
end

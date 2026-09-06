# iptv-helper (OpenWrt)

IPTV 助手:EPG 鉴权抓取 → 生成 M3U/TXT/EPG 节目单 → RTSP 回看(playseek)代理。
生成物放到 `/www/iptv`,由 OpenWrt 自带 **uhttpd** 直接提供下载;定时生成走
**cron 或内置调度**;服务由 **procd** 托管;日志进 **syslog**。代码零硬编码,
全部参数在 `/etc/config/iptv-helper`。

> ✅ **已实测通过:河北电信 IPTV**(华为 HWServer/HMS_V1R2 平台,320 频道,
> 7 天节目单抓取、直播、RTSP 回看全部验证可用)。

## 参考 / 致谢

- [xinjiawei1/heiptv](https://github.com/xinjiawei1/heiptv) — 电信 IPTV 模拟与 EPJ 抓取原理(鉴权表单、
  `getchannellistHWCTC.jsp`、`QueryPlaybillList` 等接口用法)参考该项目及其 Python 实现
- 回看 URL 格式为实测结论:频道 smil 地址 + `?playseek=YYYYMMDDHHMMSS-YYYYMMDDHHMMSS`
  (路径后缀式 `/起-止` 在华为 HMS 上为 404)
- 华为边缘服务器 302 跳转媒体节点、仅对 `RTP/AVP/TCP;interleaved` 出流等行为,
  均为对河北电信现网实测得出

## 功能

| 模式 | 说明 |
|---|---|
| `generate` | 抓取一次:鉴权 → 频道列表 → 台标 → 8 天节目单,生成 LanLive/LanReplay/NetLive/NetReplay.m3u、channels.txt、PL.xml(.gz) |
| `proxy` | RTSP 回看代理(监听 rtsp_port):302 跟随、强制 TCP interleaved、UDP 客户端重封装、`?playseek=` 回看 |
| `daemon`(默认) | 代理线程 + 按配置 schedule 定时生成(默认 01:00/13:00,启动即生成一次) |

生成的 NetReplay.m3u 带 `catchup="append" catchup-source="?playseek=${(b)yyyyMMddHHmmss}-${(e)yyyyMMddHHmmss}"`,
配合 PL.xml 节目单,APTV/TiviMate 可直接点播 7 天内历史节目。

## 编译(GitHub Actions 自动)

push 后 Actions 会用 OpenWrt 官方 SDK 交叉编译出 ipk( targets:x86_64 /
armsr-armv8 / aarch64_cortex-a53 / mt7621),tag(`v*`)推送时自动发 Release。
本地 SDK 编译:

```sh
cp -r iptv-helper <sdk>/package/
cd <sdk> && make defconfig && make package/iptv-helper/compile V=s
```

## 安装与配置

```sh
opkg install iptv-helper_*.ipk luci-app-iptv-helper_*.ipk
# 安装即自启。LuCI: 网络 → IPTV Helper
#   「服务」页: 开机自启开关、启动/停止/重启
#   其余页: EPG 地址、鉴权表单(抓包)、边缘服务器、udpxy、定时
#   「基本与生成」页点「立即生成」
```

生成物下载地址(uhttpd):
- `http://<路由器>/iptv/LanLive.m3u`、`LanReplay.m3u`
- `http://<路由器>/iptv/NetReplay.m3u`、`PL.xml.gz`、`channels.txt`

## 回看说明

- 回看 URL:频道 smil 地址 + `?playseek=YYYYMMDDHHMMSS-YYYYMMDDHHMMSS`(实测本平台支持,路径后缀式 404)
- 服务器只对 RTP/AVP/TCP 出流 → 代理上游一律强制 TCP interleaved,客户端 UDP/TCP 均可
- 华为边缘机 302 跳转媒体节点,部分播放器不跟随 → 代理代为跟随并全局缓存节点票据(5 分钟)
- 外网回看:把路由器 554 端口映射到本机,`replay_pub` 填 `rtsp://你的域名:554`
- 防火墙记得放行 554:`uci add firewall rule` 或 luci 添加(允许多播不涉及,单播 TCP)

## 日志

`logread | grep iptv-helper`

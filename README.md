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
- 回看 URL 格式为实测结论:频道 smil 地址 + `?playseek=YYYYMMDDHHMMss-YYYYMMDDHHmmss`
  (路径后缀式 `/起-止` 在华为 HMS 上为 404)
- [SrcBox 回看与时移指南](https://srcbox.top/guide/catchup-timeshift) — 仅作为**时间占位符写法**的
  参考(`${(b)格式}`、`|UTC`、`{utc:格式}`、`${timestamp}`/`${duration}` 等);属性名与「回看 / 时移
  并列」的拼装规则由本项目自定义(见下文 catchup 一节),不依赖第三方约定
- 华为边缘服务器 302 跳转媒体节点、仅对 `RTP/AVP/TCP;interleaved` 出流等行为,
  均为对河北电信现网实测得出

## 功能

| 模式 | 说明 |
|---|---|
| `generate` | 抓取一次:鉴权 → 频道列表 → 台标(缓存到 `/www/iptv/logo`) → 8 天节目单,生成 LanLive/LanReplay/NetLive/NetReplay.m3u、channels.txt、PL.xml(.gz) |
| `proxy` | RTSP 回看代理(监听 rtsp_port):302 跟随、强制 TCP interleaved、UDP 客户端重封装、`?playseek=` 回看 |
| `daemon`(默认) | 代理线程 + 按配置 schedule 定时生成(默认 01:00/13:00,启动即生成一次) |

生成的 NetReplay.m3u 带 catchup 属性,默认在同一行并列回看与时移两组(**属性名分开**,单空格分隔):

```
catchup="append" catchup-source="?playseek=${(b)yyyyMMddHHmmss}-${(e)yyyyMMddHHmmss}" shift="append" shift-source="?starttime=${(b)yyyyMMddHHmmss}-${(e)yyyyMMddHHmmss}"
```

> 回看与时移是两套并列语义,属性名必须不同(`catchup` / `shift`)。两处都写 `catchup=`
> 会在同一行产生**重名属性**,后者顶掉前者(旧版因此把 `playseek` 顶成了 `starttime`)。

配合 PL.xml 节目单,APTV/TiviMate 可直接点播 7 天内历史节目。

## 台标缓存

默认把抓到的台标下载到 `<out_dir>/<logo_dir>`(即 `/www/iptv/logo/<UserChannelID>.<png|jpg|gif>`),
m3u 里的 `tvg-logo` 指向本地缓存地址(基址 = `http_pub_base`,留空则用本机 LAN IP),
播放器不再依赖 EPG 服务器出图、内网离线也能显示台标。下载失败或响应内容不是图片时,
回退为该频道的远端原始台标地址。`logo_reuse=1` 时已缓存的文件不重复下载。

> `http_pub_base` 填**站点根**(如 `http://192.168.1.1`),web 路径由 `out_dir` 去掉 `/www` 前缀得出;
> 若已写成 `http://192.168.1.1/iptv` 也不会重复拼接。留空则用探测到的本机 LAN IP。
> 台标写在 `/www`(overlay)下,320 频道约占几 MB。`logo_dir` 是 `out_dir` 下的子目录名,
> 需位于 uhttpd 根内播放器才能访问。

## 回看 / 时移(catchup)

播放器只做一件事:把 URL 模板里的**时间占位符**替换成具体时间串并播放。
回看 = 从 EPG 节目单点一个已播节目(用节目的起止时间);时移 = 直播中拖动进度条回退(用游标时间)。
两种语义可以在**同一个 `#EXTINF` 行内并列**:回看写 `catchup` / `catchup-source`,
时移写 `shift` / `shift-source`,片段之间只用**单个空格**连接,不使用 `" or "`。
默认即并列 `append`(回看,`playseek`)与 `shift`(时移,`starttime`)两套。

> 两组的**属性名必须不同**,否则同一行会出现重名属性、后者顶掉前者。
> 想只保留一种语义,把 `catchup_params` 里对应那一项删掉即可。

| 配置 | 作用 |
|---|---|
| `catchup_params` | 每项是一段**原样写入 `#EXTINF` 的属性片段**,片段间用单个空格连接;留空按类型取兜底片段 |
| `catchup_type` | 仅用于包装「旧式裸模板」与留空时的兜底:`append` 回看(默认,参数追加在频道 URL 后) / `shift` 时移 / `default`(HTTP/HLS,UTC+T) / `flussonic`(开始+时长) / `custom` |
| `catchup_days` / `timeshift_days` | 可回看 / 可时移天数的声明属性(留空不写) |
| `catchup_correction` | 时间偏移声明(如 `-10800`) |

模板的两种写法(可混用):

- **完整属性片段**(含 `="`):原样写入 `#EXTINF`,每项自成一组。要「回看 + 时移并列」就用这种写法
- **旧式裸模板**(不含 `="`,如 `playseek=…`):按 `catchup_type` 包装,且多个裸模板会
  **合并回单条** `catchup="<类型>" catchup-source="?a or ?b"`(与 1.0.0 产物逐字节一致)

时间占位符(按类型默认模板如下,也可自行改写):

| 占位符 | 含义 | 兜底片段 |
|---|---|---|
| `${(b)格式}` / `${(e)格式}` | 开始 / 结束,本地时间;格式串为 .NET 格式 | `append`: `catchup="append" catchup-source="?playseek=${(b)yyyyMMddHHmmss}-${(e)yyyyMMddHHmmss}"` |
| `${(b)格式\|UTC}` | 同上但按 UTC 输出 | `default`: `catchup="default" catchup-source="?starttime=${(b)yyyyMMdd\|UTC}T${(b)HHmmss\|UTC}&endtime=${(e)yyyyMMdd\|UTC}T${(e)HHmmss\|UTC}"` |
| `{utc:格式}` / `{utcend:格式}` | 固定 UTC 写法 | — |
| `{start}` / `{end}` | 本地固定 `yyyyMMddHHmmss` | — |
| `${timestamp}` / `${end_timestamp}` | 开始 / 结束的秒级 Unix 时间戳 | `shift`: `shift="shift" shift-source="?starttime=${(b)yyyyMMddHHmmss}-${(e)yyyyMMddHHmmss}"` |
| `${duration}` | 时长(秒) | `flussonic`: `catchup="flussonic" catchup-source="?start=${timestamp}&duration=${duration}"` |

- 本平台(HMS/河北电信)实测回看:`catchup="append"` + `?playseek=YYYYMMDDHHmmss-YYYYMMDDHHmmss`;
  时移:`shift="append"` + `?starttime=YYYYMMDDHHmmss-YYYYMMDDHHmmss`
- 升级兼容:旧配置里若写的是裸模板(如 `playseek=…`、`starttime=…`),多个裸模板会合并回
  单条 `catchup="<类型>" catchup-source="?a or ?b"`,与 1.0.0 产物逐字节一致;要启用
  「回看 + 时移并列」需写成带属性名的完整片段(默认配置已是这种写法)
- 排错:回放地址里仍能看到 `${(b)...}`/`{utc:...}` 原文 → 该频道没带上回放属性模板,
  或播放器没走回看/时移流程;地址生成对但播不了 → 源端不认这个参数名或时间格式(换 `playseek`
  与 `starttime`、换本地时间与 UTC 试试)


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
- 台标:`http://<路由器>/iptv/logo/<UserChannelID>.png`

### 从 1.0.0 升级(注意配置合并)

`/etc/config/iptv-helper` 已声明为 `conffiles`,升级**不会**覆盖你已有的配置,
包内新默认值会落在 `/etc/config/iptv-helper-opkg` 供对照。

但正因为不覆盖,旧配置里的 `catchup_params` 是两行裸模板,按兼容规则仍会合并成
旧的单条 `catchup="append" catchup-source="?… or ?…"`,**不会自动出现 `shift=` 时移属性**。
要启用「回看 + 时移并列」,把这两项换成新写法:

```sh
uci -q delete iptv-helper.catchup_params
uci add_list iptv-helper.catchup_params='catchup="append" catchup-source="?playseek=${(b)yyyyMMddHHmmss}-${(e)yyyyMMddHHmmss}"'
uci add_list iptv-helper.catchup_params='shift="append" shift-source="?starttime=${(b)yyyyMMddHHmmss}-${(e)yyyyMMddHHmmss}"'
uci commit iptv-helper && /etc/init.d/iptv-helper restart
```

(或直接对照 `/etc/config/iptv-helper-opkg` 里的对应两行。)

> 1.0.0-1 及更早的 ipk **没有**声明 conffiles,升级会把配置整份覆盖成默认值 ——
> 从 1.1.0-2 起已修正;若从更早版本升级,请先手工备份 `/etc/config/iptv-helper`。

## 回看说明

- 回看 URL:频道 smil 地址 + `?playseek=YYYYMMDDHHMMSS-YYYYMMDDHHMMSS`(实测本平台支持,路径后缀式 404)
- 服务器只对 RTP/AVP/TCP 出流 → 代理上游一律强制 TCP interleaved,客户端 UDP/TCP 均可
- 华为边缘机 302 跳转媒体节点,部分播放器不跟随 → 代理代为跟随并全局缓存节点票据(5 分钟)
- 外网回看:把路由器 554 端口映射到本机,`replay_pub` 填 `rtsp://你的域名:554`
- 防火墙记得放行 554:`uci add firewall rule` 或 luci 添加(允许多播不涉及,单播 TCP)

## 日志

`logread | grep iptv-helper`

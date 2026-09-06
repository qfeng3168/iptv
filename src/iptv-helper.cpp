// iptv-helper.cpp — OpenWrt IPTV 助手
// 功能: EPG鉴权抓取 -> 生成 M3U/TXT/EPG(PL.xml.gz) -> RTSP 回看代理
// 下载走 OpenWrt 自带 uhttpd(生成物写入 /www/iptv),定时走 cron 或内置调度,
// 守护由 procd 管理,日志走 syslog。所有参数来自 /etc/config/iptv-helper,代码零硬编码。
//
// 用法:
//   iptv-helper generate   抓取一次并生成全部文件
//   iptv-helper proxy      仅 RTSP 回看代理(前台,配合 procd)
//   iptv-helper daemon     代理线程 + 内置定时生成(默认方式)
//   iptv-helper -c <path>  指定配置文件(默认 /etc/config/iptv-helper)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <cstdarg>
#include <csignal>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>
#include <regex>
#include <algorithm>
#include <functional>

#include <syslog.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#  define CLOSESOCK closesocket
#  define SOCKOPT const char
#  define SOCKLEN int
#  define SHUT_RDWR 2
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  define CLOSESOCK close
#  define SOCKOPT void
#  define SOCKLEN socklen_t
#endif

typedef long long ll;
static std::atomic<bool> g_run{true};

// ---------------------------------------------------------------- 基础工具
static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
static std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }
static bool starts_with(const std::string &s, const std::string &p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }

static std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out; size_t a = 0;
    while (true) {
        size_t b = s.find(sep, a);
        if (b == std::string::npos) { out.push_back(s.substr(a)); break; }
        out.push_back(s.substr(a, b - a)); a = b + 1;
    }
    return out;
}

static void replace_all(std::string &s, const std::string &f, const std::string &t) {
    if (f.empty()) return;
    size_t p = 0;
    while ((p = s.find(f, p)) != std::string::npos) { s.replace(p, f.size(), t); p += t.size(); }
}

static std::string url_encode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

// epoch 毫秒 -> "YYYYmmddHHMMSS +0800"(用本机时区,路由器需为 Asia/Shanghai)
static std::string ms_to_shift(ll ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", &tmv);
    return std::string(buf) + " +0800";
}
static std::string now_str() {
    time_t t = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

static void log_i(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    syslog(LOG_INFO, "%s", buf);
    fprintf(stderr, "%s %s\n", now_str().c_str(), buf);
    fflush(stderr);
}
static void log_e(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    syslog(LOG_ERR, "%s", buf);
    fprintf(stderr, "%s [ERR] %s\n", now_str().c_str(), buf);
    fflush(stderr);
}

// ---------------------------------------------------------------- 配置(UCI 风格,零硬编码)
struct Config {
    std::map<std::string, std::string> kv;
    void load(const std::string &path) {
        FILE *f = fopen(path.c_str(), "r");
        if (!f) { log_e("cannot open config %s", path.c_str()); return; }
        char line[8192];
        while (fgets(line, sizeof(line), f)) {
            std::string s = trim(line);
            if (s.empty() || s[0] == '#' || starts_with(s, "config ")) continue;
            if (starts_with(s, "option ") || starts_with(s, "list ")) {
                bool islist = starts_with(s, "list ");
                s = trim(s.substr(islist ? 5 : 7));
                size_t sp = s.find(' ');
                if (sp == std::string::npos) continue;
                std::string key = trim(s.substr(0, sp)), val = trim(s.substr(sp + 1));
                if (val.size() >= 2 && val.front() == '\'' && val.back() == '\'') val = val.substr(1, val.size() - 2);
                if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
                if (islist) {
                    auto it = kv.find(key);
                    kv[key] = (it == kv.end() ? "" : it->second + " ") + val;
                } else kv[key] = val;
            }
        }
        fclose(f);
        log_i("config loaded: %s (%d keys)", path.c_str(), (int)kv.size());
    }
    std::string get(const std::string &k, const std::string &def = "") const {
        auto it = kv.find(k);
        return it == kv.end() ? def : it->second;
    }
    int geti(const std::string &k, int def) const {
        auto it = kv.find(k);
        return it == kv.end() ? def : atoi(it->second.c_str());
    }
    bool getb(const std::string &k, bool def) const {
        auto it = kv.find(k);
        return it == kv.end() ? def : (it->second == "1" || it->second == "true" || it->second == "yes");
    }
    std::vector<std::string> getlist(const std::string &k) const {
        std::vector<std::string> out;
        for (auto &s : split(get(k), ' ')) if (!trim(s).empty()) out.push_back(trim(s));
        return out;
    }
};

static Config g_cfg;

// ---------------------------------------------------------------- socket 小工具
static void sock_set_timeout(int fd, int seconds) {
    struct timeval tv; tv.tv_sec = seconds; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (SOCKOPT *)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (SOCKOPT *)&tv, sizeof(tv));
}

static int tcp_connect(const char *host, int port, int timeout_sec) {
    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    char portstr[16]; snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
#ifdef _WIN32
    u_long nb = 1; ioctlsocket(fd, FIONBIO, &nb);
#else
    int flags = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    if (rc != 0) {
        fd_set w; FD_ZERO(&w); FD_SET(fd, &w);
        struct timeval tv; tv.tv_sec = timeout_sec; tv.tv_usec = 0;
        if (select(fd + 1, nullptr, &w, nullptr, &tv) <= 0) { CLOSESOCK(fd); freeaddrinfo(res); return -1; }
        int err = 0; SOCKLEN elen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (SOCKOPT *)&err, &elen);
        if (err) { CLOSESOCK(fd); freeaddrinfo(res); return -1; }
    }
#ifdef _WIN32
    u_long nb0 = 0; ioctlsocket(fd, FIONBIO, &nb0);
#else
    fcntl(fd, F_SETFL, flags);
#endif
    sock_set_timeout(fd, timeout_sec);
    freeaddrinfo(res);
    return fd;
}

static bool send_all(int fd, const char *d, size_t n) {
    size_t off = 0;
    while (off < n) {
        int k = (int)send(fd, d + off, (int)(n - off), 0);
        if (k <= 0) return false;
        off += (size_t)k;
    }
    return true;
}
static bool send_all(int fd, const std::string &s) { return send_all(fd, s.data(), s.size()); }

static std::string local_ip_of(int fd) {
    struct sockaddr_storage ss; SOCKLEN sl = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0) return "127.0.0.1";
    char buf[64] = {0};
    if (ss.ss_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, buf, sizeof(buf));
    else inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, buf, sizeof(buf));
    return buf;
}
// 本机出口 IP(用于生成指向本机代理的 URL)
static std::string detect_lan_ip(const Config &cfg) {
    std::string probe = cfg.get("lan_probe_host", "223.5.5.5");
    int fd = tcp_connect(probe.c_str(), cfg.geti("lan_probe_port", 53), 2);
    if (fd < 0) return "127.0.0.1";
    std::string ip = local_ip_of(fd);
    CLOSESOCK(fd);
    return ip;
}

// ---------------------------------------------------------------- HTTP 客户端(带 cookie)
struct HttpResp { int status = 0; std::string body; };

class HttpClient {
public:
    std::map<std::string, std::string> cookies;
    HttpResp post(const std::string &host, int port, const std::string &path,
                  const std::string &body, const std::string &ctype, int timeout = 60) {
        std::string req = "POST " + path + " HTTP/1.1\r\n";
        req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
        if (!ctype.empty()) req += "Content-Type: " + ctype + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        std::string ck = cookie_header();
        if (!ck.empty()) req += "Cookie: " + ck + "\r\n";
        req += "Connection: close\r\n\r\n" + body;
        return roundtrip(host, port, req, timeout);
    }
private:
    std::string cookie_header() {
        std::string o;
        for (auto &c : cookies) { if (!o.empty()) o += "; "; o += c.first + "=" + c.second; }
        return o;
    }
    HttpResp roundtrip(const std::string &host, int port, const std::string &req, int timeout) {
        HttpResp r;
        int fd = tcp_connect(host.c_str(), port, timeout);
        if (fd < 0) { log_e("http connect %s:%d failed", host.c_str(), port); return r; }
        if (!send_all(fd, req)) { CLOSESOCK(fd); return r; }
        std::string raw, buf(65536, '\0');
        while (raw.size() < (64u << 20)) {
            int k = (int)recv(fd, &buf[0], buf.size(), 0);
            if (k <= 0) break;
            raw.append(buf, 0, (size_t)k);
        }
        CLOSESOCK(fd);
        size_t he = raw.find("\r\n\r\n");
        if (he == std::string::npos) return r;
        std::string head = raw.substr(0, he), rest = raw.substr(he + 4);
        if (sscanf(head.c_str(), "HTTP/%*d.%*d %d", &r.status) != 1) r.status = 0;
        size_t p = 0;
        std::string lhead = lower(head);
        while ((p = lhead.find("set-cookie:", p)) != std::string::npos) {
            size_t nl = head.find('\n', p); if (nl == std::string::npos) nl = head.size();
            std::string line = trim(head.substr(p + 11, nl - p - 11));
            size_t semi = line.find(';');
            std::string kvpair = trim(semi == std::string::npos ? line : line.substr(0, semi));
            size_t eq = kvpair.find('=');
            if (eq != std::string::npos) cookies[trim(kvpair.substr(0, eq))] = trim(kvpair.substr(eq + 1));
            p = nl;
        }
        bool chunked = lhead.find("transfer-encoding: chunked") != std::string::npos;
        size_t clp = lhead.find("content-length:");
        if (chunked) {
            std::string out;
            size_t pos = 0;
            while (pos < rest.size()) {
                size_t e = rest.find("\r\n", pos); if (e == std::string::npos) break;
                long len = strtol(rest.substr(pos, e - pos).c_str(), nullptr, 16);
                if (len <= 0) break;
                if (e + 2 + (size_t)len > rest.size()) break;
                out.append(rest, e + 2, (size_t)len);
                pos = e + 2 + (size_t)len + 2;
            }
            r.body = out;
        } else if (clp != std::string::npos) {
            long cl = strtol(trim(head.substr(clp + 15, head.find('\n', clp) - clp - 15)).c_str(), nullptr, 10);
            r.body = rest.substr(0, rest.size() < (size_t)cl ? rest.size() : (size_t)cl);
        } else r.body = rest;
        return r;
    }
};

// ---------------------------------------------------------------- EPG 数据模型
struct Channel { std::string id, name, ucid, logo, group, rtsp, igmp; };
struct Prog { ll start = 0, end = 0; std::string name; };

static std::string group_of(const std::string &ucid) {
    long n = atol(ucid.c_str());
    if (n < 20) return "央视台";
    if (n < 130) return "河北省台";
    if (n < 200) return "河北市级台";
    if (n < 400) return "全国卫视";
    if (n < 500) return "数字标清";
    if (n < 600) return "数字高清";
    if (n < 700) return "数字直播";
    if (n < 900) return "县级台";
    return "其他";
}
static bool need_reauth(const std::string &ucid) {
    return ucid == "1" || ucid == "101" || ucid == "201" || ucid == "401" ||
           ucid == "501" || ucid == "601" || ucid == "701" || ucid == "801" || ucid == "901";
}

static std::string auth_body(const Config &cfg) {
    // 表单字段与值全部来自配置文件
    const char *fields[] = {"UserID","Lang","SupportHD","NetUserID","Authenticator","STBType",
        "STBVersion","conntype","STBID","templateName","areaId","userToken","userGroupId",
        "productPackageId","mac","UserField","SoftwareVersion","IsSmartStb","desktopId",
        "stbmaker","XMPPCapability","ChipID","VIP", nullptr};
    std::string body;
    for (int i = 0; fields[i]; ++i) {
        if (i) body += "&";
        body += fields[i];
        body += "=" + url_encode(cfg.get(fields[i]));
    }
    return body;
}

static void do_auth(HttpClient &hc, const Config &cfg) {
    auto r = hc.post(cfg.get("epg_host"), cfg.geti("epg_port", 80),
                     cfg.get("auth_path", "/EPG/jsp/ValidAuthenticationHWCTC.jsp"),
                     auth_body(cfg), "application/x-www-form-urlencoded");
    log_i("auth status=%d", r.status);
}

static std::vector<Channel> parse_channels(const std::string &t) {
    std::vector<Channel> out;
    size_t pos = 0;
    while (true) {
        size_t cs = t.find("ChannelID=\"", pos);
        if (cs == std::string::npos) break;
        size_t ce = t.find('"', cs + 11);
        if (ce == std::string::npos) break;
        Channel ch;
        ch.id = t.substr(cs + 11, ce - cs - 11);
        auto grab = [&](const std::string &key, size_t from, std::string &dst) -> size_t {
            size_t k = t.find(key + "=\"", from);
            if (k == std::string::npos) return std::string::npos;
            k += key.size() + 2;
            size_t e = t.find('"', k);
            if (e == std::string::npos) return std::string::npos;
            dst = t.substr(k, e - k);
            return e;
        };
        size_t p3 = grab("ChannelName", ce, ch.name);
        if (p3 == std::string::npos) { pos = ce; continue; }
        size_t p4 = grab("UserChannelID", p3, ch.ucid);
        if (p4 == std::string::npos) { pos = p3; continue; }
        std::string url;
        size_t p5 = grab("ChannelURL", p4, url);
        if (p5 == std::string::npos) { pos = p4; continue; }
        size_t gi = url.find("igmp://");
        if (gi != std::string::npos) {
            size_t e = url.find_first_of(" \t|", gi);
            ch.igmp = url.substr(gi + 7, (e == std::string::npos ? url.size() : e) - gi - 7);
        }
        size_t ri = url.find("rtsp://");
        if (ri != std::string::npos) {
            size_t e = url.find("smil", ri);
            if (e != std::string::npos) ch.rtsp = url.substr(ri, e + 4 - ri);
        }
        if (!ch.rtsp.empty() && !ch.igmp.empty()) {
            ch.group = group_of(ch.ucid);
            out.push_back(ch);
        }
        pos = p5;
    }
    return out;
}

static std::string fetch_logo(HttpClient &hc, const Config &cfg, const std::string &channel_id, ll today_zero) {
    char body[512];
    snprintf(body, sizeof(body),
        "{\"queryChannel\":{\"channelIDs\":[\"%s\"],\"offset\":0},"
        "\"queryPlaybillContext\":{\"date\":%lld,\"type\":1,\"preNumber\":1,\"nextNumber\":3}}",
        channel_id.c_str(), today_zero);
    auto r = hc.post(cfg.get("epg_host"), cfg.geti("epg_port", 80),
                     cfg.get("logo_path", "/VSP/V3/QueryPlaybillContext"), body,
                     "application/x-www-form-urlencoded; charset=UTF-8");
    std::regex re("(http://[^\"^\\s]*\\.(png|jpg|gif))");
    std::smatch m;
    if (std::regex_search(r.body, m, re)) return m[1].str();
    return "";
}

static void parse_playbill(const std::string &t, std::vector<Prog> &out) {
    size_t pos = 0;
    while (true) {
        size_t s = t.find("\"startTime\":\"", pos);
        if (s == std::string::npos) break;
        s += 13;
        size_t e = t.find('"', s);
        if (e == std::string::npos) break;
        Prog pg; pg.start = atoll(t.substr(s, e - s).c_str());
        size_t n = t.find("\"name\":\"", e);
        size_t e2 = t.find("\"endTime\":\"", e);
        if (e2 == std::string::npos) break;
        if (n != std::string::npos && n < e2) {
            size_t ne = t.find('"', n + 8);
            if (ne != std::string::npos) {
                pg.name = t.substr(n + 8, ne - n - 8);
                replace_all(pg.name, "<", "《");
                replace_all(pg.name, ">", "》");
            }
        }
        size_t ev = t.find('"', e2 + 11);
        if (ev == std::string::npos) break;
        pg.end = atoll(t.substr(e2 + 11, ev - e2 - 11).c_str());
        out.push_back(pg);
        pos = ev;
    }
}

static ll today_zero_ms() {
    time_t t = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    tmv.tm_hour = 0; tmv.tm_min = 0; tmv.tm_sec = 0;
    return (ll)mktime(&tmv) * 1000;
}

static std::vector<std::vector<Prog>> fetch_playbills(HttpClient &hc, const Config &cfg, const Channel &ch) {
    std::vector<std::vector<Prog>> days;
    ll limittime = today_zero_ms() - (ll)cfg.geti("history_days", 6) * 86400000LL;
    int rounds = cfg.geti("playbill_rounds", 8);
    for (int i = 0; i < rounds; ++i) {
        char body[512];
        snprintf(body, sizeof(body),
            "{\"queryChannel\":{\"channelIDs\":[\"%s\"]},"
            "\"queryPlaybill\":{\"type\":\"0\",\"startTime\":%lld,\"endTime\":%lld,"
            "\"count\":\"100\",\"offset\":\"0\",\"isFillProgram\":\"0\",\"mustIncluded\":\"0\"},"
            "\"needChannel\":\"0\"}",
            ch.id.c_str(), limittime, limittime + 86400000LL);
        auto r = hc.post(cfg.get("epg_host"), cfg.geti("epg_port", 80),
                         cfg.get("playbill_path", "/VSP/V3/QueryPlaybillList"), body,
                         "application/x-www-form-urlencoded; charset=UTF-8");
        std::vector<Prog> day;
        parse_playbill(r.body, day);
        days.push_back(day);
        limittime += 86400000LL;
    }
    return days;
}

// ---------------------------------------------------------------- 文件生成
static bool write_file(const std::string &path, const std::string &data) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { log_e("write %s failed", path.c_str()); return false; }
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    log_i("written %s (%d bytes)", path.c_str(), (int)data.size());
    return true;
}

static std::string extinf(const Channel &ch, const std::string &extra) {
    return "#EXTINF:-1 tvg-id=\"" + ch.ucid + "\" tvg-name=\"" + ch.name + "\" tvg-logo=\"" +
           ch.logo + "\" group-title=\"" + ch.group + "\"" + extra + ", " + ch.name + "\n";
}

static std::string smil_path(const Channel &ch) {
    size_t p = ch.rtsp.find("/PLTV/");
    return p == std::string::npos ? "" : ch.rtsp.substr(p);
}

static std::string gen_live_m3u(const std::vector<Channel> &chs, const std::string &base) {
    std::string o = "#EXTM3U\n";
    for (auto &ch : chs) {
        o += extinf(ch, "");
        o += base + "/udp/" + ch.igmp + "\n";
    }
    return o;
}

static std::string gen_replay_m3u(const Config &cfg, const std::vector<Channel> &chs,
                                  const std::string &base, bool catchup) {
    std::string o = "#EXTM3U\n";
    std::string extra;
    if (catchup) {
        // catchup-source 支持多源,播放器按 " or " 顺序尝试:
        // 默认 playseek + starttime 两种时移参数,模板全部来自配置
        std::vector<std::string> params = cfg.getlist("catchup_params");
        if (params.empty()) {
            std::string fmt = cfg.get("catchup_fmt", "yyyyMMddHHmmss");
            params.push_back("playseek=${(b)" + fmt + "}-${(e)" + fmt + "}");
        }
        std::string src;
        for (size_t i = 0; i < params.size(); ++i) {
            src += (i ? " or ?" : "?") + params[i];
        }
        extra = " catchup=\"append\" catchup-source=\"" + src + "\"";
    }
    for (auto &ch : chs) {
        std::string sp = smil_path(ch);
        if (sp.empty()) continue;
        o += extinf(ch, extra);
        o += base + sp + "\n";
    }
    return o;
}

static std::string gen_txt(const Config &cfg, const std::vector<Channel> &chs,
                           const std::string &lan_ip, const std::string &rtsp_self) {
    std::string o;
    o += "IPTV channel list  generated: " + now_str() + "\n";
    o += "live(lan):  " + cfg.get("udpxy_lan") + "/udp/<multicast>\n";
    o += "replay(lan): " + rtsp_self + "<smil_path>[?playseek=YYYYMMDDHHMMSS-YYYYMMDDHHMMSS]\n";
    o += "epg: " + cfg.get("http_pub_base") + "/" + cfg.get("epg_file", "PL.xml") + ".gz\n";
    o += std::string(72, '-') + "\n";
    int n = 1;
    for (auto &ch : chs) {
        char line[1024];
        snprintf(line, sizeof(line), "%3d | %-18s | %-10s | udp/%s | %s\n",
                 n++, ch.name.c_str(), ch.group.c_str(), ch.igmp.c_str(), smil_path(ch).c_str());
        o += line;
    }
    (void)lan_ip;
    return o;
}

static std::string gen_epg_xml(const Config &cfg, const std::vector<Channel> &chs,
                               const std::map<std::string, std::vector<std::vector<Prog>>> &pb) {
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    o += "<tv info-name=\"" + cfg.get("epg_name", "IPTV EPG") + "\" info-url=\"\">\n";
    for (auto &ch : chs) {
        o += "<channel id=\"" + ch.ucid + "\">\n<display-name lang=\"zh\">" + ch.name + "</display-name>\n</channel>\n";
        auto it = pb.find(ch.id);
        if (it == pb.end()) continue;
        for (auto &day : it->second)
            for (auto &pg : day)
                o += "<programme channel=\"" + ch.ucid + "\" start=\"" + ms_to_shift(pg.start) +
                     "\" stop=\"" + ms_to_shift(pg.end) + "\">\n<title lang=\"zh\">" + pg.name +
                     "</title>\n</programme>\n";
    }
    o += "</tv>";
    return o;
}

// gzip(stored 模式,零依赖)
static uint32_t crc32_of(const std::string &d) {
    static uint32_t tab[256]; static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : d) c = tab[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static std::string gzip_store(const std::string &in) {
    std::string o = "\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff";
    size_t pos = 0;
    while (pos < in.size()) {
        size_t n = std::min<size_t>(65535, in.size() - pos);
        bool last = (pos + n >= in.size());
        o += (char)(last ? 1 : 0);
        o += (char)(n & 0xFF); o += (char)(n >> 8);
        o += (char)(~n & 0xFF); o += (char)((~n >> 8) & 0xFF);
        o.append(in, pos, n);
        pos += n;
    }
    o += "\x01\x00\x00\xff\xff";
    uint32_t crc = crc32_of(in), isz = (uint32_t)in.size();
    for (int i = 0; i < 4; ++i) o += (char)(crc >> (8 * i));
    for (int i = 0; i < 4; ++i) o += (char)(isz >> (8 * i));
    return o;
}

// ---------------------------------------------------------------- 抓取+生成 主流程
static bool run_generate(const Config &cfg) {
    std::string epg_host = cfg.get("epg_host");
    int epg_port = cfg.geti("epg_port", 80);
    if (epg_host.empty()) { log_e("epg_host not configured"); return false; }

    log_i("generate start");
    HttpClient hc;
    do_auth(hc, cfg);
    auto list_resp = hc.post(epg_host, epg_port,
                             cfg.get("channellist_path", "/EPG/jsp/getchannellistHWCTC.jsp"), "",
                             "application/x-www-form-urlencoded");
    if (list_resp.status != 200 || list_resp.body.size() < 1000) {
        log_e("channellist failed, status=%d size=%d", list_resp.status, (int)list_resp.body.size());
        return false;
    }
    std::vector<Channel> chs = parse_channels(list_resp.body);
    log_i("channels parsed: %d", (int)chs.size());
    if (chs.empty()) return false;

    if (cfg.getb("fetch_logo", true)) {
        ll tz = today_zero_ms();
        for (size_t i = 0; i < chs.size(); ++i) {
            if (need_reauth(chs[i].ucid)) do_auth(hc, cfg);
            chs[i].logo = fetch_logo(hc, cfg, chs[i].id, tz);
            if ((i % 50) == 49) log_i("logo %d/%d", (int)i + 1, (int)chs.size());
        }
    }

    std::map<std::string, std::vector<std::vector<Prog>>> playbills;
    for (size_t i = 0; i < chs.size(); ++i) {
        if (need_reauth(chs[i].ucid)) do_auth(hc, cfg);
        playbills[chs[i].id] = fetch_playbills(hc, cfg, chs[i]);
        if ((i % 50) == 49) log_i("playbill %d/%d", (int)i + 1, (int)chs.size());
    }

    std::string dir = cfg.get("out_dir", "/www/iptv");
    std::string lan_ip = cfg.get("lan_ip");
    if (lan_ip.empty()) lan_ip = detect_lan_ip(cfg);
    std::string rtsp_self = "rtsp://" + lan_ip + ":" + std::to_string(cfg.geti("rtsp_port", 554));
    std::string lan_replay = cfg.get("replay_via", "proxy") == "direct"
        ? "rtsp://" + cfg.get("edge_host") + ":" + std::to_string(cfg.geti("edge_port", 554))
        : rtsp_self;

    write_file(dir + "/" + cfg.get("lanlive_file", "LanLive.m3u"), gen_live_m3u(chs, cfg.get("udpxy_lan")));
    // 内网回看同样带 catchup(playseek)时间戳,配合节目单可点播回放
    write_file(dir + "/" + cfg.get("lanreplay_file", "LanReplay.m3u"), gen_replay_m3u(cfg, chs, lan_replay, cfg.getb("lan_catchup", true)));
    write_file(dir + "/" + cfg.get("netlive_file", "NetLive.m3u"), gen_live_m3u(chs, cfg.get("udpxy_pub")));
    write_file(dir + "/" + cfg.get("netreplay_file", "NetReplay.m3u"), gen_replay_m3u(cfg, chs, cfg.get("replay_pub"), true));
    write_file(dir + "/" + cfg.get("txt_file", "channels.txt"), gen_txt(cfg, chs, lan_ip, rtsp_self));
    std::string xml = gen_epg_xml(cfg, chs, playbills);
    write_file(dir + "/" + cfg.get("epg_file", "PL.xml"), xml);
    write_file(dir + "/" + cfg.get("epg_file", "PL.xml") + ".gz", gzip_store(xml));
    log_i("generate done");
    return true;
}

// ---------------------------------------------------------------- RTSP 回看代理(v5 逻辑)
static std::mutex g_node_mtx;
static std::map<std::string, std::pair<std::string, time_t>> g_node_cache;
static const time_t NODE_TTL = 300;
static std::atomic<long> g_sess_counter{1};

static std::string cache_get(const std::string &k) {
    std::lock_guard<std::mutex> lk(g_node_mtx);
    auto it = g_node_cache.find(k);
    if (it != g_node_cache.end() && time(nullptr) - it->second.second < NODE_TTL) return it->second.first;
    return "";
}
static void cache_put(const std::string &k, const std::string &v) {
    std::lock_guard<std::mutex> lk(g_node_mtx);
    g_node_cache[k] = std::make_pair(v, time(nullptr));
}

struct RtspSess {
    std::atomic<int> client{-1};
    std::atomic<int> upfd{-1};
    std::string cbuf, ubuf;
    std::string client_base, path_query;
    std::string node_url, node_session, our_session;
    std::string up_host;          // 当前 upfd 连接的目标(连接复用判断)
    int up_port = 0;
    int udp_rtp = -1, udp_rtcp = -1;
    int rtp_port = 0, rtcp_port = 0;
    uint32_t client_addr4 = 0;
    bool udp = false;
    std::string local_ip;
    std::mutex up_mtx; // 上游 fd 读写保护
};

using SessPtr = std::shared_ptr<RtspSess>;

static bool read_rtsp(int fd, std::string &buf, std::string &msg, int timeout) {
    sock_set_timeout(fd, timeout);
    while (buf.find("\r\n\r\n") == std::string::npos) {
        char tmp[8192];
        int k = (int)recv(fd, tmp, sizeof(tmp), 0);
        if (k <= 0) return false;
        buf.append(tmp, (size_t)k);
        if (buf.size() > 131072) return false;
    }
    size_t he = buf.find("\r\n\r\n");
    std::string head = buf.substr(0, he);
    size_t cl = 0;
    {
        std::string lh = lower(head);
        size_t p = lh.find("content-length:");
        if (p != std::string::npos) {
            size_t nl = head.find('\n', p);
            if (nl == std::string::npos) nl = head.size();
            cl = (size_t)strtoul(trim(head.substr(p + 15, nl - p - 15)).c_str(), nullptr, 10);
        }
    }
    while (buf.size() < he + 4 + cl) {
        char tmp[8192];
        int k = (int)recv(fd, tmp, sizeof(tmp), 0);
        if (k <= 0) return false;
        buf.append(tmp, (size_t)k);
    }
    msg = buf.substr(0, he + 4 + cl);
    buf.erase(0, he + 4 + cl);
    return true;
}

static std::string first_line_of(const std::string &m) {
    size_t p = m.find("\r\n");
    return p == std::string::npos ? m : m.substr(0, p);
}
static int code_of(const std::string &m) {
    int code = 0;
    sscanf(m.c_str(), "RTSP/%*d.%*d %d", &code);
    return code;
}
static std::string authority_of(const std::string &url) {
    size_t a = url.find("rtsp://");
    if (a == std::string::npos) return "";
    size_t b = url.find('/', a + 7);
    return b == std::string::npos ? url.substr(a + 7) : url.substr(a + 7, b - a - 7);
}
static bool host_port_of(const std::string &url, std::string &host, int &port, int def_port) {
    size_t a = url.find("rtsp://");
    if (a == std::string::npos) return false;
    a += 7;
    size_t b = url.find('/', a);
    std::string auth = b == std::string::npos ? url.substr(a) : url.substr(a, b - a);
    size_t c = auth.find(':');
    if (c == std::string::npos) { host = auth; port = def_port; }
    else { host = auth.substr(0, c); port = atoi(auth.c_str() + c + 1); }
    return !host.empty();
}

struct RtspProxy {
    std::string edge_host;
    int edge_port = 554;

    // 连接上游;目标(host:port)未变时复用现有连接以保持节点会话。
    // track_host/track_port 记录 fd 当前连接的目标:主会话 fd 用 session 字段,
    // resolve() 的临时 fd 传局部变量,避免污染复用判断。
    bool upstream_connect(SessPtr &s, int &fd, std::string &ubuf, const std::string &target,
                          std::string *track_host, int *track_port) {
        std::string host; int port;
        if (!host_port_of(target, host, port, 554)) return false;
        if (fd >= 0 && host == *track_host && port == *track_port) return true; // 复用
        int nfd = tcp_connect(host.c_str(), port, 15);
        if (nfd < 0) return false;
        if (fd >= 0) CLOSESOCK(fd);
        fd = nfd; ubuf.clear();
        *track_host = host; *track_port = port;
        log_i("  upstream -> %s:%d", host.c_str(), port);
        return true;
    }

    std::string build_req(SessPtr &s, const std::string &msg, const std::string &method, const std::string &target) {
        std::string out = msg;
        size_t a = out.find(' ');
        if (a != std::string::npos) {
            size_t b = out.find(' ', a + 1);
            if (b != std::string::npos) out.replace(a + 1, b - a - 1, target);
        }
        if (!s->node_session.empty() && method != "DESCRIBE") {
            std::regex re("Session:[^\r\n]*", std::regex::icase);
            out = std::regex_replace(out, re, "Session: " + s->node_session);
        }
        if (method == "SETUP") {
            std::regex re("Transport:[^\r\n]*", std::regex::icase);
            out = std::regex_replace(out, re, "Transport: RTP/AVP/TCP;interleaved=0-1");
        }
        return out;
    }

    // DESCRIBE 走边缘解析 302(只更新 node_url/缓存),独立 fd
    bool resolve(SessPtr &s) {
        int fd = -1;
        std::string ubuf, rhost;
        int rport = 0;
        std::string target = "rtsp://" + edge_host + ":" + std::to_string(edge_port) + s->path_query;
        int hops = 0;
        while (hops <= 4) {
            if (!upstream_connect(s, fd, ubuf, target, &rhost, &rport)) { if (fd >= 0) CLOSESOCK(fd); return false; }
            std::string req = "DESCRIBE " + target + " RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n";
            if (!send_all(fd, req)) { CLOSESOCK(fd); return false; }
            std::string resp;
            if (!read_rtsp(fd, ubuf, resp, 15)) { CLOSESOCK(fd); return false; }
            int code = code_of(resp);
            if (code == 301 || code == 302 || code == 303 || code == 307) {
                std::string lresp = lower(resp);
                size_t p = lresp.find("location:");
                if (p == std::string::npos) { CLOSESOCK(fd); return false; }
                size_t vs = resp.find(' ', p), ve = resp.find("\r\n", p);
                if (vs == std::string::npos || ve == std::string::npos || ve <= vs) { CLOSESOCK(fd); return false; }
                s->node_url = trim(resp.substr(vs, ve - vs));
                cache_put(lower(s->path_query), s->node_url);
                CLOSESOCK(fd); fd = -1;
                target = s->node_url;
                hops++;
                continue;
            }
            CLOSESOCK(fd);
            return true;
        }
        return true;
    }

    std::string location_of(const std::string &resp) {
        std::string lresp = lower(resp);
        size_t p = lresp.find("location:");
        if (p == std::string::npos) return "";
        size_t vs = resp.find(' ', p), ve = resp.find("\r\n", p);
        if (vs == std::string::npos || ve == std::string::npos || ve <= vs) return "";
        return trim(resp.substr(vs, ve - vs));
    }

    // 发请求并跟随 302;fd 在 s->upfd
    std::string follow(SessPtr &s, const std::string &msg, const std::string &method, bool force_edge) {
        if (force_edge) {
            std::lock_guard<std::mutex> lk(s->up_mtx);
            if (s->upfd >= 0) { CLOSESOCK(s->upfd); s->upfd = -1; }
            s->node_url.clear(); s->node_session.clear();
        } else if (method != "DESCRIBE" && s->node_url.empty() && cache_get(lower(s->path_query)).empty()) {
            resolve(s);
            if (!s->node_url.empty()) log_i("  pre-resolved node: %.90s", s->node_url.c_str());
        }
        std::string target = force_edge
            ? "rtsp://" + edge_host + ":" + std::to_string(edge_port) + s->path_query
            : (s->node_url.empty()
                ? "rtsp://" + edge_host + ":" + std::to_string(edge_port) + s->path_query
                : s->node_url);
        int hops = 0;
        while (true) {
            {
                std::lock_guard<std::mutex> lk(s->up_mtx);
                std::string ubuf = s->ubuf;
                int fd = s->upfd;
                if (!upstream_connect(s, fd, ubuf, target, &s->up_host, &s->up_port))
                    throw std::runtime_error("upstream connect fail");
                s->upfd = fd; s->ubuf = ubuf;
                std::string req = build_req(s, msg, method, target);
                if (!send_all(s->upfd, req)) throw std::runtime_error("upstream send fail");
            }
            std::string resp;
            {
                std::lock_guard<std::mutex> lk(s->up_mtx);
                if (!read_rtsp(s->upfd, s->ubuf, resp, 15)) throw std::runtime_error("upstream read fail");
            }
            log_i("  upstream %s <- %.50s", method.c_str(), first_line_of(resp).c_str());
            int code = code_of(resp);
            if ((code == 301 || code == 302 || code == 303 || code == 307) && hops < 4) {
                std::string loc = location_of(resp);
                if (loc.empty()) return resp;
                s->node_url = loc;
                cache_put(lower(s->path_query), loc);
                std::lock_guard<std::mutex> lk(s->up_mtx);
                if (s->upfd >= 0) { CLOSESOCK(s->upfd); s->upfd = -1; }
                target = loc;
                hops++;
                continue;
            }
            return resp;
        }
    }

    std::string rewrite_resp(SessPtr &s, const std::string &resp, const std::string &method) {
        std::string out = resp;
        std::string mine = "rtsp://" + s->client_base;
        std::regex urlre("rtsp://[^/\\s\r\n]+");
        std::vector<std::string> hosts;
        auto it = std::sregex_iterator(out.begin(), out.end(), urlre);
        for (; it != std::sregex_iterator(); ++it) hosts.push_back(it->str());
        for (auto &h : hosts)
            if (mine != h) replace_all(out, h, mine);
        if (method == "DESCRIBE") {
            std::regex re("Session:[^\r\n]*\r\n", std::regex::icase);
            out = std::regex_replace(out, re, "");
        }
        if (method == "SETUP") {
            std::smatch m;
            std::regex sre("Session:\\s*([^;\r\n]+)", std::regex::icase);
            if (std::regex_search(out, m, sre)) {
                s->node_session = trim(m[1].str());
                s->our_session = "900" + std::to_string(g_sess_counter++);
                std::regex wre("Session:[^\r\n]*", std::regex::icase);
                out = std::regex_replace(out, wre, "Session: " + s->our_session);
            }
            if (s->udp) {
                int rp = 0, cp = 0;
                struct sockaddr_in sa; memset(&sa, 0, sizeof(sa)); SOCKLEN sl = sizeof(sa);
                if (getsockname(s->udp_rtp, (struct sockaddr *)&sa, &sl) == 0) rp = ntohs(sa.sin_port);
                if (getsockname(s->udp_rtcp, (struct sockaddr *)&sa, &sl) == 0) cp = ntohs(sa.sin_port);
                char tr[256];
                snprintf(tr, sizeof(tr), "Transport: RTP/AVP;unicast;client_port=%d-%d;source=%s;server_port=%d-%d",
                         s->rtp_port, s->rtcp_port, s->local_ip.c_str(), rp, cp);
                std::regex tre("Transport:[^\r\n]*", std::regex::icase);
                out = std::regex_replace(out, tre, tr);
            }
        }
        return out;
    }

    // ---------------- 转发线程 ----------------
    void pump_up_tcp(SessPtr s) {
        char b[65536];
        int fd = s->upfd;
        while (g_run && fd >= 0) {
            int k = (int)recv(fd, b, sizeof(b), 0);
            if (k <= 0) break;
            int c = s->client;
            if (c < 0 || !send_all(c, b, (size_t)k)) break;
        }
        shutdown(s->client, SHUT_RDWR);
    }
    void pump_client_tcp(SessPtr s) {
        char b[65536];
        int fd = s->client;
        while (g_run && fd >= 0) {
            int k = (int)recv(fd, b, sizeof(b), 0);
            if (k <= 0) break;
            s->cbuf.append(b, (size_t)k);
            // 拆分:客户端交织帧($)原样转发;RTSP 文本做会话映射后转发并回传响应
            while (g_run) {
                if (s->cbuf.empty()) break;
                if (s->cbuf[0] == '$') {
                    if (s->cbuf.size() < 4) break;
                    size_t ln = ((unsigned char)s->cbuf[2] << 8) | (unsigned char)s->cbuf[3];
                    if (s->cbuf.size() < 4 + ln) break;
                    std::string frame = s->cbuf.substr(0, 4 + ln);
                    s->cbuf.erase(0, 4 + ln);
                    int u = s->upfd;
                    if (u < 0 || !send_all(u, frame)) return;
                    continue;
                }
                if (s->cbuf.find("\r\n\r\n") == std::string::npos) break;
                std::string msg;
                if (!read_rtsp(fd, s->cbuf, msg, 15)) return;
                sock_set_timeout(fd, 0); // read_rtsp 会改超时,转发期恢复阻塞
                std::string method = trim(msg.substr(0, msg.find(' ')));
                if (method.empty()) return;
                try {
                    std::string resp = follow(s, msg, method, false);
                    resp = rewrite_resp(s, resp, method);
                    if (!send_all(s->client, resp)) return;
                    if (method == "TEARDOWN") return;
                } catch (...) { return; }
            }
        }
        shutdown(s->upfd, SHUT_RDWR);
    }
    void pump_up_udp(SessPtr s) {
        std::string buf;
        char b[65536];
        int fd = s->upfd;
        while (g_run && fd >= 0) {
            int k = (int)recv(fd, b, sizeof(b), 0);
            if (k <= 0) break;
            buf.append(b, (size_t)k);
            struct sockaddr_in da; memset(&da, 0, sizeof(da));
            da.sin_family = AF_INET;
            da.sin_addr.s_addr = s->client_addr4;
            frames_from(buf, [&](int ch, const char *p, size_t n) {
                da.sin_port = htons(ch == 0 ? (uint16_t)s->rtp_port : (uint16_t)s->rtcp_port);
                sendto(ch == 0 ? s->udp_rtp : s->udp_rtcp, p, (int)n, 0, (struct sockaddr *)&da, sizeof(da));
            });
        }
    }
    void pump_client_rtcp(SessPtr s) {
        char b[65536];
        while (g_run && s->udp_rtcp >= 0) {
            int k = (int)recv(s->udp_rtcp, b, sizeof(b), 0);
            if (k < 0) {
                // 超时:回到循环检查 g_run;真错误才退出
#ifdef _WIN32
                if (WSAGetLastError() == WSAETIMEDOUT) continue;
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
#endif
                break;
            }
            if (k <= 0) break;
            std::string frame = "$";
            frame += (char)1;
            frame += (char)((k >> 8) & 0xFF);
            frame += (char)(k & 0xFF);
            frame.append(b, (size_t)k);
            int u = s->upfd;
            if (u < 0 || !send_all(u, frame)) break;
        }
    }

    // UDP 模式:控制连接 keepalive + TEARDOWN(主线程)
    void control_udp(SessPtr s) {
        while (g_run) {
            std::string msg;
            if (!read_rtsp(s->client, s->cbuf, msg, 3600)) {
                if (s->cbuf.empty()) continue;
                break;
            }
            std::string method = trim(msg.substr(0, msg.find(' ')));
            if (method.empty()) continue;
            try {
                std::string resp = follow(s, msg, method, false);
                sock_set_timeout(s->upfd, 0); // 转发期保持阻塞
                resp = rewrite_resp(s, resp, method);
                send_all(s->client, resp);
                if (method == "TEARDOWN") break;
            } catch (...) { break; }
        }
    }

    // ---------------- 单连接处理 ----------------
    void handle(int cfd, uint32_t caddr) {
        SessPtr s = std::make_shared<RtspSess>();
        s->client = cfd;
        s->client_addr4 = caddr;
        log_i("connect from %u.%u.%u.%u", caddr & 0xFF, (caddr >> 8) & 0xFF, (caddr >> 16) & 0xFF, (caddr >> 24) & 0xFF);
        try {
            while (g_run) {
                std::string msg;
                if (!read_rtsp(cfd, s->cbuf, msg, 15)) break;
                std::string first = first_line_of(msg);
                size_t sp1 = first.find(' ');
                size_t sp2 = first.find(' ', sp1 + 1);
                if (sp1 == std::string::npos || sp2 == std::string::npos) break;
                std::string method = first.substr(0, sp1);
                std::string url = first.substr(sp1 + 1, sp2 - sp1 - 1);
                if (url != "*") {
                    if (s->client_base.empty()) s->client_base = authority_of(url);
                    if (s->path_query.empty()) {
                        size_t sl = url.find('/', url.find("rtsp://") + 7);
                        s->path_query = sl == std::string::npos ? "/" : url.substr(sl);
                    }
                    log_i("%s %.100s", method.c_str(), url.c_str());
                }
                if (method == "OPTIONS") {
                    std::smatch m;
                    std::string cs = "0";
                    std::regex re("CSeq:\\s*(\\d+)", std::regex::icase);
                    if (std::regex_search(msg, m, re)) cs = m[1].str();
                    std::string resp = "RTSP/1.0 200 OK\r\nCSeq: " + cs +
                        "\r\nPublic: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, SET_PARAMETER\r\n\r\n";
                    send_all(cfd, resp);
                    log_i("  client OPTIONS <- local 200 OK");
                    continue;
                }
                if (method == "SETUP" && !s->udp) {
                    std::smatch m;
                    std::regex tre("Transport:[^\r\n]*", std::regex::icase);
                    if (std::regex_search(msg, m, tre)) {
                        std::string trline = m[0].str();
                        std::string trl = lower(trline);
                        if (trl.find("rtp/avp/tcp") == std::string::npos && trl.find("interleaved") == std::string::npos) {
                            std::regex pre("client_port=(\\d+)(?:-(\\d+))?", std::regex::icase);
                            std::smatch pm;
                            if (std::regex_search(trline, pm, pre)) {
                                s->udp = true;
                                s->rtp_port = atoi(pm[1].str().c_str());
                                s->rtcp_port = pm[2].matched ? atoi(pm[2].str().c_str()) : s->rtp_port + 1;
                                s->udp_rtp = socket(AF_INET, SOCK_DGRAM, 0);
                                s->udp_rtcp = socket(AF_INET, SOCK_DGRAM, 0);
                                struct sockaddr_in a; memset(&a, 0, sizeof(a));
                                a.sin_family = AF_INET;
                                bind(s->udp_rtp, (struct sockaddr *)&a, sizeof(a));
                                bind(s->udp_rtcp, (struct sockaddr *)&a, sizeof(a));
                                sock_set_timeout(s->udp_rtcp, 5); // 周期醒来检查 g_run
                                s->local_ip = local_ip_of(cfd);
                            }
                        }
                    }
                }
                std::string resp = follow(s, msg, method, method == "DESCRIBE");
                resp = rewrite_resp(s, resp, method);
                send_all(cfd, resp);
                int code = code_of(resp);
                if (method == "PLAY" && code >= 200 && code < 300) {
                    log_i("  relaying (%s)", s->udp ? "udp reframe" : "tcp passthrough");
                    // 转发阶段取消握手期的短超时,恢复阻塞模式,防止静默期断流
                    sock_set_timeout(s->upfd, 0);
                    sock_set_timeout(s->client, 0);
                    if (s->udp) {
                        std::thread t1(&RtspProxy::pump_up_udp, this, s);
                        std::thread t2(&RtspProxy::pump_client_rtcp, this, s);
                        control_udp(s);
                        int u = s->upfd;
                        if (u >= 0) { shutdown(u, SHUT_RDWR); }
                        t1.join(); t2.join();
                    } else {
                        std::thread t1(&RtspProxy::pump_up_tcp, this, s);
                        pump_client_tcp(s);
                        int u = s->upfd;
                        if (u >= 0) shutdown(u, SHUT_RDWR);
                        int c = s->client;
                        if (c >= 0) shutdown(c, SHUT_RDWR);
                        t1.join();
                    }
                    break;
                }
                if (method == "TEARDOWN") break;
            }
        } catch (std::exception &e) {
            log_i("closed (%s)", e.what());
        }
        if (s->upfd >= 0) CLOSESOCK(s->upfd);
        if (s->udp_rtp >= 0) CLOSESOCK(s->udp_rtp);
        if (s->udp_rtcp >= 0) CLOSESOCK(s->udp_rtcp);
        if (s->client >= 0) CLOSESOCK(s->client);
        log_i("disconnect");
    }

    void frames_from(std::string &buf, std::function<void(int, const char *, size_t)> fn) {
        while (buf.size() >= 4) {
            if ((unsigned char)buf[0] != 0x24) { buf.erase(0, 1); continue; }
            size_t ln = ((unsigned char)buf[2] << 8) | (unsigned char)buf[3];
            if (buf.size() < 4 + ln) break;
            fn(buf[1], buf.data() + 4, ln);
            buf.erase(0, 4 + ln);
        }
    }

    void serve(int listen_fd) {
        while (g_run) {
            struct sockaddr_in ca; memset(&ca, 0, sizeof(ca));
            SOCKLEN cl = sizeof(ca);
            int cfd = accept(listen_fd, (struct sockaddr *)&ca, &cl);
            if (cfd < 0) continue;
            uint32_t addr = ntohl(ca.sin_addr.s_addr);
            std::thread(&RtspProxy::handle, this, cfd, addr).detach();
        }
    }
};

static void rtsp_proxy_main(const Config &cfg) {
    RtspProxy px;
    px.edge_host = cfg.get("edge_host");
    px.edge_port = cfg.geti("edge_port", 554);
    int port = cfg.geti("rtsp_port", 554);
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, (SOCKOPT *)&one, sizeof(one));
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = 0; a.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(lfd, 16) != 0) {
        log_e("rtsp proxy bind :%d failed", port);
        return;
    }
    log_i("rtsp proxy listening :%d -> %s:%d", port, px.edge_host.c_str(), px.edge_port);
    px.serve(lfd);
}

// ---------------------------------------------------------------- 内置定时调度
static void scheduler_main(const Config &cfg, const std::vector<std::string> &times) {
    log_i("scheduler started, times:");
    for (auto &t : times) log_i("  %s", t.c_str());
    int last_min = -1;
    while (g_run) {
        time_t t = time(nullptr);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        int cur = tmv.tm_hour * 100 + tmv.tm_min;
        if (cur != last_min) {
            last_min = cur;
            char hhmm[8];
            snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            for (auto &tt : times)
                if (tt == hhmm) {
                    log_i("scheduled run %s", hhmm);
                    run_generate(cfg);
                }
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

// ---------------------------------------------------------------- main
static void on_term(int) { g_run = false; }

int main(int argc, char **argv) {
#ifdef _WIN32
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
#endif
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    signal(SIGINT, on_term);
    signal(SIGTERM, on_term);

    std::string cfg_path = "/etc/config/iptv-helper";
    std::string mode = "daemon";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-c" && i + 1 < argc) cfg_path = argv[++i];
        else if (a == "generate" || a == "proxy" || a == "daemon") mode = a;
    }
    openlog("iptv-helper", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    g_cfg.load(cfg_path);

    if (mode == "generate") {
        bool ok = run_generate(g_cfg);
        closelog();
        return ok ? 0 : 1;
    }
    if (mode == "proxy") {
        rtsp_proxy_main(g_cfg);
        closelog();
        return 0;
    }
    std::thread gen([]() { run_generate(g_cfg); });
    gen.detach();
    std::thread px([]() { rtsp_proxy_main(g_cfg); });
    px.detach();
    scheduler_main(g_cfg, g_cfg.getlist("schedule"));
    closelog();
    return 0;
}

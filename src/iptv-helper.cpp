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
#include <cerrno>
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

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <direct.h>
#  pragma comment(lib, "ws2_32.lib")
#  define MKDIR(p) _mkdir(p)
#  define CLOSESOCK closesocket
#  define SOCKOPT const char
#  define SOCKLEN int
#  define SHUT_RDWR 2
// Windows 本地调试垫片:日志只走 stderr(Linux 目标不受影响)
enum { LOG_INFO = 6, LOG_ERR = 3, LOG_PID = 0x01, LOG_NDELAY = 0x08, LOG_DAEMON = 0x18 };
static inline void syslog(int, const char *, ...) {}
static inline void openlog(const char *, int, int) {}
static inline void closelog() {}
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <sys/stat.h>
#  include <syslog.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  define MKDIR(p) mkdir(p, 0755)
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
                    // 用 '\n' 而非空格做内部拼接:list 项本身可能含空格(如 catchup 属性片段)
                    kv[key] = (it == kv.end() ? "" : it->second + "\n") + val;
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
        for (auto &s : split(get(k), '\n')) if (!trim(s).empty()) out.push_back(trim(s));
        return out;
    }
};

static Config g_cfg;

// ---------------------------------------------------------------- 路径 / 图片小工具
static bool make_dir(const std::string &path) {
    if (path.empty()) return false;
    if (MKDIR(path.c_str()) == 0) return true;
    return errno == EEXIST;
}
// 递归建目录(/www/iptv 与 C:/x/y 都能处理)
static bool make_dirs(const std::string &path) {
    if (path.empty()) return false;
    std::string cur;
    size_t i = 0;
    if (path.size() >= 2 && path[1] == ':') { cur = path.substr(0, 2); i = 2; } // 盘符
    for (; i < path.size(); ++i) {
        if (path[i] != '/' && path[i] != '\\') { cur += path[i]; continue; }
        if (!cur.empty() && cur.back() != ':') make_dir(cur);
        cur += '/';
    }
    // 结尾分隔符先去掉:Windows 上 _mkdir 对 "D:/a/b/" 这类带尾分隔符的路径不生效
    while (!cur.empty() && (cur.back() == '/' || cur.back() == '\\')) cur.pop_back();
    return make_dir(cur);
}
static bool file_nonempty(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    // fseek/ftell 都要判返回值:定位失败时 ftell 的结果不可信(可能误判"非空")
    bool ok = (fseek(f, 0, SEEK_END) == 0);
    long n = ok ? ftell(f) : -1;
    fclose(f);
    return ok && n > 0;
}
// 文件名安全化(台标按 UserChannelID 命名,只保留字母数字 _ -)。
// 长度上限 111:异常超长输入直接截断,避免文件名超出 NAME_MAX 或成倍的无谓分配;
// 留 17 字符给截断后缀(下划线 + 16 位十六进制哈希),保证最终文件名 ≤ 128。
static const size_t SAFE_NAME_BASE_MAX = 111;
static std::string safe_name(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (o.size() >= SAFE_NAME_BASE_MAX) break;
        o += (isalnum(c) || c == '-' || c == '_') ? (char)c : '_';
    }
    if (o.empty()) return std::string("ch");
    if (s.size() > o.size()) { // 被截断:追加全名哈希,避免长 ID 前缀相同而互相覆盖
        // FNV-1a 64 位:offset basis 14695981039346656037 / prime 1099511628211。
        // 32 位在数百频道量级下碰撞概率不可忽略(生日问题,1/2^32),64 位可视为无碰撞
        static constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
        static constexpr uint64_t FNV_PRIME = 1099511628211ULL;
        uint64_t h = FNV_OFFSET_BASIS;
        for (unsigned char c : s) { h ^= c; h *= FNV_PRIME; }
        char buf[24];
        snprintf(buf, sizeof(buf), "_%016llx", (unsigned long long)h);
        o += buf;
    }
    return o;
}
// 按魔数判断图片类型(不信任 URL 后缀,防止把错误页当图片存下来)
static std::string sniff_image_ext(const std::string &b) {
    if (b.size() > 8 && (unsigned char)b[0] == 0x89 && b.compare(1, 3, "PNG") == 0) return "png";
    if (b.size() > 3 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xD8) return "jpg";
    if (b.size() > 6 && b.compare(0, 3, "GIF") == 0) return "gif";
    if (b.size() > 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) return "webp";
    return "";
}
// 生成物对外访问基址 = 站点根(http_pub_base,留空则用本机 LAN IP) + web 路径。
// web 路径 = out_dir 里最后一个 "/www" 之后的部分(/www/iptv -> /iptv),与 LuCI 下载地址算法一致。
static std::string web_base_of(const Config &cfg, const std::string &lan_ip) {
    std::string site = trim(cfg.get("http_pub_base"));
    if (site.empty()) site = "http://" + lan_ip;
    while (site.size() > 1 && site.back() == '/') site.pop_back();
    std::string wp = cfg.get("out_dir", "/www/iptv");
    size_t p = wp.rfind("/www");
    if (p != std::string::npos) {
        std::string rest = wp.substr(p + 4);
        if (rest.empty() || rest[0] == '/') wp = rest; // "/mnt/wwwroot" 这类不动
    } else {
        // out_dir 不在 /www 下(uhttpd 根之外):web 路径无法推导,只用站点根本身
        wp.clear();
    }
    while (wp.size() > 1 && wp.back() == '/') wp.pop_back();
    if (wp == "/") wp.clear();
    // http_pub_base 已含 web 路径(如 http://host/iptv)时不再重复拼接
    if (!wp.empty() && site.size() >= wp.size() &&
        site.compare(site.size() - wp.size(), wp.size(), wp) == 0) return site;
    return site + wp;
}

// ---------------------------------------------------------------- socket 小工具
static void sock_set_timeout(int fd, int seconds) {
#ifdef _WIN32
    // Windows: SO_RCVTIMEO/SO_SNDTIMEO 收 DWORD 毫秒;传 timeval 会被错读成毫秒
    DWORD ms = (DWORD)seconds * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
#else
    struct timeval tv; tv.tv_sec = seconds; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (SOCKOPT *)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (SOCKOPT *)&tv, sizeof(tv));
#endif
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
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &elen);
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

// 解析 http://host[:port]/path(台标 URL)
// 解析 URL 里的 "host[:port]" 段。host 支持方括号 IPv6 字面量([::1]:8080);
// 端口缺失或非法(含 "80abc" 这类半截数字)时回落 def_port;未加方括号的 IPv6 无法与
// host:port 区分,直接判为非法。返回 false 表示该 URL 不可用。
static bool parse_host_port(const std::string &auth, std::string &host, int &port, int def_port = 80) {
    port = def_port;
    std::string hpart, pstr;
    if (!auth.empty() && auth[0] == '[') {
        size_t close = auth.find(']');
        if (close == std::string::npos) return false;
        hpart = auth.substr(1, close - 1); // 去掉方括号,getaddrinfo 接受裸地址
        if (close + 1 < auth.size()) {
            if (auth[close + 1] != ':') return false;
            pstr = auth.substr(close + 2);
        }
    } else {
        size_t c = auth.find(':');
        if (c == std::string::npos) hpart = auth;
        else { hpart = auth.substr(0, c); pstr = auth.substr(c + 1); }
        if (hpart.find(':') != std::string::npos) return false; // 未加方括号的 IPv6
    }
    if (hpart.empty()) return false;
    host = hpart;
    if (pstr.empty()) return true;
    // 用 strtol 严格解析,避免 atoi 对 "80abc" 之类的宽松截断与溢出未定义行为
    char *end = nullptr;
    long pv = strtol(pstr.c_str(), &end, 10);
    if (end && *end == '\0' && pv > 0 && pv <= 65535) port = (int)pv;
    return true;
}

static bool parse_http_url(const std::string &url, std::string &host, int &port, std::string &path) {
    size_t a = url.find("://");
    if (a == std::string::npos) return false;
    a += 3;
    size_t b = url.find('/', a);
    std::string auth = b == std::string::npos ? url.substr(a) : url.substr(a, b - a);
    path = b == std::string::npos ? "/" : url.substr(b);
    return parse_host_port(auth, host, port, 80);
}

class HttpClient {
public:
    std::map<std::string, std::string> cookies;
    // GET(二进制安全,台标下载用)
    HttpResp get(const std::string &host, int port, const std::string &path, int timeout = 15) {
        std::string req = "GET " + path + " HTTP/1.1\r\n";
        req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
        std::string ck = cookie_header();
        if (!ck.empty()) req += "Cookie: " + ck + "\r\n";
        req += "User-Agent: iptv-helper\r\nAccept: image/*,*/*\r\nConnection: close\r\n\r\n";
        return roundtrip(host, port, req, timeout);
    }
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

// 下载一个小文件(台标图片),成功时 out 为原始字节
static bool http_fetch(HttpClient &hc, const std::string &url, std::string &out) {
    std::string host, path; int port = 80;
    if (!parse_http_url(url, host, port, path)) { log_e("bad url: %.100s", url.c_str()); return false; }
    HttpResp r = hc.get(host, port, path, 15);
    if (r.status != 200 || r.body.empty()) {
        log_e("GET %.80s -> status=%d size=%d", url.c_str(), r.status, (int)r.body.size());
        return false;
    }
    out.swap(r.body);
    return true;
}

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

// 固定以空值发送的鉴权字段:平台不需要,不显示也不参与配置(即使旧配置里残留了值也一律置空)
static const char *kAuthEmptyFields[] = {"NetUserID", "desktopId", "stbmaker", "ChipID", "VIP", nullptr};

static bool is_auth_empty_field(const char *f) {
    for (int i = 0; kAuthEmptyFields[i]; ++i)
        if (strcmp(f, kAuthEmptyFields[i]) == 0) return true;
    return false;
}

static std::string auth_body(const Config &cfg) {
    // 表单字段与值全部来自配置文件(固定空值字段除外)
    const char *fields[] = {"UserID","Lang","SupportHD","NetUserID","Authenticator","STBType",
        "STBVersion","conntype","STBID","templateName","areaId","userToken","userGroupId",
        "productPackageId","mac","UserField","SoftwareVersion","IsSmartStb","desktopId",
        "stbmaker","XMPPCapability","ChipID","VIP", nullptr};
    std::string body;
    for (int i = 0; fields[i]; ++i) {
        if (i) body += "&";
        body += fields[i];
        std::string v = is_auth_empty_field(fields[i]) ? std::string() : cfg.get(fields[i]);
        body += "=" + url_encode(v);
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
// 写文件并校验长度:fopen 成功但短写(磁盘满等)同样判失败,并删掉半截文件
// (fopen("wb") 已截断旧文件,留下半截 .gz 只会让播放器拿到坏数据)。
// 注意区分两类失败:短写/fflush 失败 = 内容可能不完整 -> 删掉;
// fclose 失败(此时 fflush 已成功,数据已完整交给内核)= 保留文件,只报错。
// quiet=true 时不打印成功日志(台标逐个写会刷屏)。
static bool write_file_ex(const std::string &path, const std::string &data, bool quiet) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { log_e("write %s failed: %s", path.c_str(), strerror(errno)); return false; }
    size_t n = fwrite(data.data(), 1, data.size(), f);
    bool flushed = (n == data.size()) && (fflush(f) == 0);
    int rc = fclose(f);
    if (!flushed) {
        log_e("write %s failed: short write (%d/%d)", path.c_str(), (int)n, (int)data.size());
        ::remove(path.c_str());
        return false;
    }
    if (rc != 0) {
        log_e("write %s: fclose failed (%s), file kept", path.c_str(), strerror(errno));
        return false;
    }
    if (!quiet) log_i("written %s (%d bytes)", path.c_str(), (int)data.size());
    return true;
}
static bool write_file(const std::string &path, const std::string &data) {
    return write_file_ex(path, data, false);
}

// 静默写(台标批量落盘,不逐条打日志)
static bool write_file_q(const std::string &path, const std::string &data) {
    return write_file_ex(path, data, true);
}

// 台标缓存:把 <url> 存成 <dir>/<stem>.<ext>,返回对外可访问的本地 URL(失败返回空)
static std::string cache_logo(const std::string &url, HttpClient &hc, const std::string &dir,
                              const std::string &dir_name, const std::string &web_base,
                              const std::string &stem, bool reuse, int &downloaded) {
    static const char *exts[] = {"png", "jpg", "gif", "webp"};
    if (reuse) {
        for (const char *e : exts) {
            std::string p = dir + "/" + stem + "." + e;
            if (file_nonempty(p)) return web_base + "/" + dir_name + "/" + stem + "." + e;
        }
    }
    std::string body;
    if (!http_fetch(hc, url, body)) return "";
    std::string ext = sniff_image_ext(body);
    if (ext.empty()) { log_e("not an image: %.80s (%d bytes)", url.c_str(), (int)body.size()); return ""; }
    std::string p = dir + "/" + stem + "." + ext;
    if (!write_file_q(p, body)) return "";
    downloaded++;
    return web_base + "/" + dir_name + "/" + stem + "." + ext;
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

// catchup 属性的安全边界:单段片段长度上限、天数声明上限(留空/0 = 不写入)
static const size_t CATCHUP_FRAG_MAX = 512;
static const int    CATCHUP_DAYS_MAX = 365;

static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 仅接受 [-]dddd 形式(不含空格、引号等可污染属性串的字符)
static bool is_signed_int(const std::string &s) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

// 属性名跟随语义:时移 => shift / shift-source;其余(回看)=> catchup / catchup-source。
// 两组属性名必须不同,否则同一行出现重名属性会互相顶掉(旧版把 playseek 顶成了 starttime)。
static std::string catchup_attr_name(const std::string &type) {
    return (type == "shift") ? "shift" : "catchup";
}

// catchup_params 为空时按类型给的兜底片段。
// 本平台参数对应关系(实测结论):append 回看用 playseek=起-止;时移用 starttime=起-止。
// 属性名由 catchup_attr_name 决定,属性值即所选类型;模板按类型给默认。
// default(HTTP/HLS)该平台惯例是 UTC + T 分隔;flussonic 为 开始 + 时长(秒)。
static std::string catchup_fallback(const std::string &type, const Config &cfg) {
    std::string fmt = cfg.get("catchup_fmt", "yyyyMMddHHmmss");
    const bool ts = (type == "shift");
    const std::string nm = catchup_attr_name(type);
    std::string tpl;
    if (ts) tpl = "?starttime=${(b)" + fmt + "}-${(e)" + fmt + "}";
    else if (type == "flussonic") tpl = "?start=${timestamp}&duration=${duration}";
    else if (type == "default") tpl = "?starttime=${(b)yyyyMMdd|UTC}T${(b)HHmmss|UTC}"
                                       "&endtime=${(e)yyyyMMdd|UTC}T${(e)HHmmss|UTC}";
    else tpl = "?playseek=${(b)" + fmt + "}-${(e)" + fmt + "}"; // append:本平台回看实测可用形态
    return nm + "=\"" + type + "\" " + nm + "-source=\"" + tpl + "\"";
}

// 把 catchup_params 各项归一成「属性组」。每项两种写法:
//   1) 完整属性片段(含 =\" ): catchup="append" catchup-source="?playseek=${(b)yyyyMMddHHmmss}-${(e)yyyyMMddHHmmss}"
//      或 shift="append" shift-source="?starttime=${(b)yyyyMMddHHmmss}-${(e)yyyyMMddHHmmss}"
//      -> 自成一组,原样保留。多组用单个空格连接(不使用 " or "),因此回看与时移可同时存在:
//         catchup="append" catchup-source="?playseek=..." shift="append" shift-source="?starttime=..."
//   2) 旧式裸模板:   playseek=${(b)yyyyMMddHHmmss}-${(e)yyyyMMddHHmmss}
//      -> 全部裸模板合并回「单条」catchup="<type>" catchup-source="?a or ?b",
//         与 1.0.0 的产物逐字节一致:老配置升级后 m3u 语义不变,
//         且不会退化成同一行并列两个 catchup="append"(后者 playseek 会被 starttime 顶掉)。
static std::vector<std::string> catchup_groups(const std::vector<std::string> &params,
                                               const std::string &type) {
    std::vector<std::string> groups, naked;
    size_t naked_at = std::string::npos; // 合并后的裸模板组插回首个裸模板的位置
    for (size_t i = 0; i < params.size(); ++i) {
        std::string t = trim(params[i]);
        if (t.empty()) continue;
        if (t.find("=\"") != std::string::npos) { groups.push_back(t); continue; } // 完整属性片段
        if (starts_with(t, "?")) t.erase(0, 1);
        if (naked.empty()) naked_at = groups.size();
        naked.push_back(t);
    }
    if (!naked.empty()) {
        std::string src;
        for (size_t i = 0; i < naked.size(); ++i) src += (i ? " or ?" : "?") + naked[i];
        // 属性名与兜底一致(shift 类型产出 shift/shift-source),避免与自身契约冲突
        const std::string nm = catchup_attr_name(type);
        groups.insert(groups.begin() + (naked_at == std::string::npos ? 0 : naked_at),
                      nm + "=\"" + type + "\" " + nm + "-source=\"" + src + "\"");
    }
    return groups;
}

static std::string gen_catchup_attr(const Config &cfg) {
    std::string type = trim(cfg.get("catchup_type", "append"));
    if (type.empty()) type = "append";
    std::vector<std::string> params = cfg.getlist("catchup_params");
    if (params.empty()) params.push_back(catchup_fallback(type, cfg));
    std::vector<std::string> groups = catchup_groups(params, type);
    std::string a;
    for (size_t i = 0; i < groups.size(); ++i) {
        const std::string &frag = groups[i];
        // 单段属性上限:防止手改 uci 塞入超长内容把 #EXTINF 整行撑爆(播放器可能直接弃用该行)
        if (frag.size() > CATCHUP_FRAG_MAX) {
            log_e("catchup_params[%d] too long (%d > %d), skipped",
                  (int)i, (int)frag.size(), (int)CATCHUP_FRAG_MAX);
            continue;
        }
        a += " " + frag;
    }
    if (a.empty()) {
        // 所有片段都被超长检查丢弃:补一条汇总日志,避免「自定义模板静默失效」
        log_e("all catchup_params entries rejected (> %d bytes each), fall back to bare catchup",
              (int)CATCHUP_FRAG_MAX);
        a = " catchup=\"" + type + "\"";
    }
    // 声明属性统一夹取范围:天数 0/留空 = 不写入
    int days = clamp_int(cfg.geti("catchup_days", 0), 0, CATCHUP_DAYS_MAX);
    if (days > 0) a += " catchup-days=\"" + std::to_string(days) + "\"";
    std::string corr = trim(cfg.get("catchup_correction"));
    if (!corr.empty()) {
        // 只允许 [-]整数,其余(含引号/空格)一律丢弃,避免污染属性串
        if (corr.size() <= 16 && is_signed_int(corr)) a += " catchup-correction=\"" + corr + "\"";
        else log_e("catchup_correction '%s' is not a plain integer, ignored", corr.c_str());
    }
    int ts = clamp_int(cfg.geti("timeshift_days", 0), 0, CATCHUP_DAYS_MAX);
    if (ts > 0) a += " timeshift=\"" + std::to_string(ts) + "\"";
    return a;
}

static std::string gen_replay_m3u(const Config &cfg, const std::vector<Channel> &chs,
                                  const std::string &base, bool catchup) {
    std::string o = "#EXTM3U\n";
    std::string extra;
    if (catchup) extra = gen_catchup_attr(cfg);
    for (auto &ch : chs) {
        std::string sp = smil_path(ch);
        if (sp.empty()) continue;
        o += extinf(ch, extra);
        o += base + sp + "\n";
    }
    return o;
}

static std::string gen_txt(const Config &cfg, const std::vector<Channel> &chs,
                           const std::string &lan_ip, const std::string &rtsp_self,
                           const std::string &web_base) {
    std::string o;
    o += "IPTV channel list  generated: " + now_str() + "\n";
    o += "live(lan):  " + cfg.get("udpxy_lan") + "/udp/<multicast>\n";
    o += "replay(lan): " + rtsp_self + "<smil_path>[?playseek=YYYYMMDDHHMMSS-YYYYMMDDHHMMSS]\n";
    o += "epg: " + web_base + "/" + cfg.get("epg_file", "PL.xml") + ".gz\n";
    o += "logo: " + web_base + "/" + cfg.get("logo_dir", "logo") + "/<userChannelId>.<png|jpg|gif>\n";
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
    // 注意:含 \x00 的字节串不能用 const char* 构造/追加(会在首个 NUL 截断)
    static const char hdr[10] = {0x1f, (char)0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, (char)0xff};
    std::string o(hdr, sizeof(hdr));
    size_t pos = 0;
    do {
        size_t n = std::min<size_t>(65535, in.size() - pos);
        bool last = (pos + n >= in.size());
        // 最后一块直接标 BFINAL=1,不再追加空的终结块
        // (BFINAL=1 的数据块后再补 5 字节空块,部分解码器会把它当成 CRC/ISIZE 而报错)。
        // do-while 保证空输入也至少有 1 个块(空的 BFINAL=1 块),不会产出无块的非 gzip 数据。
        o += (char)(last ? 1 : 0);
        o += (char)(n & 0xFF); o += (char)(n >> 8);
        o += (char)(~n & 0xFF); o += (char)((~n >> 8) & 0xFF);
        o.append(in, pos, n);
        pos += n;
    } while (pos < in.size());
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
    // 输出路径与对外地址先算好:台标缓存需要用到(logo 目录在 out_dir 下)
    std::string dir = cfg.get("out_dir", "/www/iptv");
    // 去掉尾部多余分隔符,避免拼出 "/iptv//logo" 这类路径
    while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    if (!make_dirs(dir)) log_e("mkdir %s failed, writes may fail", dir.c_str());
    std::string lan_ip = cfg.get("lan_ip");
    if (lan_ip.empty()) lan_ip = detect_lan_ip(cfg);
    std::string rtsp_self = "rtsp://" + lan_ip + ":" + std::to_string(cfg.geti("rtsp_port", 554));
    std::string lan_replay = cfg.get("replay_via", "proxy") == "direct"
        ? "rtsp://" + cfg.get("edge_host") + ":" + std::to_string(cfg.geti("edge_port", 554))
        : rtsp_self;
    std::string web_base = web_base_of(cfg, lan_ip);
    log_i("out_dir=%s web_base=%s", dir.c_str(), web_base.c_str());

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
        // 台标下载到 <out_dir>/<logo_dir>/,m3u 里 tvg-logo 指向本地缓存(不依赖 EPG 服务器)
        bool cache = cfg.getb("cache_logo", true);
        bool reuse = cfg.getb("logo_reuse", true);
        std::string ldir_name = cfg.get("logo_dir", "logo");
        // logo_dir 来自配置,拒绝路径穿越(.. 或绝对路径/盘符),回退默认值
        if (ldir_name.find("..") != std::string::npos || ldir_name.find(':') != std::string::npos ||
            ldir_name.find('/') != std::string::npos || ldir_name.find('\\') != std::string::npos) {
            log_e("logo_dir '%s' is not a plain subdir name, fallback to 'logo'", ldir_name.c_str());
            ldir_name = "logo";
        }
        std::string ldir = dir + "/" + ldir_name;
        if (cache) {
            if (make_dirs(ldir)) log_i("logo dir: %s", ldir.c_str());
            else { log_e("mkdir %s failed, keep remote logo urls", ldir.c_str()); cache = false; }
        }
        int got = 0, downloaded = 0;
        for (size_t i = 0; i < chs.size(); ++i) {
            if (need_reauth(chs[i].ucid)) do_auth(hc, cfg);
            std::string url = fetch_logo(hc, cfg, chs[i].id, tz);
            if (url.empty()) continue;
            if (cache) {
                std::string stem = safe_name(chs[i].ucid.empty() ? chs[i].id : chs[i].ucid);
                std::string local = cache_logo(url, hc, ldir, ldir_name, web_base, stem, reuse, downloaded);
                if (!local.empty()) { chs[i].logo = local; got++; }
                else chs[i].logo = url; // 缓存失败回退远端地址
            } else chs[i].logo = url;
            if ((i % 50) == 49) log_i("logo %d/%d", (int)i + 1, (int)chs.size());
        }
        if (cache) log_i("logo cached: %d/%d (new %d)", got, (int)chs.size(), downloaded);
    }

    std::map<std::string, std::vector<std::vector<Prog>>> playbills;
    for (size_t i = 0; i < chs.size(); ++i) {
        if (need_reauth(chs[i].ucid)) do_auth(hc, cfg);
        playbills[chs[i].id] = fetch_playbills(hc, cfg, chs[i]);
        if ((i % 50) == 49) log_i("playbill %d/%d", (int)i + 1, (int)chs.size());
    }

    // 写盘失败(磁盘满/无权限/目录不存在)不能静默:逐个写,最后汇总
    bool ok = true;
    ok &= write_file(dir + "/" + cfg.get("lanlive_file", "LanLive.m3u"), gen_live_m3u(chs, cfg.get("udpxy_lan")));
    // 内网回看同样带 catchup(playseek)时间戳,配合节目单可点播回放
    ok &= write_file(dir + "/" + cfg.get("lanreplay_file", "LanReplay.m3u"), gen_replay_m3u(cfg, chs, lan_replay, cfg.getb("lan_catchup", true)));
    ok &= write_file(dir + "/" + cfg.get("netlive_file", "NetLive.m3u"), gen_live_m3u(chs, cfg.get("udpxy_pub")));
    ok &= write_file(dir + "/" + cfg.get("netreplay_file", "NetReplay.m3u"), gen_replay_m3u(cfg, chs, cfg.get("replay_pub"), true));
    ok &= write_file(dir + "/" + cfg.get("txt_file", "channels.txt"), gen_txt(cfg, chs, lan_ip, rtsp_self, web_base));
    std::string xml = gen_epg_xml(cfg, chs, playbills);
    ok &= write_file(dir + "/" + cfg.get("epg_file", "PL.xml"), xml);
    ok &= write_file(dir + "/" + cfg.get("epg_file", "PL.xml") + ".gz", gzip_store(xml));
    if (!ok) { log_e("generate finished with write errors, check %s", dir.c_str()); return false; }
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

    // 上游目标:已解析节点 > 全局缓存 > 边缘服务器
    std::string target_url(const std::string &edge_host, int edge_port) {
        if (!node_url.empty()) return node_url;
        std::string c = cache_get(lower(path_query));
        if (!c.empty()) { node_url = c; return c; }
        return "rtsp://" + edge_host + ":" + std::to_string(edge_port) + path_query;
    }
};

using SessPtr = std::shared_ptr<RtspSess>;

static bool read_rtsp(int fd, std::string &buf, std::string &msg, int timeout) {
    sock_set_timeout(fd, timeout);
    while (buf.find("\r\n\r\n") == std::string::npos) {
        char tmp[8192];
        int k = (int)recv(fd, tmp, sizeof(tmp), 0);
        if (k <= 0) {
#ifdef _WIN32
            log_e("read_rtsp recv fail fd=%d k=%d wsaerr=%d", fd, k, WSAGetLastError());
#else
            log_e("read_rtsp recv fail fd=%d k=%d errno=%d", fd, k, errno);
#endif
            return false;
        }
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
    return parse_host_port(auth, host, port, def_port);
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
        // 目标优先级:已解析节点 > 全局缓存 > 边缘服务器(缓存命中会回填 node_url)
        std::string target = force_edge
            ? "rtsp://" + edge_host + ":" + std::to_string(edge_port) + s->path_query
            : s->target_url(edge_host, edge_port);
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
        {
            struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_addr.s_addr = caddr;
            log_i("connect from %s", inet_ntoa(sa.sin_addr));
        }
        try {
            while (g_run) {
                std::string msg;
                if (!read_rtsp(cfd, s->cbuf, msg, 15)) break;
                std::string first = first_line_of(msg);
                size_t sp1 = first.find(' ');
                size_t sp2 = first.find(' ', sp1 + 1);
                if (sp1 == std::string::npos || sp2 == std::string::npos) {
                    log_e("unparsable request line (%d bytes): %.80s", (int)msg.size(), msg.c_str());
                    break;
                }
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
                                // 绑定到客户端连入的本机地址,保证 RTP 源地址与 Transport
                                // 里宣告的 source 一致(多接口/跨网段时客户端会按源过滤)
                                a.sin_addr.s_addr = inet_addr(s->local_ip.c_str());
                                if (a.sin_addr.s_addr == 0xFFFFFFFFu || a.sin_addr.s_addr == 0)
                                    a.sin_addr.s_addr = 0; // 解析失败退回 0.0.0.0
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
            // 保持网络字节序,sendto 直接使用
            uint32_t addr = ca.sin_addr.s_addr;
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

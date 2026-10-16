/****************************************************************************
 * net/http_client.h —— 最小 HTTPS 客户端（仅用于出网查询，如 VirusTotal）
 *
 * ── 为什么本仓库原先没有 ──
 * 服务端是"零第三方依赖 + 自研 POSIX socket"，现有的 TLS 代码只用于**终止**入站连接
 * （http_server.cpp）。这里补的是**出站**方向，且只在 CV_HAVE_OPENSSL 下编译。
 * 没有 OpenSSL 时本文件照样能编译，但 httpsGet() 一律返回失败并说明原因 ——
 * 调用方（病毒扫描）按 fail-open 处理，不会让网盘不可用。
 *
 * ── 本文件里哪部分能测、哪部分不能 ──
 *   ✅ parseHttpResponse()  —— **纯函数**，输入是字节流。状态行、头、Content-Length、
 *      **chunked 分块解码**、trailer 全在这层。第三方报文最容易出 bug 的地方就在 chunked，
 *      所以它必须可单测（见 lead_http_client_harness）。
 *   ❌ httpsGet() —— socket + 非阻塞 connect + TLS 握手 + 读取。**本机无法验证**
 *      （无 OpenSSL 构建、也无出网）。这段的失败被调用方按 fail-open 处理，且会记录原始原因。
 *
 * ── 安全上不能省的几处 ──
 *   · SSL_set_verify(SSL_VERIFY_PEER) + 默认信任库 + **主机名校验**：少了任意一项就等于不做校验，
 *     中间人可以随意伪造响应说"这个文件是干净的"。
 *   · SNI（SSL_set_tlsext_host_name）：现代 CDN 必须有，否则握手直接失败。
 *   · 全程超时（connect / 握手 / 读）：没有超时的话，对方不回包会把上传线程**永久挂住**。
 ****************************************************************************/
#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace cv {
namespace net {

namespace detail {
inline std::string lower(const std::string &s);
inline std::string trim(const std::string &s);
}   // namespace detail

struct HttpResponse
{
    int                                  status = 0;
    std::vector<std::pair<std::string, std::string>> headers;   // 键已转小写
    std::string                          body;

    // ⚠️ 查询键**自己转小写**：表里存的是小写，但调用方很容易顺手写 "Content-Type"。
    //    让调用方记住这条纯属给自己埋坑（本轮用例 A4 就是这么把它抓出来的）。
    std::string header(const std::string &key) const
    {
        const std::string k = detail::lower(key);
        for (const auto &kv : headers)
            if (kv.first == k)
                return kv.second;
        return std::string();
    }
};

namespace detail {

inline std::string lower(const std::string &s)
{
    std::string o = s;
    for (char &c : o)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

inline std::string trim(const std::string &s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

// 十六进制分块长度（允许大写；忽略 ';' 之后的扩展）
inline bool parseChunkSize(const std::string &line, std::size_t &out, std::string &err)
{
    std::size_t i = 0;
    while (i < line.size() && line[i] == ' ')
        ++i;
    std::size_t v = 0;
    bool        any = false;
    for (; i < line.size(); ++i) {
        const char c = line[i];
        if (c == ';')
            break;   // 分块扩展，忽略
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else if (c == ' ' || c == '\r')
            break;
        else {
            err = std::string("分块长度含非法字符: ") + c;
            return false;
        }
        v = v * 16 + static_cast<std::size_t>(d);
        any = true;
        if (v > (1u << 28)) {   // 单个分块 > 256MB 不合理，防炸
            err = "分块长度异常过大";
            return false;
        }
    }
    if (!any) {
        err = "分块长度为空";
        return false;
    }
    out = v;
    return true;
}

// chunked 解码：<hex>[;ext]\r\n <data>\r\n ... 0\r\n [trailers] \r\n
inline bool dechunk(const std::string &raw, std::string &out, std::string &err)
{
    out.clear();
    std::size_t pos = 0;
    for (;;) {
        const std::size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos) {
            // 末尾缺 CRLF 也容忍：有些实现对最后一个 0 块之后就不再补
            if (pos >= raw.size())
                return true;
            err = "chunked 报文在分块长度行处截断";
            return false;
        }
        std::size_t size = 0;
        if (!parseChunkSize(raw.substr(pos, eol - pos), size, err))
            return false;
        pos = eol + 2;

        if (size == 0) {
            // 末尾 trailer（可为零行）→ 到空行为止；不解析内容
            for (;;) {
                const std::size_t t = raw.find("\r\n", pos);
                if (t == std::string::npos)
                    return true;   // trailer 缺失/截断：内容已收全，不算致命
                if (t == pos)
                    return true;   // 空行 = 结束
                pos = t + 2;
            }
        }
        if (pos + size > raw.size()) {
            err = "chunked 报文的数据段被截断";
            return false;
        }
        out.append(raw, pos, size);
        pos += size;
        // 数据后必须是 CRLF
        if (pos + 2 <= raw.size() && raw.compare(pos, 2, "\r\n") == 0) {
            pos += 2;
        } else if (pos >= raw.size()) {
            return true;   // 末尾截断：内容已拿到
        } else {
            err = "chunked 报文的数据段后缺少 CRLF";
            return false;
        }
    }
}

}   // namespace detail

// 解析一条完整的 HTTP 响应（**纯函数**）。
// 注意：调用方通常把连接读到 EOF 再解析，所以"用 Connection: close 读到结束"是最稳的收包方式，
// 也就不必处理"响应还没收全"的中间态。
inline bool parseHttpResponse(const std::string &raw, HttpResponse &out, std::string &err)
{
    out = HttpResponse();
    const std::size_t head = raw.find("\r\n\r\n");
    if (head == std::string::npos) {
        err = "响应头不完整（没有找到空行）";
        return false;
    }
    const std::string headText = raw.substr(0, head);
    const std::string bodyRaw  = raw.substr(head + 4);

    // ---- 状态行 ----
    const std::size_t eol = headText.find("\r\n");
    const std::string statusLine = (eol == std::string::npos) ? headText : headText.substr(0, eol);
    if (statusLine.rfind("HTTP/", 0) != 0) {
        err = "状态行不是 HTTP 响应: " + statusLine.substr(0, 40);
        return false;
    }
    const std::size_t sp = statusLine.find(' ');
    if (sp == std::string::npos) {
        err = "状态行缺少状态码";
        return false;
    }
    std::size_t p = sp + 1;
    while (p < statusLine.size() && statusLine[p] == ' ')
        ++p;
    int code = 0;
    if (p + 2 >= statusLine.size() + 1) {
        err = "状态码过短";
        return false;
    }
    for (int k = 0; k < 3 && p < statusLine.size() && std::isdigit(static_cast<unsigned char>(statusLine[p]));
         ++k, ++p)
        code = code * 10 + (statusLine[p] - '0');
    if (code < 100 || code > 599) {
        err = "状态码非法";
        return false;
    }
    out.status = code;

    // ---- 头部（键统一转小写，同名后者覆盖）----
    if (eol != std::string::npos) {
        std::size_t pos = eol + 2;
        while (pos < headText.size()) {
            const std::size_t e = headText.find("\r\n", pos);
            const std::string line =
                (e == std::string::npos) ? headText.substr(pos) : headText.substr(pos, e - pos);
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                const std::string k = detail::lower(detail::trim(line.substr(0, colon)));
                const std::string v = detail::trim(line.substr(colon + 1));
                bool replaced = false;
                for (auto &kv : out.headers)
                    if (kv.first == k) {
                        kv.second = v;
                        replaced = true;
                        break;
                    }
                if (!replaced)
                    out.headers.emplace_back(k, v);
            }
            if (e == std::string::npos)
                break;
            pos = e + 2;
        }
    }

    // ---- 正文 ----
    const std::string te = detail::lower(out.header("transfer-encoding"));
    if (te.find("chunked") != std::string::npos) {
        // ⚠️ chunked 优先于 Content-Length（RFC 7230：两者同时出现时必须忽略 Content-Length）
        if (!detail::dechunk(bodyRaw, out.body, err))
            return false;
        return true;
    }
    const std::string cl = out.header("content-length");
    if (!cl.empty()) {
        char              *end = nullptr;
        const long long    n   = std::strtoll(cl.c_str(), &end, 10);
        if (n < 0) {
            err = "Content-Length 为负数";
            return false;
        }
        const std::size_t want = static_cast<std::size_t>(n);
        if (bodyRaw.size() < want) {
            err = "正文被截断（收到的字节少于 Content-Length）";
            return false;
        }
        out.body = bodyRaw.substr(0, want);   // 多出来的（下一响应）不取
        return true;
    }
    // 无长度信息：按"读到连接关闭"处理，全收
    out.body = bodyRaw;
    return true;
}

// 出站 HTTPS GET。**本机未验证**（无 OpenSSL 构建、无出网）；失败时 err 给出原始原因。
// 无 OpenSSL 时直接失败，调用方按 fail-open 处理。
inline bool httpsGet(const std::string &host, const std::string &path,
                     const std::vector<std::pair<std::string, std::string>> &headers,
                     int timeoutMs, HttpResponse &out, std::string &err);

}   // namespace net
}   // namespace cv

#ifdef CV_HAVE_OPENSSL
#include "net/http_client_tls.ipp"
#endif

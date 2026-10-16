/****************************************************************************
 * net/http_client_tls.ipp —— httpsGet() 的实际实现（仅 CV_HAVE_OPENSSL 下编译）
 *
 * ⚠️ **本文件未经实测**。原因很实在：本机开发环境没有 OpenSSL 构建分支，也没有出网。
 *    所以它被刻意写得"每一步失败都有明确 err、且绝不静默成功" —— 调用方（病毒扫描）
 *    拿不到结论时按 fail-open 处理并记录原始原因，不会让网盘不可用。
 *    上线前请在一台配了 OpenSSL 的 Linux 上跑一次 `--scan --scan-api-key=…` 实测。
 *
 * 本文件只做三件事，且都必须带超时：
 *   1. 非阻塞 connect（select 等可写 + 查 SO_ERROR）
 *   2. TLS 握手（SSL_VERIFY_PEER + 系统信任库 + **主机名校验** + SNI）
 *   3. 发送请求、读到连接关闭
 * 收包策略是 "Connection: close 读到 EOF" —— 这样不必处理"响应还没收全"的中间态，
 * 也就把最容易写错的部分缩小到 parseHttpResponse() 里的 chunked 解码（那部分有单测）。
 ****************************************************************************/
#pragma once

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

namespace cv {
namespace net {
namespace detail {

inline std::int64_t monoMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline int remainMs(std::int64_t deadline)
{
    const std::int64_t r = deadline - monoMs();
    if (r <= 0)
        return 0;
    return static_cast<int>(r > 60000 ? 60000 : r);
}

// 返回 1 就绪 / 0 超时 / -1 出错
inline int waitFd(int fd, bool wantWrite, int timeoutMs)
{
    fd_set rs, ws;
    FD_ZERO(&rs);
    FD_ZERO(&ws);
    if (wantWrite)
        FD_SET(fd, &ws);
    else
        FD_SET(fd, &rs);
    struct timeval tv;
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return ::select(fd + 1, wantWrite ? nullptr : &rs, wantWrite ? &ws : nullptr, nullptr, &tv);
}

inline std::string opensslError()
{
    const unsigned long e = ERR_get_error();
    if (e == 0)
        return "（无 OpenSSL 错误详情）";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    return std::string(buf);
}

struct TlsConn
{
    int      fd = -1;
    SSL     *ssl = nullptr;
    SSL_CTX *ctx = nullptr;
    ~TlsConn()
    {
        if (ssl)
            SSL_free(ssl);
        if (ctx)
            SSL_CTX_free(ctx);
        if (fd >= 0)
            ::close(fd);
    }
};

// 在 SSL 上把 buf 全部发完（非阻塞 + 超时）
inline bool sslSendAll(SSL *ssl, int fd, const std::string &buf, std::int64_t deadline,
                       std::string &err)
{
    std::size_t sent = 0;
    while (sent < buf.size()) {
        const int rc = SSL_write(ssl, buf.data() + sent,
                                 static_cast<int>(buf.size() - sent));
        if (rc > 0) {
            sent += static_cast<std::size_t>(rc);
            continue;
        }
        const int e = SSL_get_error(ssl, rc);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            err = "发送请求失败: " + opensslError();
            return false;
        }
        if (waitFd(fd, e == SSL_ERROR_WANT_WRITE, remainMs(deadline)) <= 0) {
            err = "发送请求超时";
            return false;
        }
        if (remainMs(deadline) == 0) {
            err = "发送请求超时";
            return false;
        }
    }
    return true;
}

// 读到对端关闭（我们都是 Connection: close）
inline bool sslRecvAll(SSL *ssl, int fd, std::string &out, std::int64_t deadline,
                       std::string &err)
{
    char buf[8192];
    for (;;) {
        const int rc = SSL_read(ssl, buf, sizeof(buf));
        if (rc > 0) {
            out.append(buf, static_cast<std::size_t>(rc));
            if (out.size() > (4u << 20)) {   // 4MB 上限：判定报文不该有这么大，防被灌爆内存
                err = "响应过大（超过 4MB），已中止";
                return false;
            }
            continue;
        }
        const int e = SSL_get_error(ssl, rc);
        if (e == SSL_ERROR_ZERO_RETURN)   // 对端正常关闭
            return true;
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            err = "读取响应失败: " + opensslError();
            return false;
        }
        if (waitFd(fd, e == SSL_ERROR_WANT_WRITE, remainMs(deadline)) <= 0) {
            err = "读取响应超时";
            return false;
        }
        if (remainMs(deadline) == 0) {
            err = "读取响应超时";
            return false;
        }
    }
}

}   // namespace detail

inline bool httpsGet(const std::string &host, const std::string &path,
                     const std::vector<std::pair<std::string, std::string>> &headers,
                     int timeoutMs, HttpResponse &out, std::string &err)
{
    out = HttpResponse();
    if (timeoutMs <= 0)
        timeoutMs = 3000;
    const std::int64_t deadline = detail::monoMs() + timeoutMs;

    // ---- 1. DNS + 非阻塞 connect ----
    struct addrinfo  hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    if (::getaddrinfo(host.c_str(), "443", &hints, &res) != 0 || res == nullptr) {
        err = "DNS 解析失败: " + host;
        return false;
    }

    detail::TlsConn conn;
    for (struct addrinfo *ai = res; ai != nullptr; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        const int fl = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);

        const int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) {
            ::close(fd);
            continue;
        }
        if (rc != 0) {
            const int w = detail::waitFd(fd, true, detail::remainMs(deadline));
            if (w <= 0) {
                ::close(fd);
                err = "连接超时: " + host + ":443";
                continue;
            }
            int       soerr = 0;
            socklen_t sl    = sizeof(soerr);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
            if (soerr != 0) {
                ::close(fd);
                err = "连接失败: " + std::string(std::strerror(soerr));
                continue;
            }
        }
        conn.fd = fd;
        break;
    }
    ::freeaddrinfo(res);
    if (conn.fd < 0) {
        if (err.empty())
            err = "无法连接 " + host + ":443";
        return false;
    }

    // ---- 2. TLS 握手（校验一样都不能省）----
    conn.ctx = SSL_CTX_new(TLS_client_method());
    if (!conn.ctx) {
        err = "创建 TLS 上下文失败: " + detail::opensslError();
        return false;
    }
    SSL_CTX_set_min_proto_version(conn.ctx, TLS1_2_VERSION);
    if (SSL_CTX_set_default_verify_paths(conn.ctx) != 1) {
        err = "无法加载系统信任库（证书校验无从进行），已中止";
        return false;
    }
    // 少了这一句就等于不做证书校验 —— 中间人可以伪造"这个文件是干净的"
    SSL_CTX_set_verify(conn.ctx, SSL_VERIFY_PEER, nullptr);

    conn.ssl = SSL_new(conn.ctx);
    if (!conn.ssl) {
        err = "创建 TLS 会话失败: " + detail::opensslError();
        return false;
    }
    SSL_set_fd(conn.ssl, conn.fd);
    SSL_set_tlsext_host_name(conn.ssl, host.c_str());   // SNI：现代 CDN 必需

    // 主机名校验（光有证书链还不够）
    X509_VERIFY_PARAM *vp = SSL_get0_param(conn.ssl);
    X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (X509_VERIFY_PARAM_set1_host(vp, host.c_str(), host.size()) != 1) {
        err = "设置主机名校验失败";
        return false;
    }

    for (;;) {
        const int rc = SSL_connect(conn.ssl);
        if (rc == 1)
            break;
        const int e = SSL_get_error(conn.ssl, rc);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            err = "TLS 握手失败: " + detail::opensslError();
            return false;
        }
        if (detail::waitFd(conn.fd, e == SSL_ERROR_WANT_WRITE, detail::remainMs(deadline)) <= 0) {
            err = "TLS 握手超时";
            return false;
        }
    }
    if (SSL_get_verify_result(conn.ssl) != X509_V_OK) {
        err = "证书校验未通过";
        return false;
    }

    // ---- 3. 发请求 + 收响应 ----
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                      "\r\nConnection: close\r\nUser-Agent: CloudVault/1.0\r\n";
    for (const auto &kv : headers)
        req += kv.first + ": " + kv.second + "\r\n";
    req += "\r\n";

    if (!detail::sslSendAll(conn.ssl, conn.fd, req, deadline, err))
        return false;

    std::string raw;
    if (!detail::sslRecvAll(conn.ssl, conn.fd, raw, deadline, err))
        return false;

    return parseHttpResponse(raw, out, err);
}

}   // namespace net
}   // namespace cv

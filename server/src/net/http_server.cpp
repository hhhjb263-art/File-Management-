#include "net/http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>

#ifdef CV_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "core/json.h"
#include "core/logger.h"

namespace cv {
namespace net {
namespace {

constexpr std::size_t kMaxRequestBytes = 256u * 1024u * 1024u;  // 单次请求上限 256MB
constexpr int kTimeoutSeconds = 30;

std::string statusText(int code) {
  switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 416: return "Range Not Satisfiable";
    case 422: return "Unprocessable Entity";
    case 500: return "Internal Server Error";
    case 507: return "Insufficient Storage";
    default: return "Unknown";
  }
}

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

// 判断 path 是否以 prefix 开头（用于鉴权白名单放行公开分享端点 /s/）。
bool startsWith(const std::string& s, const std::string& prefix) {
  return !prefix.empty() && s.size() >= prefix.size() &&
         s.compare(0, prefix.size(), prefix) == 0;
}

std::vector<std::string> splitPath(const std::string& path) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : path) {
    if (c == '/') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

HttpServer* g_server = nullptr;

void onSignal(int) {
  if (g_server) g_server->shutdown();
}

}  // namespace

// 恒定时间字符串比较（防时序侧信道）：
//   1) 比较上界取 max(len_a, len_b)，逐字节异或累加；
//   2) 长度差以 std::size_t 全程参与 diff（不再截断到 uchar，避免“长度差恰为 256 倍数”时折入值为 0）；
//   3) 不提前 return，比较时长与首个不同字节位置无关。
// 因此“长度不同 ⇒ 必不相等”，且安全抗时序侧信道。
bool constantTimeEqual(const std::string& a, const std::string& b) {
  // 长度差直接以 size_t 参与（不截断），保证长度不同必使 diff 非零
  std::size_t diff = a.size() ^ b.size();
  const std::size_t n = a.size() < b.size() ? a.size() : b.size();
  for (std::size_t i = 0; i < n; ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  // 较长串多出的字节也参与 diff（长度差已使 diff 非零，这里仅保持恒定时长）
  const std::string& longer = a.size() < b.size() ? b : a;
  const std::size_t m = a.size() + b.size() - n;  // == max(len_a, len_b)
  for (std::size_t i = n; i < m; ++i) {
    diff |= static_cast<unsigned char>(longer[i]);
  }
  return diff == 0;
}

void Response::setJson(int code, const std::string& json) {
  status = code;
  contentType = "application/json; charset=utf-8";
  body = json;
}

void Response::setText(int code, const std::string& text) {
  status = code;
  contentType = "text/plain; charset=utf-8";
  body = text;
}

void Response::setBinary(int code, const std::string& data, const std::string& type) {
  status = code;
  contentType = type;
  body = data;
}

void Response::setError(int code, const std::string& message) {
  json::Value v = json::Value::object();
  v.set("error", message);
  v.set("status", code);
  setJson(code, json::dump(v));
}

HttpServer::~HttpServer() { shutdown(); }

void HttpServer::setAuthToken(const std::string& token) { authToken_ = token; }

bool HttpServer::authorized(const Request& req) const {
  // 期望请求头：Authorization: Bearer <token>
  // 兼容历史头：X-CV-Token: <token>（直接携带 token，无前缀）
  std::string token;
  std::string hdr = req.header("authorization");
  const std::string prefix = "Bearer ";
  if (hdr.size() > prefix.size() && hdr.compare(0, prefix.size(), prefix) == 0) {
    token = hdr.substr(prefix.size());
  } else {
    token = req.header("x-cv-token");
  }
  if (token.empty()) return false;
  return constantTimeEqual(token, authToken_);
}

namespace {
// 建立 TCP 监听 socket（bind+listen），失败返回 -1 并填充 err
int makeListener(const std::string& addr, int port, std::string& err) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    err = "socket() failed: " + std::string(std::strerror(errno));
    return -1;
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(static_cast<uint16_t>(port));
  if (addr.empty() || addr == "0.0.0.0") {
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (::inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1) {
    err = "invalid listen address: " + addr;
    ::close(fd);
    return -1;
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
    err = "bind() failed: " + std::string(std::strerror(errno));
    ::close(fd);
    return -1;
  }
  if (::listen(fd, 128) < 0) {
    err = "listen() failed: " + std::string(std::strerror(errno));
    ::close(fd);
    return -1;
  }
  return fd;
}
}  // namespace

bool HttpServer::listen(const std::string& addr, int port, int workers, std::string& err) {
  std::signal(SIGPIPE, SIG_IGN);
  workers_ = workers < 1 ? 1 : workers;
  listenFd_ = makeListener(addr, port, err);
  return listenFd_ >= 0;
}

// HTTPS 监听：加载 PEM 证书/私钥，建立第二个监听 socket。
// 双模式：HTTP（listen）与 HTTPS（本方法）可同时启用，共享同一套路由。
bool HttpServer::listenTls(const std::string& addr, int port, const std::string& certPath,
                           const std::string& keyPath, int workers, std::string& err) {
  std::signal(SIGPIPE, SIG_IGN);
  workers_ = workers < 1 ? 1 : workers;
#ifdef CV_HAVE_OPENSSL
  if (listenFdTls_ >= 0) {
    err = "tls already listening";
    return false;
  }
  if (!sslCtx_) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
      err = "SSL_CTX_new failed";
      return false;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    auto sslErrText = [] {
      char buf[256] = {0};
      ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
      ERR_clear_error();
      return std::string(buf);
    };
    if (SSL_CTX_use_certificate_chain_file(ctx, certPath.c_str()) != 1) {
      err = "load certificate failed: " + certPath + " (" + sslErrText() + ")";
      SSL_CTX_free(ctx);
      return false;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, keyPath.c_str(), SSL_FILETYPE_PEM) != 1) {
      err = "load private key failed: " + keyPath + " (" + sslErrText() + ")";
      SSL_CTX_free(ctx);
      return false;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
      err = "private key does not match certificate";
      SSL_CTX_free(ctx);
      return false;
    }
    sslCtx_ = ctx;
  }
  listenFdTls_ = makeListener(addr, port, err);
  if (listenFdTls_ < 0) return false;
  return true;
#else
  (void)addr;
  (void)port;
  (void)certPath;
  (void)keyPath;
  err = "built without TLS support (rebuild with CV_ENABLE_TLS=ON and OpenSSL installed)";
  return false;
#endif
}

void HttpServer::route(const std::string& method, const std::string& pattern, Handler h) {
  Route r;
  r.method = method;
  r.parts = splitPath(pattern);
  r.handler = std::move(h);
  routes_.push_back(std::move(r));
}

void HttpServer::runForever() {
  if (listenFd_ < 0 && listenFdTls_ < 0) return;
  g_server = this;
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  running_ = true;

  for (int i = 0; i < workers_; ++i) {
    threads_.emplace_back([this] { workerLoop(); });
  }
  if (listenFd_ >= 0) {
    threads_.emplace_back([this] { acceptLoop(listenFd_, false); });
  }
  if (listenFdTls_ >= 0) {
    threads_.emplace_back([this] { acceptLoop(listenFdTls_, true); });
  }

  // 信号处理器会调用 shutdown() 置 running_=false
  while (running_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  queueCv_.notify_all();
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
  threads_.clear();
  g_server = nullptr;
}

void HttpServer::shutdown() {
  if (!running_.exchange(false)) return;
  auto closeListener = [](int& fd) {
    if (fd >= 0) {
      ::shutdown(fd, SHUT_RDWR);
      ::close(fd);
      fd = -1;
    }
  };
  closeListener(listenFd_);
  closeListener(listenFdTls_);
#ifdef CV_HAVE_OPENSSL
  if (sslCtx_) {
    SSL_CTX_free(static_cast<SSL_CTX*>(sslCtx_));
    sslCtx_ = nullptr;
  }
#endif
  queueCv_.notify_all();
}

void HttpServer::acceptLoop(int listenFd, bool isTls) {
  while (running_) {
    int fd = ::accept(listenFd, nullptr, nullptr);
    if (fd < 0) {
      if (running_ && errno != EINTR && errno != EBADF) {
        CV_LOG_WARN(std::string("accept 失败: ") + std::strerror(errno));
      }
      continue;
    }
    timeval tv{};
    tv.tv_sec = kTimeoutSeconds;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    Conn conn;
    conn.fd = fd;
#ifdef CV_HAVE_OPENSSL
    if (isTls && sslCtx_) {
      SSL* ssl = SSL_new(static_cast<SSL_CTX*>(sslCtx_));
      if (!ssl || SSL_set_fd(ssl, fd) != 1 || SSL_accept(ssl) != 1) {
        CV_LOG_WARN(std::string("TLS 握手失败，拒绝连接"));
        if (ssl) SSL_free(ssl);
        ::close(fd);
        continue;
      }
      conn.ssl = ssl;
    }
#else
    (void)isTls;
#endif

    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      queue_.push_back(conn);
      queueCv_.notify_one();
    }
  }
}

void HttpServer::workerLoop() {
  while (true) {
    Conn conn;
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCv_.wait(lock, [this] { return !queue_.empty() || !running_; });
      if (!queue_.empty()) {
        conn = queue_.front();
        queue_.pop_front();
      } else if (!running_) {
        return;
      }
    }
    if (conn.fd >= 0) handleClient(conn);
  }
}

void HttpServer::handleClient(const Conn& conn) {
  // keep-alive：一个 worker 全程持有该连接，循环处理多个请求。
  // residue 保存上次请求读剩的字节（可能是下一请求的开头）。
  std::string residue;
  constexpr std::size_t kMaxRequestsPerConn = 200;

  for (std::size_t reqCount = 0; reqCount < kMaxRequestsPerConn; ++reqCount) {
    std::string raw;
    std::string err;
    if (!readRequest(conn, residue, raw, err)) {
      // 读失败（读超时 / 对端关闭 / 请求被截断）：静默关闭，不刷 WARN
      closeConn(conn);
      return;
    }

    Request req;
    Response resp;
    bool parsed = parseRequest(raw, req, err);
    bool keepAlive = true;

    if (!parsed) {
      resp.setError(400, err);
      keepAlive = false;  // 解析失败 → 关闭
    } else if (toLower(req.header("transfer-encoding")).find("chunked") != std::string::npos) {
      // 协议面：仅支持 Content-Length 定长帧，显式拒绝 chunked，防前置反代场景的请求走私
      resp.setError(501, "Transfer-Encoding: chunked is not supported; use Content-Length");
      keepAlive = false;
    } else if (!authToken_.empty() && req.path != "/healthz" &&
               !startsWith(req.path, "/s/") && !authorized(req)) {
      // 启用鉴权且非豁免接口：/healthz 与公开分享端点 /s/* 免鉴权（分享链接本就面向匿名访问）；
      // 其余接口缺 Token 或不匹配 → 401（JSON 说明原因，恒定时间比较），不 dispatch。
      // 注意：仅放行 /s/ 前缀，绝不放宽 /api/ 下任何路径。
      resp.setError(401, "missing or invalid Authorization token (use: Authorization: Bearer <token>)");
      keepAlive = false;  // 鉴权失败 → 关闭
    } else if (!dispatch(req, resp)) {
      resp.setError(404, "no route for " + req.method + " " + req.path);
    }

    // 解析成功且已 dispatch 时，再按客户端的 Connection 头决定是否复用
    if (keepAlive && parsed) {
      std::string connHdr = toLower(req.header("connection"));
      if (connHdr.find("close") != std::string::npos) keepAlive = false;
    }
    // 达到单连接请求上限：本响应标记关闭，下一轮循环即退出
    if (reqCount + 1 >= kMaxRequestsPerConn) keepAlive = false;

    std::ostringstream head;
    head << "HTTP/1.1 " << resp.status << ' ' << statusText(resp.status) << "\r\n"
         << "Content-Type: " << resp.contentType << "\r\n"
         << "Content-Length: " << resp.body.size() << "\r\n";
    for (const auto& kv : resp.extraHeaders) {
      head << kv.first << ": " << kv.second << "\r\n";
    }
    head << "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n\r\n";
    std::string headStr = head.str();
    if (!writeAll(conn, headStr.data(), headStr.size())) { closeConn(conn); return; }
    if (!resp.body.empty()) {
      if (!writeAll(conn, resp.body.data(), resp.body.size())) { closeConn(conn); return; }
    }

    if (!keepAlive) { closeConn(conn); return; }
    // 否则继续：residue 已在 readRequest 内保留为下一请求的开头
  }
}


bool HttpServer::readRequest(const Conn& conn, std::string& residue, std::string& raw,
                             std::string& err) {
  raw.clear();
  // 复用上次 keep-alive 读剩的字节（可能已含下一个完整请求的开头）
  if (!residue.empty()) {
    raw = std::move(residue);
    residue.clear();
  }
  char buf[8192];
  std::size_t headerEnd = std::string::npos;
  std::size_t contentLength = 0;
  bool headersDone = false;

  // 从当前已有字节（含 residue）中解析请求头并提取 Content-Length
  auto tryParseHeaders = [&]() -> bool {
    std::size_t he = raw.find("\r\n\r\n");
    if (he == std::string::npos) return false;
    headerEnd = he;
    headersDone = true;
    std::string head = raw.substr(0, he);
    std::string lower = toLower(head);
    auto pos = lower.find("content-length:");
    if (pos != std::string::npos) {
      contentLength = std::strtoull(head.c_str() + pos + 15, nullptr, 10);
    }
    return true;
  };

  bool complete = false;
  if (tryParseHeaders()) {
    std::size_t bodyHave = raw.size() - (headerEnd + 4);
    if (bodyHave >= contentLength) complete = true;
  }

  while (!complete) {
    ssize_t n = 0;
    if (!connRead(conn, buf, sizeof(buf), n, err)) return false;
    if (n < 0) {
      if (errno == EINTR) continue;
      if (err.empty()) {
        err = (errno == EAGAIN || errno == EWOULDBLOCK) ? "read timeout" : std::strerror(errno);
      }
      return false;
    }
    if (n == 0) {
      if (raw.empty()) {
        err = "connection closed";
        return false;
      }
      // 对端已关闭：若当前已含完整请求（头齐 + body 到齐）则按完整请求处理，否则报错
      if (headersDone && (raw.size() - (headerEnd + 4)) >= contentLength) {
        complete = true;
        break;
      }
      err = "connection closed before request complete";
      return false;
    }
    raw.append(buf, static_cast<std::size_t>(n));

    if (!headersDone) tryParseHeaders();
    if (headersDone) {
      std::size_t bodyHave = raw.size() - (headerEnd + 4);
      if (bodyHave >= contentLength) { complete = true; break; }
    }
    if (raw.size() > kMaxRequestBytes) {
      err = "request too large";
      return false;
    }
  }

  // 关键正确性：一个 recv 可能多读了「下一个请求」的开头字节。
  // 必须把当前请求之后的残留切出来留给下一次读，否则后续请求会解析错乱。
  std::size_t total = headerEnd + 4 + contentLength;
  if (raw.size() > total) {
    residue = raw.substr(total);
    raw.resize(total);  // raw 只保留（恰好一个）当前请求
  }
  return true;
}

bool HttpServer::parseRequest(const std::string& raw, Request& req, std::string& err) {
  std::size_t headerEnd = raw.find("\r\n\r\n");
  if (headerEnd == std::string::npos) {
    err = "malformed request";
    return false;
  }
  std::string head = raw.substr(0, headerEnd);
  std::istringstream is(head);
  std::string line;

  if (!std::getline(is, line)) {
    err = "empty request line";
    return false;
  }
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  std::istringstream rl(line);
  if (!(rl >> req.method >> req.path >> req.version)) {
    err = "bad request line";
    return false;
  }

  auto qpos = req.path.find('?');
  if (qpos != std::string::npos) {
    req.query = req.path.substr(qpos + 1);
    req.path = req.path.substr(0, qpos);
  }
  if (req.path.empty()) req.path = "/";

  while (std::getline(is, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.empty()) continue;
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = toLower(line.substr(0, colon));
    std::string value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
      value.erase(value.begin());
    req.headers[key] = value;
  }

  req.body = raw.substr(headerEnd + 4);
  return true;
}

bool HttpServer::dispatch(const Request& req, Response& resp) {
  std::vector<std::string> parts = splitPath(req.path);
  bool pathMatched = false;
  for (const Route& r : routes_) {
    if (r.parts.size() != parts.size()) continue;
    std::map<std::string, std::string> params;
    bool ok = true;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (!r.parts[i].empty() && r.parts[i][0] == ':') {
        params[r.parts[i].substr(1)] = parts[i];
      } else if (r.parts[i] != parts[i]) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;
    pathMatched = true;
    if (r.method != req.method) continue;
    Request copy = req;
    copy.params = params;
    r.handler(copy, resp);
    return true;
  }
  if (pathMatched) {
    resp.setError(405, "method not allowed");
    return true;
  }
  return false;
}

bool HttpServer::writeAll(const Conn& conn, const char* data, std::size_t len) {
  std::size_t sent = 0;
  while (sent < len) {
    ssize_t n = 0;
    if (!connWrite(conn, data + sent, len - sent, n)) return false;
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

// 读取：TLS 连接走 SSL_read，明文连接走 recv
bool HttpServer::connRead(const Conn& conn, char* buf, std::size_t cap, ssize_t& n,
                          std::string& err) {
  n = -1;
  errno = 0;
#ifdef CV_HAVE_OPENSSL
  if (conn.ssl) {
    SSL* ssl = static_cast<SSL*>(conn.ssl);
    int r = SSL_read(ssl, buf, static_cast<int>(cap));
    if (r > 0) {
      n = r;
      return true;
    }
    int e = SSL_get_error(ssl, r);
    if (e == SSL_ERROR_ZERO_RETURN) {
      n = 0;
      return true;
    }
    err = "ssl read error " + std::to_string(e);
    return false;
  }
#else
  (void)err;
#endif
  n = ::recv(conn.fd, buf, cap, 0);
  return n >= 0 || (errno != 0);
}

// 写入：TLS 连接走 SSL_write，明文连接走 send
bool HttpServer::connWrite(const Conn& conn, const char* data, std::size_t len, ssize_t& n) {
  n = -1;
  errno = 0;
#ifdef CV_HAVE_OPENSSL
  if (conn.ssl) {
    SSL* ssl = static_cast<SSL*>(conn.ssl);
    int r = SSL_write(ssl, data, static_cast<int>(len));
    if (r > 0) {
      n = r;
      return true;
    }
    return false;   // 写失败（含对端关闭），由调用方终止
  }
#endif
  n = ::send(conn.fd, data, len, 0);
  return n >= 0 || (errno != 0);
}

// 关闭：先 TLS 收尾，再关 fd
void HttpServer::closeConn(const Conn& conn) {
#ifdef CV_HAVE_OPENSSL
  if (conn.ssl) {
    SSL* ssl = static_cast<SSL*>(conn.ssl);
    SSL_shutdown(ssl);
    SSL_free(ssl);
  }
#endif
  if (conn.fd >= 0) ::close(conn.fd);
}

}  // namespace net
}  // namespace cv

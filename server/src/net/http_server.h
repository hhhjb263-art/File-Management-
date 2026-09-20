#pragma once

#include <sys/types.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cv {
namespace net {

struct Request {
  std::string method;
  std::string path;    // 不含查询串
  std::string query;   // 不含 '?'
  std::string version;
  std::map<std::string, std::string> headers;  // key 已转小写
  std::string body;
  std::map<std::string, std::string> params;   // 路径参数（不含 ':'）

  // 已认证身份（由 handleClient 的鉴权分支写入；语义冻结，勿改）：
  //   -1 = 未启用账号体系，或使用静态 --auth-token（legacy/admin）⇒ **不做归属过滤**
  //   >=1 = 已登录用户 ⇒ 只允许访问 owner_id == authUserId 的资源（越权一律 404）
  std::int64_t authUserId = -1;

  std::string header(const std::string& key) const {
    auto it = headers.find(key);
    return it == headers.end() ? std::string() : it->second;
  }
  std::string param(const std::string& key) const {
    auto it = params.find(key);
    return it == params.end() ? std::string() : it->second;
  }
};

struct Response {
  int status = 200;
  std::string contentType = "application/json; charset=utf-8";
  std::string body;
  // 附加响应头（如 Accept-Ranges / Content-Range），键已转小写
  std::map<std::string, std::string> extraHeaders;

  void setJson(int code, const std::string& json);
  void setText(int code, const std::string& text);
  void setBinary(int code, const std::string& data, const std::string& type);
  void setError(int code, const std::string& message);
};

using Handler = std::function<void(const Request&, Response&)>;

// 恒定时间字符串比较（防时序侧信道）：长度不同必返回 false，且不提前 return，
// 比较时长与首个不同字节位置无关。供鉴权令牌与分享提取码校验复用。
bool constantTimeEqual(const std::string& a, const std::string& b);

// POSIX socket + 固定线程池的 HTTP/1.1 服务器。
// 单连接支持 keep-alive：一个 worker 全程持有该连接，循环读取请求→处理→写响应，
// 直到客户端要求关闭、解析失败、达到单连接请求上限或读超时（30s SO_RCVTIMEO，静默关闭）。
// 仅支持 Content-Length 定长帧；keep-alive 下会完整读走上一请求 body 的残留字节
// （剩余字节作为下一请求的开头），避免后续请求解析错乱。
// 支持双监听：HTTP（listen）与 HTTPS（listenTls，需编译期启用 OpenSSL）可同时运行，
// 共享同一套路由。
class HttpServer {
 public:
  ~HttpServer();

  bool listen(const std::string& addr, int port, int workers, std::string& err);

  // 设置 API Bearer Token；非空即启用鉴权（除 /healthz 外全接口强制 Authorization: Bearer）。
  // 空 = 不启用（向后兼容）。
  void setAuthToken(const std::string& token);

  // 会话校验回调：token 有效则回 true 并把 userId（>=1）写入 outUserId。
  // 未设置或回 false 时，退回既有静态 token 判定（向后兼容，静态 token 仍视为 legacy/admin）。
  using SessionValidator = std::function<bool(const std::string& token, std::int64_t& outUserId)>;
  void setSessionValidator(SessionValidator v);

  // 账号体系是否"生效"。启用后，除 /healthz 与公开分享端点 /s/* 外的接口都**必须**通过鉴权。
  // 由 main.cpp 按 users 表是否有用户来设置（表空 ⇒ 保持既有单用户行为，冒烟脚本不受影响）。
  void setAccountsEnabled(bool on);

  // 启用 HTTPS 监听（需先/后调用 listen 皆可；证书为 PEM 格式）。
  // 未编译 TLS 支持（CV_HAVE_OPENSSL 未定义）时返回 false 并在 err 说明。
  bool listenTls(const std::string& addr, int port, const std::string& certPath,
                 const std::string& keyPath, int workers, std::string& err);

  // 支持路径参数，如 route("GET", "/api/v1/files/:id", handler)
  void route(const std::string& method, const std::string& pattern, Handler h);

  // 阻塞运行，直到收到 SIGINT/SIGTERM 或调用 shutdown()
  void runForever();

  void shutdown();

 private:
  struct Route {
    std::string method;
    std::vector<std::string> parts;
    Handler handler;
  };

  // 连接描述符 + 可选 TLS 会话（OpenSSL SSL*，以 void* 携带避免头文件污染；
  // 仅在 http_server.cpp 内部经 CV_HAVE_OPENSSL 守卫使用）
  struct Conn {
    int fd = -1;
    void* ssl = nullptr;
  };

  void acceptLoop(int listenFd, bool isTls);
  void workerLoop();
  void handleClient(const Conn& conn);
 private:
  // 鉴权 + 身份认定（会话优先，静态 token 兜底）。通过返回 true。
  //   会话有效 → req.authUserId = <userId>(>=1)
  //   静态 token 匹配 → req.authUserId = -1（legacy/admin，不做归属过滤）
  // 支持 Authorization: Bearer <token>（标准）与 X-CV-Token: <token>（兼容头），恒定时间比较防时序侧信道。
  bool authenticate(Request& req) const;

 public:
  // 读出一个完整请求到 raw；residue 为 in/out：进入时携带上次 keep-alive 读剩的字节
  // （可能是下一个请求的开头），返回时携带本次多读出的（下一个请求）字节。
  bool readRequest(const Conn& conn, std::string& residue, std::string& raw, std::string& err);
  bool parseRequest(const std::string& raw, Request& req, std::string& err);
  bool dispatch(const Request& req, Response& resp);
  bool writeAll(const Conn& conn, const char* data, std::size_t len);
  // 连接 IO 原语：conn.ssl 非空走 SSL_read/SSL_write，否则原生 recv/send
  bool connRead(const Conn& conn, char* buf, std::size_t cap, ssize_t& n, std::string& err);
  bool connWrite(const Conn& conn, const char* data, std::size_t len, ssize_t& n);
  void closeConn(const Conn& conn);

  int listenFd_ = -1;
  int listenFdTls_ = -1;        // HTTPS 监听（未启用为 -1）
  int workers_ = 1;
  std::vector<Route> routes_;
  std::string authToken_;        // API Bearer Token；非空即启用鉴权
  bool accountsEnabled_ = false;         // 账号体系生效 ⇒ 强制鉴权（静态 token 仍可用）
  SessionValidator sessionValidator_;  // 账号体系会话校验（未设置=仅静态 token）

  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::deque<Conn> queue_;
  std::vector<std::thread> threads_;
  std::atomic<bool> running_{false};

  void* sslCtx_ = nullptr;      // OpenSSL SSL_CTX*（TLS 关闭时为空）
};

}  // namespace net
}  // namespace cv

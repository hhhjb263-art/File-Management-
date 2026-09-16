#pragma once

#include <sys/types.h>

#include <atomic>
#include <condition_variable>
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
  // 校验请求是否携带匹配的 Bearer Token（authToken_ 为空时一律放行）。
  // 支持 Authorization: Bearer <token>（标准）与 X-CV-Token: <token>（兼容头），
  // 并以恒定时间比较防时序侧信道。
  bool authorized(const Request& req) const;
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

  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::deque<Conn> queue_;
  std::vector<std::thread> threads_;
  std::atomic<bool> running_{false};

  void* sslCtx_ = nullptr;      // OpenSSL SSL_CTX*（TLS 关闭时为空）
};

}  // namespace net
}  // namespace cv

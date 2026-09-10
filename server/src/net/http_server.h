#pragma once

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

  void setJson(int code, const std::string& json);
  void setText(int code, const std::string& text);
  void setBinary(int code, const std::string& data, const std::string& type);
  void setError(int code, const std::string& message);
};

using Handler = std::function<void(const Request&, Response&)>;

// POSIX socket + 固定线程池的 HTTP/1.1 服务器。
// 当前实现：每连接单请求后关闭（Connection: close），足够 MVP 且不易出错。
class HttpServer {
 public:
  ~HttpServer();

  bool listen(const std::string& addr, int port, int workers, std::string& err);

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

  void acceptLoop();
  void workerLoop();
  void handleClient(int fd);
  bool readRequest(int fd, std::string& raw, std::string& err);
  bool parseRequest(const std::string& raw, Request& req, std::string& err);
  bool dispatch(const Request& req, Response& resp);
  bool writeAll(int fd, const char* data, std::size_t len);

  int listenFd_ = -1;
  int workers_ = 1;
  std::vector<Route> routes_;

  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::deque<int> queue_;
  std::vector<std::thread> threads_;
  std::atomic<bool> running_{false};
};

}  // namespace net
}  // namespace cv

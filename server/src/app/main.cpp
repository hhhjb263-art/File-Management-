// 云匣 CloudVault 服务端入口（MVP 骨架）
//
// 今日能力（模块 0 + 模块 1）：
//   GET  /healthz                    健康检查
//   POST /api/v1/files               整文件上传（分块落盘 + 秒传命中）
//   GET  /api/v1/files               文件列表
//   GET  /api/v1/files/:id           文件元数据
//   GET  /api/v1/files/:id/content   下载（按分块重组）

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/json.h"
#include "core/logger.h"
#include "core/sha256.h"
#include "core/util.h"
#include "meta/db.h"
#include "meta/file_repository.h"
#include "net/http_server.h"
#include "store/content_store.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "0.5.0";
constexpr std::size_t kMaxUploadBytes = 256u * 1024u * 1024u;

cv::json::Value fileToJson(const cv::FileRow& f) {
  cv::json::Value v = cv::json::Value::object();
  v.set("id", static_cast<long long>(f.id));
  v.set("name", f.name);
  v.set("size", static_cast<long long>(f.size));
  v.set("hash", f.contentHash);
  v.set("chunks", f.chunkCount);
  v.set("created_at", static_cast<long long>(f.createdAt));
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace cv;

  Config cfg = loadConfig(argc, argv);
  log::init(cfg.logFile, log::parseLevel(cfg.logLevel));

  std::string err;
  CV_LOG_INFO("云匣服务端 v" << kVersion << " 启动中...");

  std::error_code ec;
  fs::create_directories(cfg.dataDir, ec);
  fs::create_directories(cfg.tmpRoot(), ec);
  if (ec) {
    CV_LOG_ERROR("创建数据目录失败: " << ec.message());
    return 1;
  }

  ContentStore store(cfg.blobRoot());
  if (!store.init(err)) {
    CV_LOG_ERROR("初始化内容存储失败: " << err);
    return 1;
  }

  Db db;
  if (!db.open(cfg.dbPath(), err)) {
    CV_LOG_ERROR("打开元数据库失败: " << err);
    return 1;
  }
  if (!db.initSchema(err)) {
    CV_LOG_ERROR("建表失败: " << err);
    return 1;
  }

  FileRepository repo(db);
  net::HttpServer server;

  // ---- GET /healthz ----
  server.route("GET", "/healthz", [&](const net::Request&, net::Response& resp) {
    json::Value v = json::Value::object();
    v.set("status", "ok");
    v.set("version", kVersion);
    v.set("data_dir", cfg.dataDir);
    resp.setJson(200, json::dump(v));
  });

  // ---- POST /api/v1/files ----
  server.route("POST", "/api/v1/files", [&](const net::Request& req, net::Response& resp) {
    std::string err;
    if (req.body.size() > kMaxUploadBytes) {
      resp.setError(413, "file too large");
      return;
    }
    std::string name = util::baseName(util::urlDecode(req.header("x-cv-name")));
    if (name.empty()) name = util::baseName(util::urlDecode(req.param("name")));
    if (name.empty()) name = "unnamed";

    const std::string& data = req.body;
    std::string contentHash = Sha256::of(data);

    // 秒传判定：整文件哈希是否已存在（存在则分块写入直接命中去重）
    FileRow existing;
    bool instant = repo.findByContentHash(contentHash, existing, err);

    std::vector<std::string> chunkHashes;
    std::vector<std::size_t> chunkSizes;
    std::string manifestHash;
    if (!store.putChunked(data, cfg.chunkSize, chunkHashes, chunkSizes, manifestHash,
                          err)) {
      resp.setError(500, std::string("store failed: ") + err);
      return;
    }

    std::int64_t id = 0;
    if (!repo.insertFile(name, static_cast<std::int64_t>(data.size()), contentHash,
                         chunkHashes, chunkSizes, id, err)) {
      resp.setError(500, std::string("db failed: ") + err);
      return;
    }

    json::Value v = json::Value::object();
    v.set("id", static_cast<long long>(id));
    v.set("name", name);
    v.set("size", static_cast<long long>(data.size()));
    v.set("hash", contentHash);
    v.set("chunks", static_cast<long long>(chunkHashes.size()));
    v.set("instant", instant);
    resp.setJson(201, json::dump(v));
    CV_LOG_INFO("上传 id=" << id << " name=" << name << " size=" << data.size()
                           << (instant ? " [秒传命中]" : ""));
  });

  // ---- GET /api/v1/files ----
  server.route("GET", "/api/v1/files", [&](const net::Request&, net::Response& resp) {
    std::string err;
    std::vector<FileRow> rows;
    if (!repo.listFiles(rows, err)) {
      resp.setError(500, std::string("db failed: ") + err);
      return;
    }
    json::Value arr = json::Value::array();
    for (const FileRow& f : rows) arr.push_back(fileToJson(f));
    json::Value v = json::Value::object();
    v.set("total", static_cast<long long>(rows.size()));
    v.set("items", arr);
    resp.setJson(200, json::dump(v));
  });

  // ---- GET /api/v1/files/:id ----
  server.route("GET", "/api/v1/files/:id", [&](const net::Request& req, net::Response& resp) {
    std::string err;
    std::int64_t id = std::strtoll(req.param("id").c_str(), nullptr, 10);
    FileRow row;
    if (!repo.findById(id, row, err)) {
      resp.setError(404, "file not found");
      return;
    }
    resp.setJson(200, json::dump(fileToJson(row)));
  });

  // ---- GET /api/v1/files/:id/content ----
  server.route("GET", "/api/v1/files/:id/content",
               [&](const net::Request& req, net::Response& resp) {
                 std::string err;
                 std::int64_t id = std::strtoll(req.param("id").c_str(), nullptr, 10);
                 FileRow row;
                 if (!repo.findById(id, row, err)) {
                   resp.setError(404, "file not found");
                   return;
                 }
                 std::vector<std::string> hashes;
                 if (!repo.chunkHashesOf(id, hashes, err)) {
                   resp.setError(500, std::string("db failed: ") + err);
                   return;
                 }
                 std::string content;
                 if (!store.getChunked(hashes, content, err)) {
                   resp.setError(500, std::string("store failed: ") + err);
                   return;
                 }
                 resp.setBinary(200, content, "application/octet-stream");
               });

  if (!server.listen(cfg.listenAddr, cfg.port, cfg.workers, err)) {
    CV_LOG_ERROR("监听失败: " << err);
    return 1;
  }

  CV_LOG_INFO("监听 " << cfg.listenAddr << ":" << cfg.port << "  数据目录 " << cfg.dataDir
                      << "  工作线程 " << cfg.workers);
  CV_LOG_INFO("接口就绪：GET /healthz · POST /api/v1/files · GET /api/v1/files");
  server.runForever();

  CV_LOG_INFO("服务端已停止");
  return 0;
}

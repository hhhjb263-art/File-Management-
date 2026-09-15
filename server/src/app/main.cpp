// 云匣 CloudVault 服务端入口（MVP 骨架）
//
// 今日能力：
//   GET  /healthz                    健康检查
//   POST /api/v1/files               整文件上传（分块落盘 + 秒传命中）
//   GET  /api/v1/files               文件列表
//   GET  /api/v1/files/:id           文件元数据
//   GET  /api/v1/files/:id/content   下载（按分块重组，支持 Range/206）
//   POST /api/v1/uploads/init        分块上传会话初始化（含秒传 / 断点复用）
//   GET  /api/v1/uploads/:id         查询会话状态与已收分块
//   PUT  /api/v1/uploads/:id/chunk/:seq  上传单个分块（幂等）
//   POST /api/v1/uploads/:id/complete     合并分块落库
//   DELETE /api/v1/uploads/:id       取消会话

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/json.h"
#include "core/logger.h"
#include "core/sha256.h"
#include "core/util.h"
#include "meta/db.h"
#include "meta/file_repository.h"
#include "meta/upload_repository.h"
#include "net/http_server.h"
#include "store/content_store.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "0.5.0";
constexpr std::size_t kMaxUploadBytes = 256u * 1024u * 1024u;         // 整文件单请求上限
constexpr std::int64_t kMaxChunkedBytes = 2LL * 1024 * 1024 * 1024;   // 分块总量上限 2GiB
constexpr std::int64_t kMinChunk = 64 * 1024;                         // 最小分块 64KiB
constexpr std::int64_t kMaxChunk = 64 * 1024 * 1024;                  // 最大分块 64MiB
constexpr std::int64_t kGcIdleMillis = 24LL * 3600 * 1000;            // 会话 GC 闲置阈值 24h

cv::json::Value fileToJson(const cv::FileRow& f) {
  cv::json::Value v = cv::json::Value::object();
  v.set("id", static_cast<long long>(f.id));
  v.set("name", f.name);
  v.set("size", static_cast<long long>(f.size));
  v.set("hash", f.contentHash);
  v.set("chunks", static_cast<long long>(f.chunkCount));
  v.set("created_at", static_cast<long long>(f.createdAt));
  return v;
}

// 分块总数：size 为 0 时返回 0（空文件无分块）
std::int64_t totalChunks(std::int64_t size, std::int64_t chunkSize) {
  if (size <= 0 || chunkSize <= 0) return 0;
  return (size + chunkSize - 1) / chunkSize;
}

// 第 seq 分块的期望字节数（末尾分块按剩余字节，其余等于 chunkSize）
std::int64_t expectedChunkSize(std::int64_t size, std::int64_t chunkSize,
                               std::int64_t total, std::int64_t seq) {
  if (total > 0 && seq + 1 == total) return size - seq * chunkSize;
  return chunkSize;
}

// 把整型数组写入 JSON 对象
void addIntArray(cv::json::Value& v, const char* key,
                 const std::vector<std::int64_t>& arr) {
  cv::json::Value a = cv::json::Value::array();
  for (std::int64_t x : arr) a.push_back(cv::json::Value(static_cast<long long>(x)));
  v.set(key, a);
}

// 解析路径参数为非负整数；失败返回 false
bool parseId(const std::string& s, std::int64_t& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  long long v = std::strtoll(s.c_str(), &end, 10);
  if (end == s.c_str() || *end != '\0' || v < 0) return false;
  out = static_cast<std::int64_t>(v);
  return true;
}

// 解析请求 JSON body；失败 err 非空
cv::json::Value parseJsonBody(const std::string& body, std::string& err) {
  cv::json::Value v;
  if (body.empty()) return v;
  if (!cv::json::parse(body, v, err)) return v;
  return v;
}

// 校验 64 位十六进制哈希串
bool validHashHex(const std::string& h) {
  if (h.size() != 64) return false;
  for (char c : h) {
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!ok) return false;
  }
  return true;
}

// 分块临时目录与文件路径：<dataDir>/tmp/uploads/<id>/
std::string uploadTmpDir(const cv::Config& cfg, std::int64_t id) {
  return cfg.tmpRoot() + "/uploads/" + std::to_string(id);
}
std::string partPath(const cv::Config& cfg, std::int64_t id, std::int64_t seq) {
  return uploadTmpDir(cfg, id) + "/" + std::to_string(seq) + ".part";
}

// 原子写文件：先写 .tmp 再 rename，保证落盘一致性
bool writeAtomicFile(const std::string& finalPath, const std::string& data,
                     std::string& err) {
  std::string tmp = finalPath + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      err = "cannot open tmp for write: " + tmp;
      return false;
    }
    if (!data.empty()) {
      out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    out.flush();
    if (!out.good()) {
      err = "write failed: " + tmp;
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, finalPath, ec);
  if (ec) {
    fs::remove(tmp, ec);
    err = "rename failed: " + ec.message();
    return false;
  }
  return true;
}

// 读整个文件到字符串（分块大小有限，内存占用只约一个分块）
bool readWholeFile(const std::string& path, std::string& out, std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    err = "cannot open " + path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

// 以 DB 行 + 磁盘 part 双重校验生成 uploaded 列表（自愈：不一致则剔除 DB 行）。
// 返回 received_bytes（已收分块字节数之和）。
std::vector<std::int64_t> buildUploaded(const cv::Config& cfg,
                                         cv::UploadRepository& up,
                                         const cv::UploadSession& s,
                                         std::int64_t& received, std::string& err) {
  std::vector<cv::UploadChunkRow> rows;
  up.listChunks(s.id, rows, err);
  std::vector<std::int64_t> uploaded;
  received = 0;
  bool healed = false;
  for (const auto& row : rows) {
    std::string p = partPath(cfg, s.id, row.seq);
    std::error_code ec;
    bool ok = fs::exists(p, ec) && fs::file_size(p, ec) == static_cast<std::uintmax_t>(row.size);
    if (!ok) {
      // 自愈：磁盘缺 part 或大小不符 → 删除该 DB 分块记录
      CV_LOG_WARN("自愈：upload_id=" << s.id << " seq=" << row.seq
                                     << " 磁盘分块不一致，剔除元数据");
      up.deleteChunk(s.id, row.seq, err);
      healed = true;
      continue;
    }
    uploaded.push_back(row.seq);
    received += row.size;
  }
  std::sort(uploaded.begin(), uploaded.end());
  if (healed && uploaded.empty()) up.setStatus(s.id, "created", err);
  return uploaded;
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
  UploadRepository up(db);
  net::HttpServer server;

  // ---- 启动期会话 GC：清理闲置超时的未完成会话及其临时文件 ----
  {
    std::vector<std::int64_t> stale;
    std::int64_t threshold = nowMillis() - kGcIdleMillis;
    if (up.listStaleIds(threshold, stale, err)) {
      for (std::int64_t id : stale) {
        std::error_code rec;
        fs::remove_all(uploadTmpDir(cfg, id), rec);
        up.removeSession(id, err);
        CV_LOG_INFO("GC：清理闲置会话 upload_id=" << id);
      }
    } else {
      CV_LOG_WARN("会话 GC 扫描失败: " << err);
    }
  }

  // ---- GET /healthz ----
  server.route("GET", "/healthz", [&](const net::Request&, net::Response& resp) {
    json::Value v = json::Value::object();
    v.set("status", "ok");
    v.set("version", kVersion);
    v.set("data_dir", cfg.dataDir);
    resp.setJson(200, json::dump(v));
  });

  // ---- POST /api/v1/files （整文件上传，行为保持不变）----
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

    FileRow existing;
    bool instant = repo.findByContentHash(contentHash, existing, err);

    std::vector<std::string> chunkHashes;
    std::vector<std::size_t> chunkSizes;
    std::string manifestHash;
    if (!store.putChunked(data, cfg.chunkSize, chunkHashes, chunkSizes, manifestHash, err)) {
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
    std::int64_t id = 0;
    if (!parseId(req.param("id"), id)) {
      resp.setError(404, "file not found");
      return;
    }
    FileRow row;
    if (!repo.findById(id, row, err)) {
      resp.setError(404, "file not found");
      return;
    }
    resp.setJson(200, json::dump(fileToJson(row)));
  });

  // ---- GET /api/v1/files/:id/content （支持 Range/206，按需读取分块）----
  server.route("GET", "/api/v1/files/:id/content",
               [&](const net::Request& req, net::Response& resp) {
                 std::string err;
                 std::int64_t id = 0;
                 if (!parseId(req.param("id"), id)) {
                   resp.setError(404, "file not found");
                   return;
                 }
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
                 const std::int64_t total = row.size;

                 // 解析 Range: bytes=start- / bytes=start-end / bytes=-N（后缀范围）。
                 // 非法形态（无 '-'、含非数字、后缀 N<=0）一律忽略 Range -> 回 200 全文。
                 bool haveRange = false;
                 std::int64_t rstart = 0, rend = total - 1;
                 std::string rh = req.header("range");
                 auto allDigits = [](const std::string& s) {
                   if (s.empty()) return false;
                   for (char c : s) {
                     if (c < '0' || c > '9') return false;
                   }
                   return true;
                 };
                 if (!rh.empty() && rh.rfind("bytes=", 0) == 0) {
                   std::string spec = rh.substr(6);
                   std::size_t dash = spec.find('-');
                   if (dash != std::string::npos) {
                     std::string a = spec.substr(0, dash);
                     std::string b = spec.substr(dash + 1);
                     if (!a.empty() && allDigits(a) && (b.empty() || allDigits(b))) {
                       // bytes=start- / bytes=start-end
                       rstart = std::strtoll(a.c_str(), nullptr, 10);
                       rend = b.empty() ? (total - 1) : std::strtoll(b.c_str(), nullptr, 10);
                       haveRange = true;
                     } else if (a.empty() && allDigits(b)) {
                       // bytes=-N：末尾 N 字节（N 超过文件大小时取整个文件）
                       std::int64_t suffixN = std::strtoll(b.c_str(), nullptr, 10);
                       if (suffixN > 0) {
                         rstart = total - suffixN;
                         if (rstart < 0) rstart = 0;
                         rend = total - 1;
                         haveRange = true;
                       }
                     }
                   }
                 }

                 if (haveRange) {
                   if (total == 0 || rstart < 0 || rstart >= total) {
                     resp.setError(416, "range not satisfiable");
                     return;
                   }
                   if (rend >= total) rend = total - 1;
                   if (rend < rstart) {
                     resp.setError(416, "range not satisfiable");
                     return;
                   }
                   std::int64_t len = rend - rstart + 1;
                   std::string slice;
                   if (!store.readRange(hashes, rstart, len, slice, err)) {
                     resp.setError(500, std::string("store failed: ") + err);
                     return;
                   }
                   resp.status = 206;
                   resp.extraHeaders["Accept-Ranges"] = "bytes";
                   resp.extraHeaders["Content-Range"] =
                       "bytes " + std::to_string(rstart) + "-" + std::to_string(rend) + "/" +
                       std::to_string(total);
                   resp.setBinary(206, slice, "application/octet-stream");
                   CV_LOG_INFO("分块下载 id=" << id << " range=" << rstart << "-" << rend);
                   return;
                 }

                 // 无 Range：全文下载（沿用 getChunked，行为不变）
                 std::string content;
                 if (!store.getChunked(hashes, content, err)) {
                   resp.setError(500, std::string("store failed: ") + err);
                   return;
                 }
                 resp.setBinary(200, content, "application/octet-stream");
               });

  // ---- POST /api/v1/uploads/init ----
  server.route("POST", "/api/v1/uploads/init",
               [&](const net::Request& req, net::Response& resp) {
                 std::string perr;
                 json::Value body = parseJsonBody(req.body, perr);
                 if (!perr.empty()) {
                   resp.setError(400, "invalid json: " + perr);
                   return;
                 }
                 const json::Value* pname = body.find("name");
                 const json::Value* psize = body.find("size");
                 const json::Value* pchunk = body.find("chunk_size");
                 const json::Value* phash = body.find("hash");
                 if (!pname || pname->type() != json::Value::Type::String || !psize) {
                   resp.setError(400, "missing name or size");
                   return;
                 }
                 std::int64_t size = static_cast<std::int64_t>(psize->numberValue());
                 if (size < 0) {
                   resp.setError(400, "invalid size");
                   return;
                 }
                 if (size > kMaxChunkedBytes) {
                   resp.setError(413, "total size exceeds 2GiB limit");
                   return;
                 }
                 // chunk_size 缺省用服务端配置，并 clamp 到 [64KiB, 64MiB]
                 std::int64_t chunkSize =
                     pchunk ? static_cast<std::int64_t>(pchunk->numberValue()) : 0;
                 if (chunkSize <= 0) chunkSize = static_cast<std::int64_t>(cfg.chunkSize);
                 chunkSize = std::max(kMinChunk, std::min(kMaxChunk, chunkSize));

                 std::string name = util::baseName(pname->stringValue());
                 if (name.empty()) name = "unnamed";
                 std::string fileHash =
                     phash && phash->type() == json::Value::Type::String ? phash->stringValue()
                                                                         : std::string();
                 if (!fileHash.empty() && !validHashHex(fileHash)) {
                   resp.setError(400, "invalid hash (expect 64-hex sha256)");
                   return;
                 }

                 auto buildOk = [&](const UploadSession& s) {
                   std::int64_t recv = 0;
                   std::vector<std::int64_t> ul = buildUploaded(cfg, up, s, recv, perr);
                   json::Value v = json::Value::object();
                   v.set("upload_id", static_cast<long long>(s.id));
                   v.set("name", s.name);
                   v.set("size", static_cast<long long>(s.size));
                   v.set("chunk_size", static_cast<long long>(s.chunkSize));
                   v.set("hash", s.fileHash);
                   addIntArray(v, "uploaded", ul);
                   v.set("received_bytes", static_cast<long long>(recv));
                   return v;
                 };

                 // 秒传：file_hash 已在内容库命中 → 直接返回已存在文件
                 if (!fileHash.empty()) {
                   FileRow existing;
                   if (repo.findByContentHash(fileHash, existing, perr)) {
                     json::Value v = json::Value::object();
                     v.set("upload_id", static_cast<long long>(0));
                     v.set("done", true);
                     v.set("file_id", static_cast<long long>(existing.id));
                     v.set("name", name);
                     v.set("size", static_cast<long long>(size));
                     v.set("chunk_size", static_cast<long long>(chunkSize));
                     v.set("hash", fileHash);
                     v.set("uploaded", json::Value::array());
                     v.set("received_bytes", static_cast<long long>(size));
                     resp.setJson(200, json::dump(v));
                     CV_LOG_INFO("init 秒传命中 hash=" << fileHash
                                                      << " file_id=" << existing.id);
                     return;
                   }
                   // 断点复用：同 (file_hash,size,chunk_size) 的未完成会话
                   UploadSession s;
                   if (up.findResumable(fileHash, size, chunkSize, s, perr)) {
                     json::Value v = buildOk(s);
                     resp.setJson(200, json::dump(v));
                     CV_LOG_INFO("init 复用断点 upload_id=" << s.id
                                                           << " hash=" << fileHash);
                     return;
                   }
                 }

                 // 新建会话
                 std::int64_t newId = 0;
                 if (!up.create(name, size, chunkSize, fileHash, newId, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 std::error_code msec;
                 fs::create_directories(uploadTmpDir(cfg, newId), msec);
                 json::Value v = json::Value::object();
                 v.set("upload_id", static_cast<long long>(newId));
                 v.set("name", name);
                 v.set("size", static_cast<long long>(size));
                 v.set("chunk_size", static_cast<long long>(chunkSize));
                 v.set("hash", fileHash);
                 v.set("uploaded", json::Value::array());
                 v.set("received_bytes", static_cast<long long>(0));
                 resp.setJson(200, json::dump(v));
                 CV_LOG_INFO("init 新建会话 upload_id=" << newId << " name=" << name
                                                      << " size=" << size);
               });

  // ---- GET /api/v1/uploads/:id ----
  server.route("GET", "/api/v1/uploads/:id",
               [&](const net::Request& req, net::Response& resp) {
                 std::string perr;
                 std::int64_t id = 0;
                 if (!parseId(req.param("id"), id)) {
                   resp.setError(404, "session not found");
                   return;
                 }
                 UploadSession s;
                 if (!up.findById(id, s, perr)) {
                   resp.setError(404, "session not found");
                   return;
                 }
                 std::int64_t received = 0;
                 std::vector<std::int64_t> ul = buildUploaded(cfg, up, s, received, perr);
                 json::Value v = json::Value::object();
                 v.set("upload_id", static_cast<long long>(s.id));
                 v.set("size", static_cast<long long>(s.size));
                 v.set("chunk_size", static_cast<long long>(s.chunkSize));
                 addIntArray(v, "uploaded", ul);
                 v.set("received_bytes", static_cast<long long>(received));
                 v.set("status", s.status);
                 resp.setJson(200, json::dump(v));
               });

  // ---- PUT /api/v1/uploads/:id/chunk/:seq ----
  server.route("PUT", "/api/v1/uploads/:id/chunk/:seq",
               [&](const net::Request& req, net::Response& resp) {
                 std::string perr;
                 std::int64_t id = 0, seq = 0;
                 if (!parseId(req.param("id"), id) || !parseId(req.param("seq"), seq)) {
                   resp.setError(404, "session or chunk not found");
                   return;
                 }
                 UploadSession s;
                 if (!up.findById(id, s, perr)) {
                   resp.setError(404, "session not found");
                   return;
                 }
                 if (s.status == "completed") {
                   resp.setError(409, "session already completed");
                   return;
                 }
                 std::int64_t total = totalChunks(s.size, s.chunkSize);
                 if (total == 0 || seq < 0 || seq >= total) {
                   resp.setError(400, "chunk seq out of range");
                   return;
                 }
                 const std::string& data = req.body;
                 std::int64_t expect = expectedChunkSize(s.size, s.chunkSize, total, seq);
                 if (static_cast<std::int64_t>(data.size()) != expect) {
                   resp.setError(400, "chunk size mismatch");
                   return;
                 }
                 std::string chunkHash = Sha256::of(data);
                 std::string clientHash = req.header("x-chunk-sha256");
                 if (!clientHash.empty() && clientHash != chunkHash) {
                   resp.setError(400, "chunk hash mismatch");
                   return;
                 }
                 // 原子落盘到 tmp/uploads/<id>/<seq>.part
                 std::error_code msec;
                 fs::create_directories(uploadTmpDir(cfg, id), msec);
                 std::string ppath = partPath(cfg, id, seq);
                 if (!writeAtomicFile(ppath, data, perr)) {
                   resp.setError(500, std::string("write chunk failed: ") + perr);
                   return;
                 }
                 if (!up.putChunk(id, seq, static_cast<std::int64_t>(data.size()), chunkHash, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 if (s.status == "created") up.setStatus(id, "uploading", perr);
                 json::Value v = json::Value::object();
                 v.set("seq", static_cast<long long>(seq));
                 v.set("received_bytes", static_cast<long long>(data.size()));
                 resp.setJson(200, json::dump(v));
                 CV_LOG_INFO("收分块 upload_id=" << id << " seq=" << seq
                                                << " size=" << data.size());
               });

  // ---- POST /api/v1/uploads/:id/complete ----
  server.route("POST", "/api/v1/uploads/:id/complete",
               [&](const net::Request& req, net::Response& resp) {
                 std::string perr;
                 std::int64_t id = 0;
                 if (!parseId(req.param("id"), id)) {
                   resp.setError(404, "session not found");
                   return;
                 }
                 UploadSession s;
                 if (!up.findById(id, s, perr)) {
                   resp.setError(404, "session not found");
                   return;
                 }
                 if (s.status == "completed") {
                   resp.setError(409, "session already completed");
                   return;
                 }
                 std::int64_t total = totalChunks(s.size, s.chunkSize);

                 // P2-3：用 DB+磁盘双重校验（自愈）算出已收分块；缺失则 409（先于任何落库）
                 std::int64_t received = 0;
                 std::vector<std::int64_t> uploaded = buildUploaded(cfg, up, s, received, perr);
                 if (total > 0) {
                   std::vector<std::int64_t> missing;
                   for (std::int64_t i = 0; i < total; ++i) {
                     if (std::find(uploaded.begin(), uploaded.end(), i) == uploaded.end())
                       missing.push_back(i);
                   }
                   if (!missing.empty()) {
                     json::Value v = json::Value::object();
                     addIntArray(v, "missing", missing);
                     resp.setJson(409, json::dump(v));
                     return;
                   }
                 }

                 // P2-4 校验阶段（第一遍，不落库、不产生孤儿 blob）：
                 // 流式算整文件哈希 + 逐块完整性（实测 sha256 与库中 upload_chunk.sha256 比对）
                 Sha256 hasher;
                 std::vector<std::int64_t> badSeqs;  // 真正损坏（哈希不符/读失败）的分块
                 for (std::int64_t seq : uploaded) {
                   std::string p = partPath(cfg, id, seq);
                   std::string data;
                   if (!readWholeFile(p, data, perr)) {
                     badSeqs.push_back(seq);
                     continue;
                   }
                   hasher.update(data);
                   UploadChunkRow cr;
                   bool found = false;
                   up.findChunk(id, seq, cr, found, perr);
                   if (found && !cr.sha256.empty() && cr.sha256 != Sha256::of(data)) {
                     badSeqs.push_back(seq);
                   }
                 }
                 std::string computed = hasher.hex();

                 // 逐块确有损坏：精确回传坏 seq，便于客户端只重传坏块
                 if (!badSeqs.empty()) {
                   std::sort(badSeqs.begin(), badSeqs.end());
                   badSeqs.erase(std::unique(badSeqs.begin(), badSeqs.end()), badSeqs.end());
                   json::Value v = json::Value::object();
                   addIntArray(v, "invalid", badSeqs);
                   resp.setJson(422, json::dump(v));
                   return;
                 }
                 // 各块单检均通过，但整文件哈希与客户端声明不符 → 说明客户端声明的 hash 有误；
                 // 回传空 invalid + reason，避免让客户端对全量 seq 死循环重传
                 if (!s.fileHash.empty() && s.fileHash != computed) {
                   json::Value v = json::Value::object();
                   v.set("invalid", json::Value::array());
                   v.set("reason",
                         "whole-file hash mismatch: client-provided hash is incorrect");
                   resp.setJson(422, json::dump(v));
                   return;
                 }

                 // 落库阶段（第二遍）：校验已通过，按 seq 顺序迁入内容库（内容寻址去重）；
                 // 引用计数由下方 insertFile 统一维护，此处只负责落 blob
                 std::vector<std::string> chunkHashes;
                 std::vector<std::size_t> chunkSizes;
                 for (std::int64_t seq : uploaded) {
                   std::string p = partPath(cfg, id, seq);
                   std::string data;
                   if (!readWholeFile(p, data, perr)) {
                     resp.setError(500, std::string("read chunk failed: ") + perr);
                     return;
                   }
                   UploadChunkRow cr;
                   bool found = false;
                   up.findChunk(id, seq, cr, found, perr);
                   std::string chash =
                       (found && !cr.sha256.empty()) ? cr.sha256 : Sha256::of(data);
                   if (!store.put(chash, data, perr)) {
                     resp.setError(500, std::string("store failed: ") + perr);
                     return;
                   }
                   chunkHashes.push_back(chash);
                   chunkSizes.push_back(static_cast<std::size_t>(data.size()));
                 }
                 std::int64_t fileId = 0;
                 if (!repo.insertFile(s.name, s.size, computed, chunkHashes, chunkSizes,
                                     fileId, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 up.setStatus(id, "completed", perr);
                 // 清理临时分块文件与会话分块记录（会话行保留供查询）
                 std::error_code rec;
                 fs::remove_all(uploadTmpDir(cfg, id), rec);
                 up.deleteChunks(id, perr);

                 json::Value v = json::Value::object();
                 v.set("file_id", static_cast<long long>(fileId));
                 v.set("name", s.name);
                 v.set("size", static_cast<long long>(s.size));
                 v.set("hash", computed);
                 resp.setJson(200, json::dump(v));
                 CV_LOG_INFO("complete upload_id=" << id << " -> file_id=" << fileId
                                                  << " hash=" << computed);
               });

  // ---- DELETE /api/v1/uploads/:id ----
  server.route("DELETE", "/api/v1/uploads/:id",
               [&](const net::Request& req, net::Response& resp) {
                 std::string perr;
                 std::int64_t id = 0;
                 if (!parseId(req.param("id"), id)) {
                   resp.setError(404, "session not found");
                   return;
                 }
                 UploadSession s;
                 if (!up.findById(id, s, perr)) {
                   resp.setError(404, "session not found");
                   return;
                 }
                 std::error_code rec;
                 fs::remove_all(uploadTmpDir(cfg, id), rec);  // 删临时分块
                 if (!up.removeSession(id, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 resp.status = 204;  // 无 body
                 CV_LOG_INFO("取消会话 upload_id=" << id);
               });

  if (!server.listen(cfg.listenAddr, cfg.port, cfg.workers, err)) {
    CV_LOG_ERROR("监听失败: " << err);
    return 1;
  }

  CV_LOG_INFO("监听 " << cfg.listenAddr << ":" << cfg.port << "  数据目录 " << cfg.dataDir
                      << "  工作线程 " << cfg.workers);
  CV_LOG_INFO("接口就绪：GET /healthz · POST /api/v1/files · 分块上传 /api/v1/uploads/*");
  server.runForever();

  CV_LOG_INFO("服务端已停止");
  return 0;
}

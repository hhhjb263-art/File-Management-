// 云匣 CloudVault 服务端入口（MVP 骨架）
//
// 今日能力：
//   GET  /healthz                    健康检查
//   POST /api/v1/files               整文件上传（分块落盘 + 秒传命中；头 X-CV-Dir 指定目录）
//   GET  /api/v1/files               文件列表（含 dir）
//   GET  /api/v1/files/:id           文件元数据
//   GET  /api/v1/files/:id/content   下载（按分块重组，支持 Range/206）
//   POST /api/v1/files/new           新建空文件 {dir,name}（同名 409）
//   POST /api/v1/files/:id/rename    重命名 {name}（同名 409）
//   DELETE /api/v1/files/:id         软删除（移除镜像文件）
//   GET  /api/v1/download?path=      按路径下载（仅限已记录文件，严格越界校验）
//   POST /api/v1/dirs                创建目录（规范化 + 符号链接/越界拒绝）
//   GET  /api/v1/dirs                列出已登记目录
//   GET  /api/v1/storage             磁盘空间自查（free/total 字节）
//
// TLS：--tls-port/--tls-cert/--tls-key 启用 HTTPS（OpenSSL），与 HTTP 双模式并行；
//   GET  /api/v1/tree                嵌套文件树（目录在前/文件在后、name 升序；供客户端树选择）
//   POST /api/v1/uploads/init        分块上传会话初始化（含秒传 / 断点复用；body 可带 dir）
//   GET  /api/v1/uploads/:id         查询会话状态与已收分块
//   PUT  /api/v1/uploads/:id/chunk/:seq  上传单个分块（幂等）
//   POST /api/v1/uploads/:id/complete     合并分块落库（落入 init 登记的目录）
//   DELETE /api/v1/uploads/:id       取消会话

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/json.h"
#include "core/logger.h"
#include "core/path_util.h"
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
// 整文件单请求上限：再大请走分块上传（/api/v1/uploads/*），避免服务端把整个 body 读进内存
constexpr std::size_t kMaxUploadBytes = 64u * 1024u * 1024u;
// 单次响应（无 Range 的整文件下载）上限：超过则要求客户端用 Range 分段拉取，
// 避免服务端 getChunked 把整个文件拼进内存（内存占用与文件大小同阶 → OOM 风险）
constexpr std::int64_t kMaxSingleShotDownload = 8LL * 1024 * 1024;
// 单次 Range 响应跨度上限：即使客户端发 `bytes=0-`（开到文件尾），
// 也只回这么多字节，避免一次把 4GiB+ 文件读进内存；客户端按段继续拉取即可。
constexpr std::int64_t kMaxRangeSpan = 16LL * 1024 * 1024;
// 分块总量上限：1 TiB（仅作 sanity 上限；单块 5 MiB、seq 与 size 全程 int64，
// 故 4 GiB+ 的文件可正常传输。实际瓶颈是磁盘空间与文件系统，而非本上限）
constexpr std::int64_t kMaxChunkedBytes = 1LL * 1024 * 1024 * 1024 * 1024;
constexpr std::int64_t kMinChunk = 64 * 1024;                         // 最小分块 64KiB
constexpr std::int64_t kMaxChunk = 64 * 1024 * 1024;                  // 最大分块 64MiB
constexpr std::int64_t kGcIdleMillis = 24LL * 3600 * 1000;            // 会话 GC 闲置阈值 24h

cv::json::Value fileToJson(const cv::FileRow& f) {
  cv::json::Value v = cv::json::Value::object();
  v.set("id", static_cast<long long>(f.id));
  v.set("name", f.name);
  v.set("dir", f.dir);
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

// ---- 目录树（边界受限的虚拟目录 + 物理镜像）----

// 允许上传/下载的物理根目录：由配置显式给出（默认 <dataDir>/files），
// 不随进程启动目录（cwd）变化；相对路径在这里统一转成绝对路径。
std::string filesRoot(const cv::Config& cfg) {
  std::error_code ec;
  fs::path p(cfg.filesRootDir());
  if (p.is_relative()) {
    p = fs::absolute(p, ec);
  }
  return p.lexically_normal().string();
}

// dir/name 已是规范化相对路径，此处仅做拼接（绝不再接收原始用户路径）
std::string diskFilePath(const cv::Config& cfg, const std::string& dir,
                         const std::string& name) {
  return filesRoot(cfg) + "/" + (dir.empty() ? std::string() : dir + "/") + name;
}

// 从 query string 取参数（已百分号编码，调用方自行 urlDecode）
std::string queryParam(const std::string& query, const std::string& key) {
  std::size_t start = 0;
  while (start <= query.size()) {
    std::size_t amp = query.find('&', start);
    std::string item = query.substr(start, amp == std::string::npos
                                              ? std::string::npos
                                              : amp - start);
    if (!item.empty()) {
      std::size_t eq = item.find('=');
      std::string k = (eq == std::string::npos) ? item : item.substr(0, eq);
      if (k == key) return (eq == std::string::npos) ? std::string() : item.substr(eq + 1);
    }
    if (amp == std::string::npos) break;
    start = amp + 1;
  }
  return "";
}

// 目录/路径参数的统一入口：清洗 + 校验。
// 空 = 根目录（放行，out 置空）；穿越/绝对路径 → 403 越界；
// 非法字符/超长 → 400 非法。返回 false 表示已写响应。
bool sanitizeDirParam(const std::string& raw, std::string& out, cv::net::Response& resp) {
  cv::PathStatus st = cv::sanitizeRelPath(raw, out);
  if (st == cv::PathStatus::Empty) {
    out.clear();
    return true;
  }
  if (st == cv::PathStatus::Ok) return true;
  if (st == cv::PathStatus::Traversal || st == cv::PathStatus::Absolute) {
    resp.setError(403, std::string("path escapes allowed root: ") + cv::pathStatusText(st));
  } else {
    resp.setError(400, std::string("invalid path: ") + cv::pathStatusText(st));
  }
  return false;
}

// 在 rootAbs 下逐级创建 relDir（已规范化）。任何一级若是已存在的符号链接则拒绝；
// 完成后做规范化包含校验，防止符号链接把真实路径引到 root 之外。
bool ensureRealDirUnder(const std::string& rootAbs, const std::string& relDir,
                        std::string& err) {
  fs::path root = fs::path(rootAbs).lexically_normal();
  std::error_code ec;
  fs::create_directories(root, ec);
  if (ec && !fs::is_directory(root)) {
    err = "create root failed: " + ec.message();
    return false;
  }
  fs::path cur = root;
  std::istringstream iss(relDir);
  std::string seg;
  while (std::getline(iss, seg, '/')) {
    if (seg.empty()) continue;
    cur /= seg;
    ec.clear();
    auto st = fs::symlink_status(cur, ec);
    if (!ec && fs::is_symlink(st)) {
      err = "refuse symlink in path: " + cur.string();
      return false;
    }
    if (ec || st.type() == fs::file_type::not_found) {
      std::error_code cec;
      fs::create_directory(cur, cec);
      if (cec && !fs::is_directory(cur, ec)) {
        err = "create dir failed: " + cur.string() + ": " + cec.message();
        return false;
      }
    } else if (!fs::is_directory(st)) {
      err = "path component is not a directory: " + cur.string();
      return false;
    }
  }
  // 包含校验：解析符号链接后的真实路径必须仍位于 root 之下
  std::error_code e1, e2;
  fs::path canonCur = fs::weakly_canonical(cur, e1);
  fs::path canonRoot = fs::weakly_canonical(root, e2);
  if (e1 || e2) {
    err = "canonicalize failed: " + (e1 ? e1.message() : e2.message());
    return false;
  }
  const std::string cs = canonCur.string();
  const std::string rs = canonRoot.string();
  if (cs != rs && cs.rfind(rs + "/", 0) != 0) {
    err = "path escapes allowed root: " + relDir;
    return false;
  }
  return true;
}

// 磁盘剩余空间（字节）；拿不到返回 -1。
// 目录可能尚不存在，向上找最近存在的祖先再取 space()。
std::int64_t diskFreeBytes(const std::string& path) {
  std::error_code ec;
  fs::path p(path);
  while (!p.empty() && !fs::exists(p, ec)) {
    p = p.parent_path();
  }
  if (p.empty()) return -1;
  const fs::space_info sp = fs::space(p, ec);
  if (ec) return -1;
  return static_cast<std::int64_t>(sp.available);
}

// 上传前空间预检：分块上传的 complete 会把 tmp 分块「同盘 rename 搬进」内容库
// （ContentStore::putFromFile），不再产生 tmp+blob 双份；因此实际需要 ≈ 1 倍文件大小
// （另加镜像树 1 倍，若空间不够会跳过镜像）。空间不足 → 507，并给出 need/free。
constexpr std::int64_t kSpaceSafetyFactor = 1;
constexpr std::int64_t kSpaceMarginBytes = 64LL * 1024 * 1024;

// 剩余空间是否不足以镜像一份 size 字节的文件（不足则跳过镜像，避免把盘写满）
bool freeSpaceBelow(const cv::Config& cfg, std::int64_t size) {
  const std::int64_t freeBytes = diskFreeBytes(cfg.dataDir);
  return freeBytes >= 0 && freeBytes < size + kSpaceMarginBytes;
}

bool checkSpaceForUpload(const cv::Config& cfg, std::int64_t size,
                         cv::net::Response& resp) {
  const std::int64_t need = size * kSpaceSafetyFactor + kSpaceMarginBytes;
  const std::int64_t freeBytes = diskFreeBytes(cfg.dataDir);
  if (freeBytes < 0) {
    return true;   // 拿不到空间信息就不拦，交给实际写入报错
  }
  if (freeBytes < need) {
    cv::json::Value v = cv::json::Value::object();
    v.set("error", "insufficient disk space on server");
    v.set("need_bytes", static_cast<long long>(need));
    v.set("free_bytes", static_cast<long long>(freeBytes));
    v.set("data_dir", cfg.dataDir);
    resp.setJson(507, cv::json::dump(v));
    CV_LOG_WARN("空间不足：需要 " << need << " 字节，剩余 " << freeBytes << " 字节（"
                                 << cfg.dataDir << "）");
    return false;
  }
  return true;
}

// 删除一批「引用计数归零」的 blob，返回实际释放的字节数（失败仅告警，不阻断主流程）
std::int64_t dropOrphanBlobs(cv::ContentStore& store,
                             const std::vector<std::string>& orphanHashes) {
  std::int64_t freed = 0;
  for (const std::string& h : orphanHashes) {
    std::error_code sec;
    const auto sz = fs::file_size(store.pathOf(h), sec);
    std::string derr;
    if (store.drop(h, derr)) {
      if (!sec) freed += static_cast<std::int64_t>(sz);
    } else {
      CV_LOG_WARN("删除 blob 失败 " << h << ": " << derr);
    }
  }
  return freed;
}

// 按文件行提供内容（整文件 / Range 分块复用）。调用方已完成鉴权与存在性检查。
void serveFileContent(cv::FileRepository& repo, cv::ContentStore& store, const cv::Config& cfg,
                      const cv::FileRow& row,
                      const cv::net::Request& req, cv::net::Response& resp) {
  std::string err;
  std::vector<std::string> hashes;
  if (!repo.chunkHashesOf(row.id, hashes, err)) {
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
    // 跨度保护：`bytes=0-` 这类开放区间也只回 kMaxRangeSpan 字节（客户端按段续拉）
    if (rend - rstart + 1 > kMaxRangeSpan) {
      rend = rstart + kMaxRangeSpan - 1;
    }
    std::int64_t len = rend - rstart + 1;

    // 快路径：文件树镜像存在且大小吻合 → 直接顺序读该文件。
    // 镜像是完整连续文件，比分块拼装（开多个 blob、seek、拷贝拼接）快得多，
    // 也对操作系统预读 / 页缓存友好。镜像可能缺失（空间不足被跳过 / 旧版本上传），
    // 此时回退到按分块拼装，行为不变。
    std::string slice;
    bool servedFromMirror = false;
    {
      const std::string mirrorPath = diskFilePath(cfg, row.dir, row.name);
      std::error_code mec;
      if (fs::file_size(mirrorPath, mec) == static_cast<std::uintmax_t>(total) && !mec) {
        std::ifstream in(mirrorPath, std::ios::binary);
        if (in.is_open()) {
          in.seekg(rstart);
          slice.resize(static_cast<std::size_t>(len));
          in.read(slice.data(), len);   // C++17：data() 可写
          const auto got = in.gcount();
          if (got == len) {
            servedFromMirror = true;
          } else {
            slice.clear();   // 读取不完整（镜像被动过？）→ 回退分块拼装
          }
        }
      }
    }
    if (!servedFromMirror) {
      if (!store.readRange(hashes, rstart, len, slice, err)) {
        resp.setError(500, std::string("store failed: ") + err);
        return;
      }
    }
    resp.status = 206;
    resp.extraHeaders["Accept-Ranges"] = "bytes";
    resp.extraHeaders["Content-Range"] =
        "bytes " + std::to_string(rstart) + "-" + std::to_string(rend) + "/" +
        std::to_string(total);
    resp.setBinary(206, slice, "application/octet-stream");
    CV_LOG_INFO("分块下载 id=" << row.id << " range=" << rstart << "-" << rend
                               << (servedFromMirror ? " [镜像直读]" : " [分块拼装]"));
    return;
  }

  // 无 Range：整文件下载。大文件拒绝一次性回发，要求客户端用 Range 分段拉取，
  // 否则服务端 getChunked 会把整个文件拼进内存（内存占用 ~ 文件大小）。
  if (total > kMaxSingleShotDownload) {
    cv::json::Value v = cv::json::Value::object();
    v.set("error", "file too large for single-shot download; use Range requests");
    v.set("size", static_cast<long long>(total));
    v.set("max_single_shot", static_cast<long long>(kMaxSingleShotDownload));
    resp.setJson(409, cv::json::dump(v));
    return;
  }
  std::string content;
  if (!store.getChunked(hashes, content, err)) {
    resp.setError(500, std::string("store failed: ") + err);
    return;
  }
  resp.setBinary(200, content, "application/octet-stream");
}

// ---- 文件树（GET /api/v1/tree）----

// 目录/文件树节点：目录为容器，文件为叶子
struct TreeNode {
  bool isDir = true;                          // true=目录 false=文件
  std::string name;                           // 末级段名（根目录不进入树节点）
  std::string path;                           // 规范化相对路径（目录=dir；文件=dir/name）
  std::int64_t size = 0;
  std::int64_t id = 0;
  std::vector<std::string> children;          // 子节点 key：目录="D:"+path，文件="F:"+path
};

// 取父目录路径；'' 表示根（根无父目录节点）
std::string treeParentPath(const std::string& p) {
  if (p.empty()) return "";
  std::size_t slash = p.find_last_of('/');
  return (slash == std::string::npos) ? std::string() : p.substr(0, slash);
}

// 取末级段名
std::string treeLastSegment(const std::string& p) {
  if (p.empty()) return "";
  std::size_t slash = p.find_last_of('/');
  return (slash == std::string::npos) ? p : p.substr(slash + 1);
}

// 子节点排序：目录在前、文件在后；同类按 name 升序（字典序）
bool treeChildLess(const std::map<std::string, TreeNode>& nodes, const std::string& ka,
                   const std::string& kb) {
  const TreeNode& a = nodes.at(ka);
  const TreeNode& b = nodes.at(kb);
  if (a.isDir != b.isDir) return a.isDir;     // dir(true) < file(false)
  return a.name < b.name;
}

// 以 DB 行（dir_node 全量 + file_node/file_dir）在内存按路径段建树。
// key 命名空间隔离：目录="D:"+path，文件="F:"+path，避免同名目录/文件 key 冲突。
void buildFileTree(cv::FileRepository& repo, cv::json::Value& rootArr, std::int64_t& totalDirs,
                   std::int64_t& totalFiles, std::string& err) {
  std::vector<std::string> dirs;
  if (!repo.listDirsAll(dirs, err)) return;
  std::vector<cv::FileRow> files;
  if (!repo.listFiles(files, err)) return;

  std::map<std::string, TreeNode> nodes;
  // 1) 目录节点（根 '' 不建节点，由 root 数组表达）
  for (const std::string& d : dirs) {
    if (d.empty()) continue;
    TreeNode n;
    n.isDir = true;
    n.path = d;
    n.name = treeLastSegment(d);
    nodes["D:" + d] = n;
  }
  // 2) 文件节点（并补齐其父目录节点，防 listDirsAll 漏网）
  for (const cv::FileRow& f : files) {
    std::string fpath = f.dir.empty() ? f.name : f.dir + "/" + f.name;
    TreeNode fn;
    fn.isDir = false;
    fn.path = fpath;
    fn.name = f.name;
    fn.size = f.size;
    fn.id = f.id;
    nodes["F:" + fpath] = fn;
    if (!f.dir.empty()) {
      std::string key = "D:" + f.dir;
      if (nodes.find(key) == nodes.end()) {
        TreeNode dn;
        dn.isDir = true;
        dn.path = f.dir;
        dn.name = treeLastSegment(f.dir);
        nodes[key] = dn;
      }
    }
  }
  // 3) 建立父子关系（父目录路径 = parentPath(path)；空父即根级）
  std::vector<std::string> rootKeys;
  for (const auto& kv : nodes) {
    const TreeNode& n = kv.second;
    std::string par = treeParentPath(n.path);
    if (par.empty()) {
      rootKeys.push_back(kv.first);
    } else {
      auto it = nodes.find("D:" + par);
      if (it != nodes.end()) it->second.children.push_back(kv.first);
      else rootKeys.push_back(kv.first);      // 父目录缺失兜底（理论上不发生）
    }
  }

  totalDirs = 0;
  for (const auto& kv : nodes) if (kv.second.isDir) ++totalDirs;
  totalFiles = static_cast<std::int64_t>(files.size());

  // 递归序列化（dirs-first / name-asc）
  std::function<cv::json::Value(const std::string&)> serialize =
      [&](const std::string& key) -> cv::json::Value {
    const TreeNode& n = nodes.at(key);
    cv::json::Value v = cv::json::Value::object();
    if (n.isDir) {
      v.set("type", "dir");
      v.set("name", n.name);
      v.set("path", n.path);
      std::vector<std::string> sorted = n.children;
      std::sort(sorted.begin(), sorted.end(),
                [&](const std::string& ka, const std::string& kb) {
                  return treeChildLess(nodes, ka, kb);
                });
      cv::json::Value children = cv::json::Value::array();
      for (const std::string& ck : sorted) children.push_back(serialize(ck));
      v.set("children", children);
    } else {
      v.set("type", "file");
      v.set("name", n.name);
      v.set("path", n.path);
      v.set("size", static_cast<long long>(n.size));
      v.set("id", static_cast<long long>(n.id));
    }
    return v;
  };

  std::sort(rootKeys.begin(), rootKeys.end(),
            [&](const std::string& ka, const std::string& kb) {
              return treeChildLess(nodes, ka, kb);
            });
  rootArr = cv::json::Value::array();
  for (const std::string& k : rootKeys) rootArr.push_back(serialize(k));
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
  fs::create_directories(filesRoot(cfg), ec);   // 目录树物理根
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
    v.set("files_root", filesRoot(cfg));   // 客户端可操作的目录树根（便于自查）
    std::error_code sec;
    const fs::space_info sinfo = fs::space(cfg.dataDir, sec);
    v.set("disk_free_bytes",
          static_cast<long long>(sec ? -1 : static_cast<std::int64_t>(sinfo.available)));
    v.set("disk_total_bytes",
          static_cast<long long>(sec ? 0 : static_cast<std::int64_t>(sinfo.capacity)));
    v.set("tls_port", static_cast<long long>(cfg.tlsPort));
    resp.setJson(200, json::dump(v));
  });

  // ---- POST /api/v1/files （整文件上传；头 X-CV-Dir 指定目标目录）----
  server.route("POST", "/api/v1/files", [&](const net::Request& req, net::Response& resp) {
    std::string err;
    if (req.body.size() > kMaxUploadBytes) {
      json::Value v = json::Value::object();
      v.set("error", "file too large for single-shot upload; use POST /api/v1/uploads/init (chunked)");
      v.set("max_single_shot", static_cast<long long>(kMaxUploadBytes));
      resp.setJson(413, json::dump(v));
      return;
    }
    if (!checkSpaceForUpload(cfg, static_cast<std::int64_t>(req.body.size()), resp)) {
      return;
    }
    std::string name = util::baseName(util::urlDecode(req.header("x-cv-name")));
    if (name.empty()) name = util::baseName(util::urlDecode(req.param("name")));
    if (name.empty()) name = "unnamed";

    // 目标目录：空 = 根目录；非法/越界直接拒绝
    std::string dir;
    if (!sanitizeDirParam(util::urlDecode(req.header("x-cv-dir")), dir, resp)) return;

    // 同名检测：同目录下已有同名文件 → 未声明覆盖则 409，让客户端询问用户
    // （X-CV-Overwrite: 1 表示用户已确认覆盖）
    const bool overwrite = !req.header("x-cv-overwrite").empty();
    FileRow sameName;
    const bool nameTaken = repo.findByPath(dir, name, sameName, err);
    if (nameTaken && !overwrite) {
      json::Value v = json::Value::object();
      v.set("error", "name exists in target directory");
      v.set("exists", true);
      v.set("file_id", static_cast<long long>(sameName.id));
      v.set("name", name);
      v.set("dir", dir);
      resp.setJson(409, json::dump(v));
      return;
    }

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
    if (nameTaken) {
      // 覆盖：保留原 id 与名称，替换内容与分块清单
      id = sameName.id;
      std::vector<std::string> orphans;
      if (!repo.replaceContent(id, static_cast<std::int64_t>(data.size()), contentHash,
                               chunkHashes, chunkSizes, orphans, err)) {
        resp.setError(500, std::string("db failed: ") + err);
        return;
      }
      const std::int64_t freed = dropOrphanBlobs(store, orphans);
      if (freed > 0) {
        CV_LOG_INFO("覆盖释放旧内容 blob " << orphans.size() << " 个，" << freed << " 字节");
      }
    } else if (!repo.insertFile(name, dir, static_cast<std::int64_t>(data.size()), contentHash,
                                chunkHashes, chunkSizes, id, err)) {
      resp.setError(500, std::string("db failed: ") + err);
      return;
    }

    // 物理镜像树（尽力而为；内容仍以 blob + DB 为权威，失败仅告警）
    std::string mirrorErr;
    if (!store.materializeFromChunks(chunkHashes, diskFilePath(cfg, dir, name), mirrorErr)) {
      CV_LOG_WARN("镜像文件树失败 id=" << id << ": " << mirrorErr);
    }

    json::Value v = json::Value::object();
    v.set("id", static_cast<long long>(id));
    v.set("name", name);
    v.set("dir", dir);
    v.set("size", static_cast<long long>(data.size()));
    v.set("hash", contentHash);
    v.set("chunks", static_cast<long long>(chunkHashes.size()));
    v.set("instant", instant);
    v.set("overwritten", nameTaken);
    resp.setJson(nameTaken ? 200 : 201, json::dump(v));
    CV_LOG_INFO("上传 id=" << id << " dir=" << dir << " name=" << name << " size=" << data.size()
                           << (nameTaken ? " [覆盖同名]" : "")
                           << (instant ? " [秒传命中]" : ""));
  });

  // ---- POST /api/v1/files/new （新建空文件；含同名检测）----
  server.route("POST", "/api/v1/files/new", [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    json::Value body = parseJsonBody(req.body, perr);
    if (!perr.empty()) {
      resp.setError(400, "invalid json: " + perr);
      return;
    }
    const json::Value* pname = body.find("name");
    if (!pname || pname->type() != json::Value::Type::String || pname->stringValue().empty()) {
      resp.setError(400, "missing name");
      return;
    }
    // 名称必须是单个合法路径段（复用统一清洗规则）
    std::string rel;
    const cv::PathStatus st = cv::sanitizeRelPath(pname->stringValue(), rel);
    if (st != cv::PathStatus::Ok || rel.find('/') != std::string::npos) {
      resp.setError(400, std::string("invalid file name: ") + cv::pathStatusText(st));
      return;
    }
    const std::string name = rel;

    const json::Value* pdir = body.find("dir");
    std::string dir;
    if (!sanitizeDirParam(pdir && pdir->type() == json::Value::Type::String
                              ? pdir->stringValue()
                              : std::string(),
                          dir, resp)) {
      return;
    }

    FileRow same;
    if (repo.findByPath(dir, name, same, perr)) {
      json::Value v = json::Value::object();
      v.set("error", "name exists in target directory");
      v.set("exists", true);
      v.set("file_id", static_cast<long long>(same.id));
      v.set("name", name);
      v.set("dir", dir);
      resp.setJson(409, json::dump(v));
      return;
    }

    // 空文件内容：sha256("") 作为内容哈希，落一个 0 字节 blob
    const std::string emptyData;
    const std::string emptyHash = Sha256::of(emptyData);
    if (!store.put(emptyHash, emptyData, perr)) {
      resp.setError(500, std::string("store failed: ") + perr);
      return;
    }
    std::int64_t id = 0;
    if (!repo.insertFile(name, dir, 0, emptyHash, {}, {}, id, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    std::string mirrorErr;
    if (!store.materializeFromChunks({}, diskFilePath(cfg, dir, name), mirrorErr)) {
      CV_LOG_WARN("镜像文件树失败 id=" << id << ": " << mirrorErr);
    }
    json::Value v = json::Value::object();
    v.set("id", static_cast<long long>(id));
    v.set("name", name);
    v.set("dir", dir);
    v.set("size", static_cast<long long>(0));
    v.set("hash", emptyHash);
    v.set("chunks", static_cast<long long>(0));
    resp.setJson(201, json::dump(v));
    CV_LOG_INFO("新建空文件 id=" << id << " dir=" << dir << " name=" << name);
  });

  // ---- POST /api/v1/files/:id/rename （重命名；含同名检测）----
  server.route("POST", "/api/v1/files/:id/rename",
               [&](const net::Request& req, net::Response& resp) {
                 std::string perr;
                 std::int64_t id = 0;
                 if (!parseId(req.param("id"), id)) {
                   resp.setError(404, "file not found");
                   return;
                 }
                 FileRow row;
                 if (!repo.findById(id, row, perr)) {
                   resp.setError(404, "file not found");
                   return;
                 }
                 json::Value body = parseJsonBody(req.body, perr);
                 if (!perr.empty()) {
                   resp.setError(400, "invalid json: " + perr);
                   return;
                 }
                 const json::Value* pname = body.find("name");
                 if (!pname || pname->type() != json::Value::Type::String ||
                     pname->stringValue().empty()) {
                   resp.setError(400, "missing name");
                   return;
                 }
                 std::string rel;
                 const cv::PathStatus st = cv::sanitizeRelPath(pname->stringValue(), rel);
                 if (st != cv::PathStatus::Ok || rel.find('/') != std::string::npos) {
                   resp.setError(400, std::string("invalid file name: ") + cv::pathStatusText(st));
                   return;
                 }
                 const std::string newName = rel;
                 if (newName == row.name) {
                   json::Value v = json::Value::object();
                   v.set("id", static_cast<long long>(id));
                   v.set("name", row.name);
                   v.set("dir", row.dir);
                   resp.setJson(200, json::dump(v));   // 同名 → 幂等成功
                   return;
                 }
                 bool exists = false;
                 if (!repo.nameExists(row.dir, newName, id, exists, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 if (exists) {
                   json::Value v = json::Value::object();
                   v.set("error", "name exists in target directory");
                   v.set("exists", true);
                   v.set("name", newName);
                   v.set("dir", row.dir);
                   resp.setJson(409, json::dump(v));
                   return;
                 }
                 if (!repo.renameFile(id, newName, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 // 物理镜像同步改名（尽力而为；失败则按新名重建）
                 std::error_code rec;
                 fs::rename(diskFilePath(cfg, row.dir, row.name),
                            diskFilePath(cfg, row.dir, newName), rec);
                 if (rec) {
                   std::string mirrorErr;
                   std::vector<std::string> chunkHashes;
                   if (repo.chunkHashesOf(row.id, chunkHashes, perr)) {
                     store.materializeFromChunks(chunkHashes,
                                                 diskFilePath(cfg, row.dir, newName),
                                                 mirrorErr);
                   }
                 }
                 json::Value v = json::Value::object();
                 v.set("id", static_cast<long long>(id));
                 v.set("name", newName);
                 v.set("dir", row.dir);
                 resp.setJson(200, json::dump(v));
                 CV_LOG_INFO("重命名 id=" << id << " " << row.name << " -> " << newName);
               });

  // ---- DELETE /api/v1/files/:id （软删除 + 删镜像）----
  server.route("DELETE", "/api/v1/files/:id",
               [&](const net::Request& req, net::Response& resp) {
                 std::string perr;
                 std::int64_t id = 0;
                 if (!parseId(req.param("id"), id)) {
                   resp.setError(404, "file not found");
                   return;
                 }
                 FileRow row;
                 if (!repo.findById(id, row, perr)) {
                   resp.setError(404, "file not found");
                   return;
                 }
                 std::vector<std::string> orphans;
                 if (!repo.softDelete(id, orphans, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 // 释放磁盘空间：删镜像 + 删引用计数归零的 blob
                 std::error_code rec;
                 const auto mirrorSize = fs::file_size(diskFilePath(cfg, row.dir, row.name), rec);
                 fs::remove(diskFilePath(cfg, row.dir, row.name), rec);
                 const std::int64_t freedBlob = dropOrphanBlobs(store, orphans);
                 const std::int64_t freedMirror = rec ? 0 : static_cast<std::int64_t>(mirrorSize);
                 json::Value v = json::Value::object();
                 v.set("deleted", true);
                 v.set("file_id", static_cast<long long>(id));
                 v.set("name", row.name);
                 v.set("dir", row.dir);
                 v.set("freed_bytes", static_cast<long long>(freedBlob + freedMirror));
                 v.set("blobs_removed", static_cast<long long>(orphans.size()));
                 v.set("disk_free_bytes",
                       static_cast<long long>(diskFreeBytes(cfg.dataDir)));
                 resp.setJson(200, json::dump(v));
                 CV_LOG_INFO("删除文件 id=" << id << " dir=" << row.dir << " name=" << row.name
                                            << " 释放 " << (freedBlob + freedMirror)
                                            << " 字节（blob " << orphans.size() << " 个）");
               });

  // ---- GET /api/v1/storage （磁盘空间自查）----
  server.route("GET", "/api/v1/storage", [&](const net::Request&, net::Response& resp) {
    std::error_code ec;
    const std::int64_t freeBytes = diskFreeBytes(cfg.dataDir);
    std::int64_t totalBytes = 0;
    const fs::space_info sp = fs::space(cfg.dataDir, ec);
    if (!ec) {
      totalBytes = static_cast<std::int64_t>(sp.capacity);
    }
    json::Value v = json::Value::object();
    v.set("data_dir", cfg.dataDir);
    v.set("files_root", filesRoot(cfg));
    v.set("free_bytes", static_cast<long long>(freeBytes));
    v.set("total_bytes", static_cast<long long>(totalBytes));
    v.set("upload_safety_factor", static_cast<long long>(kSpaceSafetyFactor));
    resp.setJson(200, json::dump(v));
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
                 serveFileContent(repo, store, cfg, row, req, resp);
               });

  // ---- GET /api/v1/download?path=dir/name （按路径下载，严格越界校验）----
  // 只服务"已记录且位于允许目录树内"的文件；路径先清洗，再按 (dir,name) 查 DB，
  // 用户路径从不直接拼接磁盘路径；镜像树若存在则追加一次符号链接包含校验。
  server.route("GET", "/api/v1/download",
               [&](const net::Request& req, net::Response& resp) {
                 std::string err;
                 std::string raw = req.header("x-cv-path");
                 if (raw.empty()) raw = queryParam(req.query, "path");
                 std::string norm;
                 if (!sanitizeDirParam(util::urlDecode(raw), norm, resp)) return;
                 if (norm.empty()) {
                   resp.setError(400, "missing path (expect dir/name, root has no files)");
                   return;
                 }
                 std::size_t slash = norm.find_last_of('/');
                 std::string dir = (slash == std::string::npos) ? std::string()
                                                                : norm.substr(0, slash);
                 std::string name =
                     (slash == std::string::npos) ? norm : norm.substr(slash + 1);
                 FileRow row;
                 if (!repo.findByPath(dir, name, row, err)) {
                   resp.setError(404,
                                 "file not found under allowed root (path rejected or "
                                 "not recorded)");
                   return;
                 }
                 // 防御纵深：镜像树中该文件若存在，校验其真实路径未逃出允许根
                 std::string disk = diskFilePath(cfg, row.dir, row.name);
                 std::error_code ec;
                 if (fs::symlink_status(disk, ec).type() == fs::file_type::symlink) {
                   resp.setError(403, "refuse symlink in file tree");
                   return;
                 }
                 if (fs::exists(disk, ec)) {
                   std::error_code e1, e2;
                   fs::path canon = fs::weakly_canonical(disk, e1);
                   fs::path root = fs::weakly_canonical(filesRoot(cfg), e2);
                   if (!e1 && !e2) {
                     const std::string cs = canon.string();
                     const std::string rs = root.string();
                     if (cs != rs && cs.rfind(rs + "/", 0) != 0) {
                       resp.setError(403, "path escapes allowed root");
                       return;
                     }
                   }
                 }
                 serveFileContent(repo, store, cfg, row, req, resp);
                 CV_LOG_INFO("按路径下载 " << norm << " -> file_id=" << row.id);
               });

  // ---- POST /api/v1/dirs （创建目录，边界受限）----
  server.route("POST", "/api/v1/dirs",
               [&](const net::Request& req, net::Response& resp) {
                 std::string perr;
                 json::Value body = parseJsonBody(req.body, perr);
                 if (!perr.empty()) {
                   resp.setError(400, "invalid json: " + perr);
                   return;
                 }
                 const json::Value* ppath = body.find("path");
                 if (!ppath || ppath->type() != json::Value::Type::String) {
                   resp.setError(400, "missing path");
                   return;
                 }
                 std::string dir;
                 if (!sanitizeDirParam(ppath->stringValue(), dir, resp)) return;
                 if (dir.empty()) {
                   resp.setError(400, "empty path (root always exists)");
                   return;
                 }
                 // 物理侧：逐级创建，任何一级符号链接/逃逸都拒绝
                 std::string ferr;
                 if (!ensureRealDirUnder(filesRoot(cfg), dir, ferr)) {
                   resp.setError(403, ferr);
                   return;
                 }
                 // 元数据侧：幂等登记
                 bool created = false;
                 if (!repo.createDir(dir, created, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 json::Value v = json::Value::object();
                 v.set("path", dir);
                 v.set("created", created);
                 v.set("exists", !created);
                 resp.setJson(created ? 201 : 200, json::dump(v));
                 CV_LOG_INFO((created ? "创建目录 " : "目录已存在 ") << dir);
               });

  // ---- GET /api/v1/dirs （列出已登记目录）----
  server.route("GET", "/api/v1/dirs", [&](const net::Request&, net::Response& resp) {
    std::string err;
    std::vector<std::string> dirs;
    if (!repo.listDirs(dirs, err)) {
      resp.setError(500, std::string("db failed: ") + err);
      return;
    }
    json::Value arr = json::Value::array();
    for (const std::string& d : dirs) arr.push_back(json::Value(d));
    json::Value v = json::Value::object();
    v.set("total", static_cast<long long>(dirs.size()));
    v.set("items", arr);
    resp.setJson(200, json::dump(v));
  });

  // ---- GET /api/v1/tree （嵌套文件树，供客户端文件树选择对话框）----
  // 不鉴权、不分页；从 dir_node + file_node/file_dir 在内存按路径段建树，
  // 目录在前文件在后、同类 name 升序；空目录以 children:[] 出现；路径为规范化相对路径（'/' 分隔）。
  server.route("GET", "/api/v1/tree", [&](const net::Request&, net::Response& resp) {
    std::string err;
    json::Value rootArr;
    std::int64_t totalDirs = 0, totalFiles = 0;
    buildFileTree(repo, rootArr, totalDirs, totalFiles, err);
    if (!err.empty()) {
      resp.setError(500, std::string("db failed: ") + err);
      return;
    }
    json::Value v = json::Value::object();
    v.set("total_dirs", static_cast<long long>(totalDirs));
    v.set("total_files", static_cast<long long>(totalFiles));
    v.set("root", rootArr);
    resp.setJson(200, json::dump(v));
    CV_LOG_INFO("列出文件树 dirs=" << totalDirs << " files=" << totalFiles);
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
                   json::Value v = json::Value::object();
                   v.set("error", "total size exceeds server chunked-upload limit");
                   v.set("size", static_cast<long long>(size));
                   v.set("max_chunked_bytes", static_cast<long long>(kMaxChunkedBytes));
                   resp.setJson(413, json::dump(v));
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
                 // 目标目录（可选）：空 = 根目录；非法/越界直接拒绝
                 std::string dir;
                 const json::Value* pdir = body.find("dir");
                 std::string dirRaw =
                     (pdir && pdir->type() == json::Value::Type::String)
                         ? pdir->stringValue()
                         : std::string();
                 if (!sanitizeDirParam(dirRaw, dir, resp)) return;

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

                 // 同名检测：目标目录下已存在同名文件 → 未声明覆盖则 409（客户端询问用户后重发）
                 bool overwrite = false;
                 const json::Value* pov = body.find("overwrite");
                 if (pov && pov->type() == json::Value::Type::Bool) {
                   overwrite = pov->boolValue();
                 }
                 {
                   FileRow same;
                   if (repo.findByPath(dir, name, same, perr) && !overwrite) {
                     json::Value v = json::Value::object();
                     v.set("error", "name exists in target directory");
                     v.set("exists", true);
                     v.set("file_id", static_cast<long long>(same.id));
                     v.set("name", name);
                     v.set("dir", dir);
                     resp.setJson(409, json::dump(v));
                     return;
                   }
                 }

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

                 // 新建会话前才做空间预检（秒传命中 / 断点复用都不额外占盘）
                 if (!checkSpaceForUpload(cfg, size, resp)) {
                   return;
                 }
                 // 新建会话
                 std::int64_t newId = 0;
                 if (!up.create(name, size, chunkSize, fileHash, newId, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 up.setDir(newId, dir, perr);   // 登记目标目录，complete 时取用
                 up.setOverwrite(newId, overwrite, perr);   // 登记覆盖标志
                 std::error_code msec;
                 fs::create_directories(uploadTmpDir(cfg, newId), msec);
                 json::Value v = json::Value::object();
                 v.set("upload_id", static_cast<long long>(newId));
                 v.set("name", name);
                 v.set("dir", dir);
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
                   UploadChunkRow cr;
                   bool found = false;
                   up.findChunk(id, seq, cr, found, perr);
                   const std::string part = partPath(cfg, id, seq);
                   if (found && !cr.sha256.empty()) {
                     // 正常路径：分块已在上一步校验过，这里直接「搬移」进内容库
                     // （同盘 rename → 零拷贝，不再 tmp+blob 双份占盘）
                     std::error_code sec;
                     const auto fsize = fs::file_size(part, sec);
                     if (!store.putFromFile(cr.sha256, part, perr)) {
                       resp.setError(500, std::string("store failed: ") + perr);
                       return;
                     }
                     chunkHashes.push_back(cr.sha256);
                     chunkSizes.push_back(sec ? 0 : static_cast<std::size_t>(fsize));
                     continue;
                   }
                   // 异常兜底（库中无分块记录）：读出内容算哈希后入库
                   std::string data;
                   if (!readWholeFile(part, data, perr)) {
                     resp.setError(500, std::string("read chunk failed: ") + perr);
                     return;
                   }
                   const std::string chash = Sha256::of(data);
                   if (!store.put(chash, data, perr)) {
                     resp.setError(500, std::string("store failed: ") + perr);
                     return;
                   }
                   chunkHashes.push_back(chash);
                   chunkSizes.push_back(static_cast<std::size_t>(data.size()));
                 }
                 std::int64_t fileId = 0;
                 // 目标目录：init 时登记（upload_dir 表），读取失败按根目录兜底
                 std::string dir;
                 up.getDir(id, dir, perr);
                 bool overwrite = false;
                 up.getOverwrite(id, overwrite, perr);
                 perr.clear();
                 // 覆盖：同目录同名已存在且会话声明了 overwrite → 替换原记录内容
                 FileRow sameName;
                 bool replace = overwrite && repo.findByPath(dir, s.name, sameName, perr);
                 perr.clear();
                 if (replace) {
                   fileId = sameName.id;
                   std::vector<std::string> orphans;
                   if (!repo.replaceContent(fileId, s.size, computed, chunkHashes, chunkSizes,
                                            orphans, perr)) {
                     resp.setError(500, std::string("db failed: ") + perr);
                     return;
                   }
                   const std::int64_t freed = dropOrphanBlobs(store, orphans);
                   if (freed > 0) {
                     CV_LOG_INFO("覆盖释放旧内容 blob " << orphans.size() << " 个，" << freed
                                                        << " 字节");
                   }
                 } else if (!repo.insertFile(s.name, dir, s.size, computed, chunkHashes,
                                            chunkSizes, fileId, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 // 物理镜像树（尽力而为；空间不足则跳过，失败仅告警，内容仍以 blob + DB 为权威）
                 std::string mirrorErr;
                 if (freeSpaceBelow(cfg, static_cast<std::int64_t>(s.size))) {
                   CV_LOG_WARN("跳过镜像（空间不足）file_id=" << fileId);
                 } else if (!store.materializeFromChunks(chunkHashes,
                                                        diskFilePath(cfg, dir, s.name),
                                                        mirrorErr)) {
                   CV_LOG_WARN("镜像文件树失败 file_id=" << fileId << ": " << mirrorErr);
                 }
                 up.setStatus(id, "completed", perr);
                 // 清理临时分块文件与会话分块记录（会话行保留供查询）
                 std::error_code rec;
                 fs::remove_all(uploadTmpDir(cfg, id), rec);
                 up.deleteChunks(id, perr);

                 json::Value v = json::Value::object();
                 v.set("file_id", static_cast<long long>(fileId));
                 v.set("name", s.name);
                 v.set("dir", dir);
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

  // TLS 双模式：配置了 --tls-port 且证书/私钥齐全 → 额外开 HTTPS 监听（与 HTTP 并行）。
  // 用户显式要求 HTTPS 而二进制不支持时直接退出（静默降级是安全 footgun）。
  if (cfg.tlsPort > 0) {
#ifdef CV_HAVE_OPENSSL
    if (cfg.tlsCert.empty() || cfg.tlsKey.empty()) {
      CV_LOG_ERROR("启用 HTTPS 需要同时提供 --tls-cert 与 --tls-key（或 CV_TLS_CERT/CV_TLS_KEY）");
      return 1;
    }
    std::string tlsErr;
    if (!server.listenTls(cfg.listenAddr, cfg.tlsPort, cfg.tlsCert, cfg.tlsKey, cfg.workers,
                          tlsErr)) {
      CV_LOG_ERROR("HTTPS 监听失败: " << tlsErr);
      return 1;
    }
    CV_LOG_INFO("HTTPS 监听 " << cfg.listenAddr << ":" << cfg.tlsPort << "  证书 " << cfg.tlsCert);
#else
    CV_LOG_ERROR("本二进制未编译 TLS 支持（CV_ENABLE_TLS=OFF 或缺少 OpenSSL），无法启用 HTTPS");
    return 1;
#endif
  }
  {
    const std::int64_t freeBytes = diskFreeBytes(cfg.dataDir);
    if (freeBytes >= 0) {
      CV_LOG_INFO("磁盘剩余 " << freeBytes << " 字节（" << cfg.dataDir << "）");
      if (freeBytes < 512LL * 1024 * 1024) {
        CV_LOG_WARN("磁盘剩余空间不足 512MiB，上传可能失败（GET /api/v1/storage 可查）");
      }
    }
  }
  CV_LOG_INFO("接口就绪：GET /healthz · POST /api/v1/files · 分块上传 /api/v1/uploads/* · GET /api/v1/storage");
  server.runForever();

  CV_LOG_INFO("服务端已停止");
  return 0;
}

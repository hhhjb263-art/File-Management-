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
//   POST /api/v1/shares             创建分享链接（body: file_id, code?, expire_days?, max_downloads?）
//   GET  /api/v1/shares             列出分享（最新在前）
//   DELETE /api/v1/shares/:id       撤销分享
//   GET  /s/:token/meta            公开：分享元数据（免鉴权，?code= 提取码）
//   GET  /s/:token                 公开：下载分享文件（免鉴权，支持 Range，?code= 提取码）
//
// 鉴权：--auth-token / CV_AUTH_TOKEN / auth_token 非空时启用 Bearer Token 鉴权，
//   除 /healthz 与公开分享端点 /s/* 外全接口强制 Authorization: Bearer；缺失/不匹配 → 401（恒定时间比较）。
//   公开分享端点 /s/ 面向匿名访问，故免鉴权（提取码由分享侧另行校验）。
// 明文 HTTP：--http=on|off（默认 on）；off 时仅保留 HTTPS（须同时配置 TLS，否则启动报错）。

#include <algorithm>
#include <cctype>
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
#include "meta/share_repository.h"
#include "meta/schema_users.h"
#include "meta/user_repository.h"
#include "app/auth_routes.h"
#include "meta/upload_repository.h"
#include "app/share_page.h"
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

// 把分享记录转为 JSON 对象（管理接口 201 / GET 列表共用，字段契约冻结）。
// 注意：明文提取码 code 由调用方（仅创建路由）按需追加，不在此函数内——
// 列表接口拿不到明文，加进来会暴露空 code 键、泄露设计意图，且契约冻结不允许列表出现该字段。
cv::json::Value shareToJson(const cv::Share& s, const std::string& name,
                            std::int64_t size) {
  cv::json::Value v = cv::json::Value::object();
  v.set("id", static_cast<long long>(s.id));
  v.set("token", s.token);
  v.set("file_id", static_cast<long long>(s.fileId));
  v.set("name", name);
  v.set("size", static_cast<long long>(size));
  v.set("need_code", !s.codeHash.empty());
  v.set("expires_at", static_cast<long long>(s.expiresAt));
  v.set("max_downloads", static_cast<long long>(s.maxDownloads));
  v.set("downloads", static_cast<long long>(s.downloads));
  v.set("created_at", static_cast<long long>(s.createdAt));
  v.set("path", "/s/" + s.token);
  return v;
}

// 分享访问前置校验：顺序 404(调用方已查 token 存在) → 403(提取码) → 410(过期/用尽)。
// 返回 0 表示通过；否则返回 HTTP 状态码并在 reason 填入中文说明。
// 提取码以 sha256(token+":"+code) 与存储值做恒定时间比较，防时序侧信道。
int shareAccessStatus(const cv::Share& s, const std::string& providedCode,
                     std::string& reason) {
  if (!s.codeHash.empty()) {
    if (providedCode.empty()) {
      reason = "需要提取码";
      return 403;
    }
    std::string got = cv::Sha256::of(s.token + ":" + providedCode);
    if (!cv::net::constantTimeEqual(got, s.codeHash)) {
      reason = "提取码错误";
      return 403;
    }
  }
  std::int64_t now = cv::nowMillis();
  if (s.expiresAt != 0 && now >= s.expiresAt) {
    reason = "分享链接已过期";
    return 410;
  }
  if (s.maxDownloads > 0 && s.downloads >= s.maxDownloads) {
    reason = "下载次数已用尽";
    return 410;
  }
  return 0;
}

// 浏览器内容协商：请求头 Accept 含 "text/html" 则返回 true。
// headers 的 key 已转小写（见 Request::headers 注释），但 value 仍可能大小写混合，
// 故对 value 统一转小写再比对。客户端显式发 "Accept: application/json"（不含 text/html），
// 不会命中落地页，对既有 JSON/附件行为零影响。
// ⚠️ 本函数位于**全局匿名命名空间**（本文件 :64 打开），不在 `cv` 内、也早于 `:721` 的
//    `using namespace cv;`，故 `net` 必须写成 **`cv::net`**（照同段 cv::net::constantTimeEqual 的写法）。
bool acceptHtml(const cv::net::Request& req) {
  std::string a = req.header("accept");
  std::string low;
  low.reserve(a.size());
  for (char c : a) low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return low.find("text/html") != std::string::npos;
}

// 把分享访问状态码映射为落地页状态字符串（见 share_page.h::renderSharePage）。
const char* pageStateFor(int st) {
  if (st == 403) return "code";
  if (st == 410) return "gone";
  if (st == 404) return "missing";
  return "ok";
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

// ---- 文件归属（按用户隔离）----
// 归属语义（与 auth 工位冻结约定一致）：
//   authUserId == -1  → 账号体系未启用 / 静态 --auth-token（legacy/admin），不做归属过滤，
//                       完全保持历史行为；repo 查询用 owner 桶 0。
//   authUserId >= 1   → 已登录用户，只能访问 owner_id == authUserId 的资源。
//   file.ownerId == 0 → 账号体系启用前的历史数据；仅 authUserId == -1（legacy/admin）可见，
//                       authUserId >= 1 访问它一律视为"不存在"（fail-closed → 404）。
//
// repo 层查询统一用 scopeOwner()：legacy 传 0（命中历史桶=全部既有数据），
// 登录用户传自身 id。
std::int64_t scopeOwner(std::int64_t authUserId) {
  return authUserId == -1 ? 0 : authUserId;
}

// 越权判定：登录用户访问非自身资源（含历史数据桶 owner_id==0）→ false（调用方应回 404）。
// legacy（authUserId==-1）放行。此函数刻意只返回"可见/不可见"布尔，绝不区分
// "不存在"与"不属于你"，以防通过 403 泄漏"该 id 存在"。
bool ownerVisible(const cv::FileRow& row, std::int64_t authUserId) {
  if (authUserId == -1) return true;
  return row.ownerId == authUserId;
}
bool ownerVisible(const cv::UploadSession& s, std::int64_t authUserId) {
  if (authUserId == -1) return true;
  return s.ownerId == authUserId;
}

// 镜像树路径按 owner 作用域隔离：
//   ownerId <= 0  → 维持历史布局 <filesRoot>/<dir>/<name>（兼容既有部署的镜像目录不"消失"）。
//   ownerId >= 1  → <filesRoot>/u<ownerId>/<dir>/<name>，各用户互不覆盖。
std::string mirrorPathFor(const cv::Config& cfg, std::int64_t ownerId,
                          const std::string& dir, const std::string& name) {
  if (ownerId <= 0) return diskFilePath(cfg, dir, name);
  return filesRoot(cfg) + "/u" + std::to_string(ownerId) + "/" +
         (dir.empty() ? std::string() : dir + "/") + name;
}

// 物理目录树根：与镜像路径同源，按 owner 隔离（legacy 用原始根）。
std::string userFilesRoot(const cv::Config& cfg, std::int64_t ownerId) {
  if (ownerId <= 0) return filesRoot(cfg);
  return filesRoot(cfg) + "/u" + std::to_string(ownerId);
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
    // 静态加密开启时镜像落盘已被自动关闭，故强制走「分块拼装 + 解密」（日志显示 [分块拼装]）。
    std::string slice;
    bool servedFromMirror = false;
    {
      const std::string mirrorPath = mirrorPathFor(cfg, row.ownerId, row.dir, row.name);
      std::error_code mec;
      if (!store.encryptionEnabled() &&
          fs::file_size(mirrorPath, mec) == static_cast<std::uintmax_t>(total) && !mec) {
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
                   std::int64_t& totalFiles, std::int64_t ownerId, std::string& err) {
  std::vector<std::string> dirs;
  if (!repo.listDirsAll(ownerId, dirs, err)) return;
  std::vector<cv::FileRow> files;
  if (!repo.listFiles(ownerId, files, err)) return;

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

  // 静态数据加密：--data-key / CV_DATA_KEY / data_key 非空即启用 blob 落盘加密。
  // 密钥文件格式：64 hex（32 字节）或 32 字节原文；缺失/长度不对 → 启动报错退出。
  // 已配置密钥但二进制未编译 OpenSSL → 启动报错退出（无法加密）。
  bool encEnabled = false;
  if (!cfg.dataKey.empty()) {
#ifdef CV_HAVE_OPENSSL
    std::vector<unsigned char> dataKey;
    if (!loadDataKeyFile(cfg.dataKey, dataKey, err)) {
      CV_LOG_ERROR("加载静态加密密钥失败: " << err);
      return 1;
    }
    if (!store.setDataKey(dataKey, err)) {
      CV_LOG_ERROR("启用静态加密失败: " << err);
      return 1;
    }
    encEnabled = true;
    // 取舍：加密后文件树镜像若为明文副本会让加密形同虚设，故自动关闭镜像落盘；
    // Range 下载改走「分块拼装 + 解密」（日志显示 [分块拼装]），镜像直读快路径不再可用。
    CV_LOG_WARN("静态数据加密已启用：文件树镜像落盘已自动关闭，Range 下载将走分块拼装 + 解密路径"
                "（安全性提升，单次下载吞吐略降）。");
#else
    CV_LOG_ERROR("已配置 --data-key 但本二进制未编译 OpenSSL（CV_ENABLE_TLS=OFF），无法启用静态加密；"
                 "请使用编译期启用 TLS 的二进制。");
    return 1;
#endif
  }
  const bool mirrorEnabled = !encEnabled;

  Db db;
  if (!db.open(cfg.dbPath(), err)) {
    CV_LOG_ERROR("打开元数据库失败: " << err);
    return 1;
  }
  if (!db.initSchema(err)) {
    CV_LOG_ERROR("建表失败: " << err);
    return 1;
  }

  // ---- 账号数据库：**独立文件** users.db（与文件元数据分离，备份/权限策略互不影响）----
  Db usersDb;
  const std::string usersDbPath = cfg.dataDir + "/meta/users.db";
  if (!usersDb.open(usersDbPath, err)) {
    CV_LOG_ERROR("打开账号数据库失败: " << err);
    return 1;
  }
  if (!usersDb.exec(kUsersSchemaSql, err)) {
    CV_LOG_ERROR("账号库建表失败: " << err);
    return 1;
  }
  UserRepository usersRepo(usersDb);

  FileRepository repo(db);
  UploadRepository up(db);
  ShareRepository shr(db);
  net::HttpServer server;

  // 启用 API Bearer Token 鉴权（空 = 不启用，向后兼容）
  server.setAuthToken(cfg.authToken);
  // ---- 账号体系装配 ----
  // 兼容开关（关键）：**users 表为空 ⇒ 不启用强制鉴权**，保持既有单用户行为。
  // 这样 testdata/smoke*.sh（不带 token）与既有部署不受影响；
  // 一旦有人注册了账号，除 /healthz 与公开分享端点 /s/* 外都必须带有效会话令牌。
  {
    std::int64_t userCount = 0;
    std::string    uerr;
    if (!usersRepo.countUsers(userCount, uerr))
      CV_LOG_WARN("统计用户数失败（按未启用账号体系处理）: " << uerr);
    server.setAccountsEnabled(userCount > 0);
    if (userCount > 0) {
      CV_LOG_INFO("账号体系已启用：共 " << userCount
                                      << " 个用户；除 /healthz 与 /s/* 外均需有效会话令牌，"
                                      << "且文件按 owner 隔离（越权一律 404）");
    } else {
      CV_LOG_INFO("未检测到用户账号：保持单用户模式（不做按用户隔离，静态 token 仍有效）");
    }
    std::string perr2;
    if (!usersRepo.purgeDeadSessions(nowMillis(), perr2))
      CV_LOG_WARN("清理过期会话失败: " << perr2);
  }
  // 会话校验回调：把 Bearer 令牌哈希后查库（库内只存 sha256(token)，不存明文）。
  server.setSessionValidator([&usersRepo](const std::string& token, std::int64_t& outUserId) {
    std::string verr;
    outUserId = -1;
    return usersRepo.validateSession(Sha256::of(token), nowMillis(), outUserId, verr);
  });
  registerAuthRoutes(server, usersRepo, AuthConfig{});

  if (cfg.authToken.empty()) {
    // 仅在“暴露面”较大时（HTTPS 且监听非本地）升级为更醒目的风险告警；
    // 纯明文 HTTP 或仅本地监听也告警，但措辞区分。
    if (cfg.tlsPort > 0 && cfg.listenAddr != "127.0.0.1") {
      CV_LOG_WARN("未启用鉴权，任何能访问该端口(HTTPS) 的人都可读写全部文件");
    } else {
      CV_LOG_WARN("认证未启用：任何人可读写全部文件（生产环境请用 --auth-token）");
    }
  } else {
    // 不打印 token 明文
    CV_LOG_INFO("鉴权已启用：除 /healthz 外全部接口强制 Authorization: Bearer <token>"
                "（同时兼容 X-CV-Token 头）");
  }

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
    // 启用鉴权时不再泄露服务器路径（data_dir/files_root），仅暴露运维状态
    if (cfg.authToken.empty()) {
      v.set("data_dir", cfg.dataDir);
      v.set("files_root", filesRoot(cfg));
    }
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
    const std::int64_t auth = req.authUserId;        // 冻结字段：见 auth 工位约定
    const std::int64_t oid = scopeOwner(auth);        // legacy → 0；登录 → 自身 id
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
    const bool nameTaken = repo.findByPath(oid, dir, name, sameName, err);
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
    } else if (!repo.insertFile(name, dir, oid, static_cast<std::int64_t>(data.size()),
                                contentHash, chunkHashes, chunkSizes, id, err)) {
      resp.setError(500, std::string("db failed: ") + err);
      return;
    }

    // 物理镜像树（尽力而为；内容仍以 blob + DB 为权威，失败仅告警）
    // 静态加密开启时镜像已自动关闭（避免明文副本），此路径跳过。
    if (mirrorEnabled) {
      std::string mirrorErr;
      if (!store.materializeFromChunks(chunkHashes, mirrorPathFor(cfg, oid, dir, name),
                                      mirrorErr)) {
        CV_LOG_WARN("镜像文件树失败 id=" << id << ": " << mirrorErr);
      }
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
    const std::int64_t auth = req.authUserId;
    const std::int64_t oid = scopeOwner(auth);
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
    if (repo.findByPath(oid, dir, name, same, perr)) {
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
    if (!repo.insertFile(name, dir, oid, 0, emptyHash, {}, {}, id, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    if (mirrorEnabled) {
      std::string mirrorErr;
      if (!store.materializeFromChunks({}, mirrorPathFor(cfg, oid, dir, name), mirrorErr)) {
        CV_LOG_WARN("镜像文件树失败 id=" << id << ": " << mirrorErr);
      }
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
                 const std::int64_t auth = req.authUserId;
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
                 // 归属校验：越权一律 404（fail-closed；不暴露"该 id 存在"）
                 if (!ownerVisible(row, auth)) {
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
                 if (!repo.nameExists(scopeOwner(auth), row.dir, newName, id, exists, perr)) {
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
                 fs::rename(mirrorPathFor(cfg, row.ownerId, row.dir, row.name),
                            mirrorPathFor(cfg, row.ownerId, row.dir, newName), rec);
                 if (rec && mirrorEnabled) {
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
                 const std::int64_t auth = req.authUserId;
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
                 if (!ownerVisible(row, auth)) {
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
                 const auto mirrorSize = fs::file_size(mirrorPathFor(cfg, row.ownerId, row.dir, row.name), rec);
                 fs::remove(mirrorPathFor(cfg, row.ownerId, row.dir, row.name), rec);
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
  server.route("GET", "/api/v1/storage",
               [&](const net::Request& req, net::Response& resp) {
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
    // 归属统计：登录用户只回传"自己已用字节"，避免泄漏他人占用。
    if (req.authUserId != -1) {
      std::string perr;
      std::int64_t used = 0;
      if (repo.sumSizeOfOwner(req.authUserId, used, perr)) {
        v.set("used_bytes", static_cast<long long>(used));
      }
    }
    resp.setJson(200, json::dump(v));
  });

  // ---- GET /api/v1/files ----
  server.route("GET", "/api/v1/files",
               [&](const net::Request& req, net::Response& resp) {
    std::string err;
    std::vector<FileRow> rows;
    // 列表按 owner 过滤（SQL 层过滤，避免 C++ 侧泄漏他人元数据）
    if (!repo.listFiles(scopeOwner(req.authUserId), rows, err)) {
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
    const std::int64_t auth = req.authUserId;
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
    if (!ownerVisible(row, auth)) {
      resp.setError(404, "file not found");
      return;
    }
    resp.setJson(200, json::dump(fileToJson(row)));
  });

  // ---- GET /api/v1/files/:id/content （支持 Range/206，按需读取分块）----
  server.route("GET", "/api/v1/files/:id/content",
               [&](const net::Request& req, net::Response& resp) {
                 std::string err;
                 const std::int64_t auth = req.authUserId;
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
                 if (!ownerVisible(row, auth)) {
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
                 const std::int64_t auth = req.authUserId;
                 const std::int64_t oid = scopeOwner(auth);
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
                 // owner 作用域：越权/不存在统一 404，不泄漏他人文件名存在性
                 if (!repo.findByPath(oid, dir, name, row, err)) {
                   resp.setError(404,
                                 "file not found under allowed root (path rejected or "
                                 "not recorded)");
                   return;
                 }
                 // 防御纵深：镜像树中该文件若存在，校验其真实路径未逃出允许根
                 std::string disk = mirrorPathFor(cfg, row.ownerId, row.dir, row.name);
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
                 const std::int64_t auth = req.authUserId;
                 const std::int64_t oid = scopeOwner(auth);
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
                 // 物理侧：逐级创建，任何一级符号链接/逃逸都拒绝。
                 // 目录树按 owner 作用域隔离，避免 B 建 /docs 撞 A 的目录（否则冲突错误会泄漏 A 的目录名存在）。
                 std::string ferr;
                 if (!ensureRealDirUnder(userFilesRoot(cfg, oid), dir, ferr)) {
                   resp.setError(403, ferr);
                   return;
                 }
                 // 元数据侧：按 owner 幂等登记（复合主键 owner_id,path）
                 bool created = false;
                 if (!repo.createDir(oid, dir, created, perr)) {
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
  server.route("GET", "/api/v1/dirs",
               [&](const net::Request& req, net::Response& resp) {
    std::string err;
    std::vector<std::string> dirs;
    // 仅列出调用者名下的目录（按 owner 隔离）
    if (!repo.listDirs(scopeOwner(req.authUserId), dirs, err)) {
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
  // 受全局鉴权保护（白名单仅豁免 /healthz，本接口需有效 Bearer 令牌）；
  // 不分页；从 dir_node + file_node/file_dir 在内存按路径段建树，
  // 目录在前文件在后、同类 name 升序；空目录以 children:[] 出现；路径为规范化相对路径（'/' 分隔）。
  // 仅构建调用者名下的目录/文件（按 owner 隔离）。
  server.route("GET", "/api/v1/tree",
               [&](const net::Request& req, net::Response& resp) {
    std::string err;
    json::Value rootArr;
    std::int64_t totalDirs = 0, totalFiles = 0;
    buildFileTree(repo, rootArr, totalDirs, totalFiles, scopeOwner(req.authUserId), err);
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
                 const std::int64_t auth = req.authUserId;
                 const std::int64_t oid = scopeOwner(auth);
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
                 // 同名检测：目标目录下已存在同名文件 → 记录 sameName；未声明覆盖则 409。
                 // （nameTaken/sameName 提升到路由作用域，供下方秒传"覆盖"分支复用。）
                 FileRow sameName;
                 bool nameTaken = repo.findByPath(oid, dir, name, sameName, perr);
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

                // 秒传：file_hash 已在内容库命中 → 复用既有内容，但必须为"调用者"新建一行
                // file_node（owner=调用者、落在目标 dir），把 ref_count 正确 +1，
                // 返回**新建的 id**（绝不直接把命中那行——可能是别人的——id 回给调用者，
                // 那既是越权入口、也导致"秒传成功"却不在调用者目录树里）。
                // 与同名检测交互：目标目录下已有同名文件且声明 overwrite → 走既有"覆盖"语义。
                if (!fileHash.empty()) {
                  FileRow existing;
                  if (repo.findByContentHash(fileHash, existing, perr)) {
                    std::vector<std::string> ch;
                    std::vector<std::size_t> cs;
                    if (!repo.chunkHashesOf(existing.id, ch, perr) ||
                        !repo.chunkSizesOf(ch, cs, perr)) {
                      resp.setError(500, std::string("db failed: ") + perr);
                      return;
                    }
                    std::int64_t newFileId = 0;
                    if (nameTaken && overwrite) {
                      // 覆盖目标目录同名文件：保留其 id/名，替换内容（引用计数同步增减）
                      std::vector<std::string> orphans;
                      if (!repo.replaceContent(sameName.id, existing.size, fileHash, ch, cs,
                                               orphans, perr)) {
                        resp.setError(500, std::string("db failed: ") + perr);
                        return;
                      }
                      newFileId = sameName.id;
                      const std::int64_t freed = dropOrphanBlobs(store, orphans);
                      if (freed > 0) {
                        CV_LOG_INFO("覆盖释放旧内容 blob " << orphans.size() << " 个，" << freed
                                                          << " 字节");
                      }
                    } else {
                      if (!repo.insertFile(name, dir, oid, existing.size, fileHash, ch, cs,
                                          newFileId, perr)) {
                        resp.setError(500, std::string("db failed: ") + perr);
                        return;
                      }
                    }
                    // 物理镜像树（尽力而为；ref_content 以 blob + DB 为权威）
                    if (mirrorEnabled) {
                      std::string mirrorErr;
                      if (freeSpaceBelow(cfg, static_cast<std::int64_t>(existing.size))) {
                        CV_LOG_WARN("跳过镜像（空间不足）file_id=" << newFileId);
                      } else if (!store.materializeFromChunks(
                                     ch, mirrorPathFor(cfg, oid, dir, name), mirrorErr)) {
                        CV_LOG_WARN("镜像文件树失败 file_id=" << newFileId << ": " << mirrorErr);
                      }
                    }
                    json::Value v = json::Value::object();
                    v.set("upload_id", static_cast<long long>(0));
                    v.set("done", true);
                    v.set("file_id", static_cast<long long>(newFileId));
                    v.set("name", name);
                    v.set("size", static_cast<long long>(size));
                    v.set("chunk_size", static_cast<long long>(chunkSize));
                    v.set("hash", fileHash);
                    v.set("uploaded", json::Value::array());
                    v.set("received_bytes", static_cast<long long>(size));
                    v.set("instant", true);
                    resp.setJson(200, json::dump(v));
                    CV_LOG_INFO("init 秒传命中 hash=" << fileHash
                                                      << " → 新建 file_id=" << newFileId
                                                      << " owner=" << oid);
                    return;
                  }
                  // 断点复用：同 (file_hash,size,chunk_size,owner) 的未完成会话
                  UploadSession s;
                  if (up.findResumable(fileHash, size, chunkSize, oid, s, perr)) {
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
                if (!up.create(oid, name, size, chunkSize, fileHash, newId, perr)) {
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
                 const std::int64_t auth = req.authUserId;
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
                 if (!ownerVisible(s, auth)) {
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
                 const std::int64_t auth = req.authUserId;
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
                 if (!ownerVisible(s, auth)) {
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
                 const std::int64_t auth = req.authUserId;
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
                 if (!ownerVisible(s, auth)) {
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
                 // 覆盖：同目录同名已存在（且归属本会话 owner）且会话声明了 overwrite → 替换原记录内容
                 FileRow sameName;
                 bool replace = overwrite && repo.findByPath(s.ownerId, dir, s.name, sameName, perr);
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
                 } else if (!repo.insertFile(s.name, dir, s.ownerId, s.size, computed,
                                            chunkHashes, chunkSizes, fileId, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 // 物理镜像树（尽力而为；空间不足则跳过，失败仅告警，内容仍以 blob + DB 为权威）
                 // 静态加密开启时镜像已自动关闭（避免明文副本），此路径跳过。
                 if (mirrorEnabled) {
                   std::string mirrorErr;
                   if (freeSpaceBelow(cfg, static_cast<std::int64_t>(s.size))) {
                     CV_LOG_WARN("跳过镜像（空间不足）file_id=" << fileId);
                   } else if (!store.materializeFromChunks(
                                  chunkHashes,
                                  mirrorPathFor(cfg, s.ownerId, dir, s.name), mirrorErr)) {
                     CV_LOG_WARN("镜像文件树失败 file_id=" << fileId << ": " << mirrorErr);
                   }
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

  // ================= 分享链接（管理接口需鉴权；公开端点 /s/* 免鉴权） =================

  // ---- POST /api/v1/shares （需鉴权）----
  // body: {"file_id":<id>,"code":<可空>,"expire_days":<int,0=永久>,"max_downloads":<int,0=不限>}
  server.route("POST", "/api/v1/shares", [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    cv::json::Value body = parseJsonBody(req.body, perr);
    if (!perr.empty()) {
      resp.setError(400, "invalid json: " + perr);
      return;
    }
    // file_id：兼容数字与字符串两种形态
    const auto* fidV = body.find("file_id");
    std::int64_t fileId = 0;
    bool haveFileId = false;
    if (fidV) {
      if (fidV->type() == cv::json::Value::Type::Number) {
        fileId = static_cast<std::int64_t>(fidV->numberValue());
        haveFileId = true;
      } else if (fidV->type() == cv::json::Value::Type::String) {
        haveFileId = parseId(fidV->stringValue(), fileId);
      }
    }
    if (!haveFileId || fileId <= 0) {
      resp.setError(400, "missing or invalid file_id");
      return;
    }
    cv::FileRow frow;
    if (!repo.findById(fileId, frow, perr)) {
      resp.setError(404, "file not found");  // 文件不存在
      return;
    }
    std::string code = body.find("code") ? body.find("code")->stringValue() : "";
    std::int64_t expireDays = 0, maxDownloads = 0;
    if (const auto* ed = body.find("expire_days")) expireDays = static_cast<std::int64_t>(ed->numberValue());
    if (const auto* md = body.find("max_downloads")) maxDownloads = static_cast<std::int64_t>(md->numberValue());
    if (expireDays < 0 || maxDownloads < 0) {
      resp.setError(400, "expire_days / max_downloads must be >= 0");
      return;
    }
    // token：系统随机源（randomHex(16) → 32 位十六进制），绝不用 rand()/时间戳
    std::string token = cv::randomHex(16);
    // 提取码不存明文：code_hash = sha256(token + ":" + code)；空 code ⇒ 空串（need_code=false）
    std::string codeHash = cv::shareCodeHash(token, code);

    cv::Share s;
    if (!shr.create(fileId, codeHash, expireDays, maxDownloads, token, s, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    cv::json::Value v = shareToJson(s, frow.name, frow.size);
    // 明文提取码**仅在此 201 响应回显一次**（库中只存 sha256(token+":"+code)，列表接口无法也不应回显）
    v.set("code", code);
    resp.setJson(201, cv::json::dump(v));
    CV_LOG_INFO("创建分享 id=" << s.id << " file_id=" << fileId << " token=" << token
                               << (code.empty() ? " [无提取码]" : " [有提取码]"));
  });

  // ---- GET /api/v1/shares （需鉴权，最新在前）----
  server.route("GET", "/api/v1/shares", [&](const net::Request&, net::Response& resp) {
    std::string perr;
    std::vector<cv::Share> items;
    if (!shr.listAll(items, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    cv::json::Value arr = cv::json::Value::array();
    for (const auto& s : items) {
      cv::FileRow frow;
      std::string ferr;
      std::string name;
      std::int64_t size = 0;
      if (repo.findById(s.fileId, frow, ferr)) {
        name = frow.name;
        size = frow.size;
      }
      arr.push_back(shareToJson(s, name, size));
    }
    cv::json::Value v = cv::json::Value::object();
    v.set("items", arr);
    resp.setJson(200, cv::json::dump(v));
  });

  // ---- DELETE /api/v1/shares/:id （需鉴权）----
  server.route("DELETE", "/api/v1/shares/:id", [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    std::int64_t id = 0;
    if (!parseId(req.param("id"), id)) {
      resp.setError(404, "share not found");
      return;
    }
    cv::Share s;
    bool found = false;
    if (!shr.findById(id, s, found, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    if (!found) {
      resp.setError(404, "share not found");  // 不存在/已撤销
      return;
    }
    if (!shr.removeById(id, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    cv::json::Value v = cv::json::Value::object();
    v.set("ok", true);
    resp.setJson(200, cv::json::dump(v));
    CV_LOG_INFO("撤销分享 id=" << id);
  });

  // ---- GET /s/:token/meta （公开，免鉴权）----
  // 顺序：404(token) → 403(提取码) → 410(过期/用尽) → 200
  // 内容协商：Accept 含 text/html（浏览器）→ 返回 HTML 落地页；否则保持既有 JSON 不变
  //（客户端显式发 Accept: application/json，不含 text/html，不会命中落地页，零影响）。
  server.route("GET", "/s/:token/meta", [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    cv::Share s;
    bool found = false;
    if (!shr.findByToken(req.param("token"), s, found, perr)) {
      if (acceptHtml(req)) {
        resp.setBinary(500, cv::share_page::renderSharePage(req.param("token"), "missing", "",
                            0, 0, 0, 0, "", "服务端内部错误"), "text/html; charset=utf-8");
        return;
      }
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    if (!found) {
      if (acceptHtml(req)) {
        resp.setBinary(404, cv::share_page::renderSharePage(req.param("token"), "missing", "",
                            0, 0, 0, 0, "", "分享链接不存在或已撤销"), "text/html; charset=utf-8");
        return;
      }
      resp.setError(404, "分享链接不存在或已撤销");
      return;
    }
    // 复用既有 queryParam()（本文件统一入口），并补 URL 解码：提取码若含需转义字符也能正确比对。
    std::string providedCode = cv::util::urlDecode(queryParam(req.query, "code"));
    std::string reason;
    int st = shareAccessStatus(s, providedCode, reason);
    if (st != 0) {
      if (acceptHtml(req)) {
        resp.setBinary(st, cv::share_page::renderSharePage(s.token, pageStateFor(st), "", 0,
                            s.expiresAt, s.maxDownloads, s.downloads, "",
                            reason), "text/html; charset=utf-8");
        return;
      }
      resp.setError(st, reason);  // 403 提取码错误/缺失；410 已过期/次数用尽
      return;
    }
    cv::FileRow frow;
    if (!repo.findById(s.fileId, frow, perr)) {
      if (acceptHtml(req)) {
        resp.setBinary(404, cv::share_page::renderSharePage(s.token, "missing", "", 0, 0, 0, 0,
                            "", "文件不存在"), "text/html; charset=utf-8");
        return;
      }
      resp.setError(404, "文件不存在");
      return;
    }
    if (acceptHtml(req)) {
      // 浏览器：返回落地页（含文件名/大小/有效期/剩余次数 + 下载按钮）；不消耗下载次数。
      resp.setBinary(200, cv::share_page::renderSharePage(s.token, "ok", frow.name, frow.size,
                          s.expiresAt, s.maxDownloads, s.downloads, providedCode, ""),
                     "text/html; charset=utf-8");
      return;
    }
    cv::json::Value v = cv::json::Value::object();
    v.set("name", frow.name);
    v.set("size", static_cast<long long>(frow.size));
    v.set("need_code", !s.codeHash.empty());
    v.set("expires_at", static_cast<long long>(s.expiresAt));
    v.set("max_downloads", static_cast<long long>(s.maxDownloads));
    v.set("downloads", static_cast<long long>(s.downloads));
    v.set("expired", (s.expiresAt != 0 && cv::nowMillis() >= s.expiresAt));
    v.set("exhausted", (s.maxDownloads > 0 && s.downloads >= s.maxDownloads));
    resp.setJson(200, cv::json::dump(v));
  });

  // ---- GET /s/:token （公开，免鉴权，支持 Range，复用 serveFileContent）----
  // 顺序：404(token) → 403(提取码) → 410(过期/用尽) → 落地页/计数+1 → 回内容
  // 内容协商：Accept 含 text/html（浏览器）→ 返回 HTML 落地页（?dl=1 显式下载除外）；
  //          否则保持既有附件下载行为不变（客户端发 Accept: application/json，零影响）。
  server.route("GET", "/s/:token", [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    cv::Share s;
    bool found = false;
    // 410（已过期 / 次数用尽）统一出口：HTML 落地页分支与 JSON 分支行为一致，
    // 供“预检 410”与“并发用尽 410”两处复用，避免复制两份 410 代码。
    auto sendGone = [&](const std::string& reason) {
      if (acceptHtml(req)) {
        resp.setBinary(410, cv::share_page::renderSharePage(s.token, "gone", "", 0,
                            s.expiresAt, s.maxDownloads, s.downloads, "", reason),
                       "text/html; charset=utf-8");
        return;
      }
      resp.setError(410, reason);
    };
    if (!shr.findByToken(req.param("token"), s, found, perr)) {
      if (acceptHtml(req)) {
        resp.setBinary(500, cv::share_page::renderSharePage(req.param("token"), "missing", "",
                            0, 0, 0, 0, "", "服务端内部错误"), "text/html; charset=utf-8");
        return;
      }
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    if (!found) {
      if (acceptHtml(req)) {
        resp.setBinary(404, cv::share_page::renderSharePage(req.param("token"), "missing", "",
                            0, 0, 0, 0, "", "分享链接不存在或已撤销"), "text/html; charset=utf-8");
        return;
      }
      resp.setError(404, "分享链接不存在或已撤销");
      return;
    }
    // 复用既有 queryParam()（本文件统一入口），并补 URL 解码：提取码若含需转义字符也能正确比对。
    std::string providedCode = cv::util::urlDecode(queryParam(req.query, "code"));
    std::string reason;
    int st = shareAccessStatus(s, providedCode, reason);
    if (st != 0) {
      if (st == 410) { sendGone(reason); return; }  // 已过期 / 预检用尽，统一出口
      // 403 提取码错误/缺失：HTML 表单态（reason 由 renderSharePage 内部转义，勿外层再转）
      if (acceptHtml(req)) {
        resp.setBinary(st, cv::share_page::renderSharePage(s.token, pageStateFor(st), "", 0,
                            s.expiresAt, s.maxDownloads, s.downloads, "", reason),
                       "text/html; charset=utf-8");
        return;
      }
      resp.setError(st, reason);
      return;
    }
    cv::FileRow frow;
    if (!repo.findById(s.fileId, frow, perr)) {
      if (acceptHtml(req)) {
        resp.setBinary(404, cv::share_page::renderSharePage(s.token, "missing", "", 0, 0, 0, 0,
                            "", "文件不存在"), "text/html; charset=utf-8");
        return;
      }
      resp.setError(404, "文件不存在");
      return;
    }
    // 浏览器落地页：?dl=1 为显式下载提示，跳过 HTML 直接回附件（见 share_page.h 说明）。
    // 落地页本身不消耗下载次数；只有真正的附件下载（客户端 Accept: application/json 或 ?dl=1）才 downloads++。
    bool wantRawDownload = (queryParam(req.query, "dl") == "1");
    if (acceptHtml(req) && !wantRawDownload) {
      resp.setBinary(200, cv::share_page::renderSharePage(s.token, "ok", frow.name, frow.size,
                          s.expiresAt, s.maxDownloads, s.downloads, providedCode, ""),
                     "text/html; charset=utf-8");
      return;
    }
    // 以下为原有附件下载逻辑：开始回内容之前下载计数 +1。
    // 条件自增（SQL 端原子判定），0 行受影响 = 已达上限 → 回 410（与预检 410 一致），绝不当成 DB 错误。
    bool exhausted = false;
    if (!shr.incrDownloads(s.id, perr, &exhausted)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    if (exhausted) {
      sendGone("下载次数已用尽");
      return;
    }
    // Content-Disposition：原文件名（UTF-8 百分号编码），触发浏览器下载
    resp.extraHeaders["content-disposition"] =
        "attachment; filename*=UTF-8''" + util::urlEncode(frow.name);
    serveFileContent(repo, store, cfg, frow, req, resp);
  });

  // 明文 HTTP 监听（可经 --http=off 关闭，仅保留 HTTPS）
  if (cfg.httpEnabled) {
    if (!server.listen(cfg.listenAddr, cfg.port, cfg.workers, err)) {
      CV_LOG_ERROR("HTTP 监听失败: " << err);
      return 1;
    }
    CV_LOG_INFO("监听(明文) " << cfg.listenAddr << ":" << cfg.port << "  数据目录 " << cfg.dataDir
                             << "  工作线程 " << cfg.workers);
  } else {
    CV_LOG_INFO("明文 HTTP 已按 --http=off 关闭（HTTPS 是否成功启动见下方监听日志）");
  }

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
  } else if (!cfg.httpEnabled) {
    // --http=off 但未配置任何 TLS → 将出现“零监听”静默状态，必须显式报错退出
    CV_LOG_ERROR("配置冲突：--http=off（明文 HTTP 已关闭）但未配置 --tls-port，"
                 "服务端将无任何监听端口。请配置 TLS(--tls-port/--tls-cert/--tls-key)或保持 HTTP 开启。");
    return 1;
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

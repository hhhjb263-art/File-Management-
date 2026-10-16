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
//   GET  /api/v1/shares             列出分享（最新在前，每条含 state 字段）
//   POST /api/v1/shares/cleanup     清除调用者名下无效分享（revoked/过期/用尽）→ {removed:N}
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
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
#include "meta/tag_repository.h"
#include "meta/user_repository.h"
#include "meta/version_repository.h"
#include "app/auth_routes.h"
#include "meta/upload_repository.h"
#include "app/share_page.h"
#include "net/http_server.h"
#include "store/content_store.h"
#include "store/blob_gc.h"   // 启动期孤儿 blob 回收
#include "store/scrub.h"        // 主动巡检：发现已经损坏的块
#include "store/blob_backup.h"  // 内容库副本：备份 + 从副本修复
#include "core/quota.h"         // 用户空间配额判定（纯逻辑）
#include "meta/schema_users.h"  // 已在下方用于 users.db
#include <atomic>
#include <thread>
#include "app/db_backup.h"      // 数据库一致性热备
#include "app/virus_scan.h"     // 恶意文件判定（策略+缓存+可注入远端查询）
#include <ctime>               // std::strftime（备份目录时间戳）
#include "store/durable_io.h"   // 落盘屏障（fsync 语义，见 writeAtomicFile / 秒传守卫）

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

// ---- 回收站 / 版本 / 标签的 JSON 序列化 ----
cv::json::Value versionToJson(const cv::VersionRow& v, bool current) {
  cv::json::Value o = cv::json::Value::object();
  o.set("id", static_cast<long long>(v.id));
  o.set("file_id", static_cast<long long>(v.fileId));
  o.set("version", static_cast<long long>(v.versionNo));
  o.set("size", static_cast<long long>(v.size));
  o.set("hash", v.contentHash);
  o.set("chunks", static_cast<long long>(v.chunkCount));
  o.set("device", v.device);
  o.set("note", v.note);
  o.set("created_at", static_cast<long long>(v.createdAt));
  o.set("current", current);
  return o;
}

cv::json::Value tagToJson(const cv::TagRow& t) {
  cv::json::Value o = cv::json::Value::object();
  o.set("id", static_cast<long long>(t.id));
  o.set("name", t.name);
  o.set("color", t.color);
  o.set("count", static_cast<long long>(t.fileCount));
  o.set("created_at", static_cast<long long>(t.createdAt));
  return o;
}

// 回收站位点：数字文件 id 与目录相对路径**二选一**（同时给出时 dir 优先）。
//
// 为什么走 body 而不走 URL 路径参数：http_server::dispatch 把路径段原样塞进 params，
// **不做百分号解码**，所以 `dir:照片/2026` 这类含冒号 + 中文的组合塞进路径只会得到
// 一串未解码字节，与库里的路径永远比对不上。放 JSON body 里由解析器解码，干净。
struct TrashTarget {
  bool isDir = false;
  std::int64_t fileId = 0;
  std::string dir;
};

bool parseTrashTarget(const cv::json::Value& body, TrashTarget& out, std::string& err) {
  out = TrashTarget{};
  const cv::json::Value* pdir = body.find("dir");
  if (pdir && pdir->type() == cv::json::Value::Type::String && !pdir->stringValue().empty()) {
    std::string rel;
    const cv::PathStatus st = cv::sanitizeRelPath(pdir->stringValue(), rel);
    if (st != cv::PathStatus::Ok) {
      err = std::string("invalid dir: ") + cv::pathStatusText(st);
      return false;
    }
    out.isDir = true;
    out.dir = rel;
    return true;
  }
  const cv::json::Value* pid = body.find("file_id");
  if (!pid || pid->type() != cv::json::Value::Type::Number) {
    err = "missing file_id or dir";
    return false;
  }
  out.fileId = static_cast<std::int64_t>(pid->numberValue());
  if (out.fileId <= 0) {
    err = "invalid file_id";
    return false;
  }
  return true;
}

// 展开相对路径的全部前缀链："a/b/c" → ["a", "a/b", "a/b/c"]；根目录（空串）→ 空列表。
// 用途：恢复/兜底登记时，"文件要回到 a/b/c"意味着 a、a/b、a/b/c 三层目录都得在。
std::vector<std::string> pathPrefixChain(const std::string& path) {
  std::vector<std::string> chain;
  std::string cur;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string seg =
        path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (seg.empty()) break;
    cur = cur.empty() ? seg : (cur + "/" + seg);
    chain.push_back(cur);
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return chain;
}

// ASCII 小写化（非 ASCII 字节原样保留 —— 必须走 unsigned char，否则中文字节上
// 直接调 tolower 是 UB）。检索的关键词匹配用。
std::string asciiLower(std::string s) {
  for (char& c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u >= 'A' && u <= 'Z') c = static_cast<char>(u - 'A' + 'a');
  }
  return s;
}

// 按扩展名归类，取值与客户端 core/Types.h 的 FileType 枚举顺序**严格一致**：
// 0 文件夹 1 文档 2 图片 3 视频 4 音频 5 压缩包 6 代码 7 其它
int fileTypeOf(const std::string& name) {
  const std::size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= name.size()) return 7;
  const std::string ext = asciiLower(name.substr(dot + 1));

  static const char* kDoc[] = {"pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx",
                               "txt", "md",   "rtf",  "odt", "ods",  "csv"};
  static const char* kImg[] = {"jpg", "jpeg", "png", "gif", "bmp", "webp", "svg", "ico", "heic"};
  static const char* kVideo[] = {"mp4", "mkv", "mov", "avi", "wmv", "flv", "webm", "m4v"};
  static const char* kAudio[] = {"mp3", "wav", "flac", "aac", "ogg", "m4a", "wma"};
  static const char* kArchive[] = {"zip", "rar", "7z", "tar", "gz", "bz2", "xz", "iso"};
  static const char* kCode[] = {"c",    "h",   "cpp",  "hpp",  "cc",  "py",   "js",  "ts",
                                "java", "go",  "rs",   "rb",   "php", "sh",   "bat", "ps1",
                                "html", "css", "json", "xml",  "yaml", "yml", "toml", "sql"};

  auto in = [&ext](const char* const* arr, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      if (ext == arr[i]) return true;
    }
    return false;
  };
  if (in(kDoc, sizeof(kDoc) / sizeof(kDoc[0]))) return 1;
  if (in(kImg, sizeof(kImg) / sizeof(kImg[0]))) return 2;
  if (in(kVideo, sizeof(kVideo) / sizeof(kVideo[0]))) return 3;
  if (in(kAudio, sizeof(kAudio) / sizeof(kAudio[0]))) return 4;
  if (in(kArchive, sizeof(kArchive) / sizeof(kArchive[0]))) return 5;
  if (in(kCode, sizeof(kCode) / sizeof(kCode[0]))) return 6;
  return 7;
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
  v.set("revoked", s.revoked);
  v.set("state", cv::shareStateOf(s, cv::nowMillis()));  // active|revoked|expired|exhausted
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
  // 原子 + 持久的写盘已收敛到 store/durable_io.h。
  // 原实现是 `ofstream::flush() + rename`：flush 只把数据推进**页缓存**，不是落盘屏障，
  // 所以断电后可能出现"分块文件在、内容不在（长度 0 或截断）"。
  // 现在由 durable::writeFileAtomic 负责：写临时文件 → fsync → rename → fsync 父目录。
  // 它同样会按需创建父目录（历史真机踩过"cannot open tmp for write: /files/u1/xxx.tmp"）。
  return cv::durable::writeFileAtomic(finalPath, data, err);
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

// query 里是否**显式出现**了某键。与 queryParam 的区别：空值也算"出现"。
// 为什么需要它：`?dir=` 表示"根目录"，必须与"根本没传 dir"区分开 ——
// 否则根目录要么永远列不出文件，要么变成"不过滤"把整个库拉回来。
bool queryHasKey(const std::string& query, const std::string& key) {
  std::size_t start = 0;
  while (start <= query.size()) {
    std::size_t amp = query.find('&', start);
    std::string item =
        query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
    if (!item.empty()) {
      std::size_t eq = item.find('=');
      if ((eq == std::string::npos ? item : item.substr(0, eq)) == key) return true;
    }
    if (amp == std::string::npos) break;
    start = amp + 1;
  }
  return false;
}

// 文件列表分页参数
constexpr std::int64_t kDefaultFileListLimit = 1000;  // 不传 limit 时的每页条数
constexpr std::int64_t kMaxFileListLimit = 5000;      // 防止 limit 被用来拉爆服务端内存

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

// 秒传 / 断点复用前的**磁盘完整性守卫**。
//
// 为什么必须有：内容哈希相同**不代表内容真的还在**。服务端没有任何 fsync 屏障时，
// 掉电可能留下 0 字节或截断的 blob（名字在、数据没落盘），而
//   · `ContentStore::exists()` 只看"文件在不在"，不看内容；
//   · `ContentStore::get()` 读出来直接用，也不校验哈希。
// 于是秒传一旦放行，坏内容就被**永久固化** —— 相同内容再传仍然命中，永远不会重传，
// ref_count 还在、GC 也不会碰它。
//
// 代价：逐块只做 stat + 尺寸比对（N 次系统调用，相对网络传输可忽略），**不做全量哈希**，
// 所以不会把秒传变成"每次都读一遍整文件"。尺寸不符是断电的典型表现（0 长度/截断），
// 这一步足以把绝大多数损坏挡在门外；调用方据此退回正常上传以覆盖修复。
bool chunksIntactOnDisk(const cv::ContentStore& store,
                        const std::vector<std::string>& hashes,
                        const std::vector<std::size_t>& sizes) {
  for (std::size_t i = 0; i < hashes.size(); ++i) {
    const std::int64_t expect =
        i < sizes.size() ? static_cast<std::int64_t>(sizes[i]) : 0;
    if (!store.sizeMatches(hashes[i], expect)) return false;
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

// 版本保留策略（PRD FR-4-2）落地：每次产生新版本后按「条数 + 天数双限取先到者」清理，
// 并把因此引用计数归零的 blob 真正删掉。
//
// 为什么必须接线：每次覆盖 / 回滚都会先 captureCurrent() 留一份快照，若不清理就是
// **磁盘只涨不降**，而且用户完全看不到增长来源（版本占用没有单独展示）。
// 之前 pruneVersions() 有实现却零调用，等于这条策略从未生效。
//
// 安全性：pruneVersions 内部只删「序号超出 keep」或「早于 cutoff」的版本；
// 刚创建的那一版必然是最新且未过期 ⇒ 永远不会被自己刚生成的快照清掉。
// 失败只告警、不阻断主流程 —— 版本多留几条远比让上传失败好。
void pruneVersionsAndFree(cv::VersionRepository& ver, cv::ContentStore& store,
                          const cv::Config& cfg, std::int64_t fileId) {
  const std::int64_t keep = cfg.versionKeepCount;
  const std::int64_t days = cfg.versionRetentionDays;
  if (keep <= 0 && days <= 0) return;   // 两项都不限：按配置不清理

  // created_at 是 Unix 毫秒（与 version_repository 写入口径一致）
  std::int64_t cutoff = 0;   // 0 = 不限天数
  if (days > 0) {
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    cutoff = static_cast<std::int64_t>(nowMs) - days * 86400000LL;
  }

  std::vector<std::string> orphans;
  std::string perr;
  if (!ver.pruneVersions(fileId, keep, cutoff, orphans, perr)) {
    CV_LOG_WARN("版本清理失败 file_id=" << fileId << "：" << perr);
    return;
  }
  if (orphans.empty()) return;
  const std::int64_t freed = dropOrphanBlobs(store, orphans);
  CV_LOG_INFO("版本清理 file_id=" << fileId << " 释放 blob " << orphans.size()
                                  << " 个，" << freed << " 字节");
}

// 按文件行提供内容（整文件 / Range 分块复用）。调用方已完成鉴权与存在性检查。
// 把 [rstart, rend] 这段内容以**流式**挂到响应上：每块 ≤ kMaxRangeSpan，内存占用有界。
// 镜像可用时按文件顺序读（连续、对页缓存友好）；否则按分块拼装（静态加密库必然走这条）。
//
// ⚠️ lambda 是在 serveFileContent 返回之后、由 HTTP 层边写边调的，所以：
//    · hashes 必须**按值**搬运（用 shared_ptr 持有），不能悬空引用局部变量；
//    · store 是 main() 级对象，引用安全；
//    · 中途读失败没法再改状态码（响应头已发出）——只结束，客户端会因长度不符判失败，
//      这比静默回一份内容不对的数据要好。
void attachStreamBody(cv::net::Response& resp, cv::ContentStore& store,
                      const std::vector<std::string>& hashes, const std::string& mirrorPath,
                      bool useMirror, std::int64_t rstart, std::int64_t rend,
                      std::int64_t fileId) {
  resp.streamLength = rend - rstart + 1;
  auto cursor = std::make_shared<std::int64_t>(rstart);
  auto mir    = std::make_shared<std::ifstream>();
  if (useMirror) mir->open(mirrorPath, std::ios::binary);
  const bool mirOk = mir->is_open();
  auto       hs    = std::make_shared<std::vector<std::string>>(hashes);

  resp.nextChunk = [&store, hs, cursor, mir, mirOk, rend, fileId](std::string& out) -> bool {
    if (*cursor > rend) return false;
    const std::int64_t n = std::min<std::int64_t>(kMaxRangeSpan, rend - *cursor + 1);
    if (mirOk) {
      mir->clear();   // 清掉上一轮 read 的 eof/fail 位，否则第二次 seek 之后读不出来
      mir->seekg(*cursor);
      out.assign(static_cast<std::size_t>(n), '\0');
      mir->read(out.empty() ? nullptr : &out[0], n);
      out.resize(static_cast<std::size_t>(mir->gcount()));
      *cursor += static_cast<std::int64_t>(out.size());
      return !out.empty();
    }
    std::string part, e;
    if (!store.readRange(*hs, *cursor, n, part, e)) {
      CV_LOG_WARN("流式下载读分块失败 id=" << fileId << " offset=" << *cursor << ": " << e);
      return false;
    }
    *cursor += static_cast<std::int64_t>(part.size());
    out = std::move(part);
    return !out.empty();
  };
}

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
    // ⚠️ 这里**曾经**把跨度过大的区间截断到 kMaxRangeSpan，再按截断后的区间回 206。
    //    那是给"我们自己的客户端"设计的（它会按段续拉），但**浏览器不会**：
    //    浏览器收到长度短于它请求区间的 206，就判定响应异常 → 报"网络错误，无法下载"。
    //    现在改成：小跨度照旧整份回；大跨度走**流式**（不截断，内存仍每块 ≤ kMaxRangeSpan）。
    std::int64_t len = rend - rstart + 1;

    // 快路径：文件树镜像存在且大小吻合 → 直接顺序读该文件。
    // 镜像是完整连续文件，比分块拼装（开多个 blob、seek、拷贝拼接）快得多，
    // 也对操作系统预读 / 页缓存友好。镜像可能缺失（空间不足被跳过 / 旧版本上传），
    // 此时回退到按分块拼装，行为不变。
    // 静态加密开启时镜像落盘已被自动关闭，故强制走「分块拼装 + 解密」（日志显示 [分块拼装]）。
    // ---- 大跨度：流式回（既不截断，也不把整份拼进内存）----
    if (len > kMaxSingleShotDownload) {
      const std::string mirrorPath = mirrorPathFor(cfg, row.ownerId, row.dir, row.name);
      std::error_code   mec;
      const bool useMirror =
          !store.encryptionEnabled() &&
          fs::file_size(mirrorPath, mec) == static_cast<std::uintmax_t>(total) && !mec;
      resp.extraHeaders["Accept-Ranges"] = "bytes";
      resp.extraHeaders["Content-Range"] = "bytes " + std::to_string(rstart) + "-" +
                                          std::to_string(rend) + "/" + std::to_string(total);
      resp.contentType = "application/octet-stream";
      attachStreamBody(resp, store, hashes, mirrorPath, useMirror, rstart, rend, row.id);
      CV_LOG_INFO("下载(流式 206) id=" << row.id << " range=" << rstart << "-" << rend
                                      << (useMirror ? " [镜像直读]" : " [分块拼装]"));
      return;
    }

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
    // 这里**曾经**回 409，让客户端改用 Range。问题是浏览器根本不懂这个约定：
    // 它点下载 → 拿到一坨 JSON → 报"网络错误，无法下载"。现在改为流式回整份。
    const std::string mirrorPath = mirrorPathFor(cfg, row.ownerId, row.dir, row.name);
    std::error_code   mec;
    const bool        useMirror =
        !store.encryptionEnabled() &&
        fs::file_size(mirrorPath, mec) == static_cast<std::uintmax_t>(total) && !mec;
    resp.contentType = "application/octet-stream";
    attachStreamBody(resp, store, hashes, mirrorPath, useMirror, 0, total - 1, row.id);
    CV_LOG_INFO("下载(流式 200) id=" << row.id << " size=" << total
                                    << (useMirror ? " [镜像直读]" : " [分块拼装]"));
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

// 取某个用户的空间配额（PRD FR-1-4）。
//   · 未启用账号体系（userId < 1，静态 token 模式）⇒ 不限
//   · users.quota_bytes 为 0 ⇒ 回落到 --user-quota 的默认值（0 也是不限）
std::int64_t userQuotaBytes(cv::Db& usersDb, const cv::Config& cfg, std::int64_t userId) {
  if (userId < 1) return 0;
  std::vector<std::vector<std::string>> rows;
  std::string                           err;
  // userId 是 std::int64_t，由我们格式化进 SQL；不存在用户可控字符串注入
  if (!usersDb.query("SELECT COALESCE(quota_bytes, 0) FROM users WHERE id = " +
                         std::to_string(userId),
                     rows, err))
    return 0;
  if (rows.empty() || rows[0].empty()) return 0;
  std::int64_t q = 0;
  try {
    q = std::stoll(rows[0][0]);
  } catch (...) {
    return 0;
  }
  return q > 0 ? q : cfg.defaultUserQuotaBytes;
}

// 配额拦截（PRD FR-7-6：≥80% 提醒；≥95% 阻止新上传并明确原因）。
// 返回 true = 放行；false = **已经写好响应**，调用方直接 return。
bool enforceQuota(cv::Db& usersDb, cv::FileRepository& repo, const cv::Config& cfg,
                  std::int64_t ownerId, std::int64_t incoming, cv::net::Response& resp) {
  const std::int64_t quota = userQuotaBytes(usersDb, cfg, ownerId);
  if (quota <= 0) return true;   // 不限

  std::int64_t used = 0;
  std::string  serr;
  if (!repo.sumSizeOfOwner(ownerId, used, serr)) {
    // 取不到用量时**放行 + 大声记日志**。这是个刻意的取舍：配额是防"把盘写满"的运维阈值，
    // 不是安全边界；为一次数据库抖动就把所有人的上传全拒掉，代价比超一点配额大得多。
    CV_LOG_WARN("配额检查跳过（读不到用量，按不限处理）: " << serr);
    return true;
  }

  const cv::quota::Decision d = cv::quota::evaluate(used, quota, incoming);
  if (d.verdict == cv::quota::Verdict::Reject) {
    resp.setError(413, d.reason);   // 413 Payload Too Large
    CV_LOG_WARN("拒绝上传（配额）: " << d.reason);
    return false;
  }
  if (d.verdict == cv::quota::Verdict::Warn) CV_LOG_WARN("配额预警: " << d.reason);
  return true;
}

// 恶意文件判定（清单 8.2 / 8.5）。返回 true = 放行；false = 已写好响应。
// ⚠️ 定位：这是**尽力而为的闸门**，不是安全边界 —— 默认 fail-open，远端服务抖动不该让网盘不可用。
bool enforceVirusScan(cv::Db& db, const cv::Config& cfg, const cv::scan::Fetcher& fetcher,
                      const std::string& contentHash, const std::string& name,
                      cv::net::Response& resp) {
  if (!cfg.scanEnabled) return true;
  // 非 64 位小写十六进制（空文件 / 异常路径）不查：查询键必须严格
  if (contentHash.size() != 64) return true;

  cv::scan::Policy policy;
  policy.blockThreshold = cfg.scanBlockThreshold;
  policy.failOpen       = cfg.scanFailOpen;

  const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  std::string            serr;
  const cv::scan::Result r = cv::scan::scan(db, contentHash, fetcher, policy,
                                            static_cast<std::int64_t>(nowMs),
                                            /*cacheEnabled=*/true, serr);
  if (!serr.empty()) CV_LOG_WARN("病毒扫描缓存读写异常: " << serr);

  if (r.decision == cv::scan::Decision::Block) {
    resp.setError(403, "文件未通过安全检测（" + cv::scan::verdictName(r.verdict) + "）：" +
                           r.detail + "。如确认是误报，请联系管理员调整判定阈值。");
    CV_LOG_WARN("拒绝上传（恶意文件判定）: " << name << " | " << cv::scan::verdictName(r.verdict)
                                            << " | " << r.detail);
    return false;
  }
  if (r.fetcherFailed)
    CV_LOG_WARN("恶意文件判定未取得远端结论，按策略放行: " << name << " | " << r.detail);
  else if (r.decision == cv::scan::Decision::AllowWithWarn)
    CV_LOG_WARN("恶意文件判定提示: " << name << " | " << cv::scan::verdictName(r.verdict) << " | "
                                    << r.detail);
  return true;
}

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

  // ---- 备份模式：--backup-db=<dir> 只做一次一致性热备然后退出，不启动服务 ----
  // 放在「目录已建、日志已就绪」之后、「打开任何库 / 监听端口」之前：
  // 此时本进程没有任何在途写入，且用的是 VACUUM INTO（只读源库）⇒ 服务端在跑也能安全备份。
  // 每次备份落到独立的 <dir>/<时间戳>/ 子目录：绝不覆盖上一份（备份最忌讳悄悄盖旧）。
  if (!cfg.backupDbDir.empty()) {
    char stamp[32] = {0};
    const std::time_t nowT = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&nowT));
    const std::string outDir = cfg.backupDbDir + "/" + stamp;

    cv::dbbackup::Stats bst;
    std::string         berr;
    if (!cv::dbbackup::backupAll(cfg, outDir, bst, berr)) {
      CV_LOG_ERROR("数据库备份失败: " << berr);
      return 1;
    }
    for (const std::string& p : bst.written) CV_LOG_INFO("已备份 " << p);
    for (const std::string& p : bst.skipped) CV_LOG_WARN("已跳过 " << p);
    CV_LOG_INFO("数据库备份完成：文件 " << bst.written.size() << " 个，共 " << bst.bytes
                                      << " 字节 → " << outDir);
    CV_LOG_INFO("恢复方式：停服后用备份文件覆盖 <dataDir>/meta/ 下同名文件，"
                "并删除残留的 -wal / -shm，再重启。");
    return 0;
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
  const std::string usersDbPath = cfg.usersDbPath();   // 路径定义收敛到 Config，避免两处漂移
  if (!usersDb.open(usersDbPath, err)) {
    CV_LOG_ERROR("打开账号数据库失败: " << err);
    return 1;
  }
  // users.db 加列迁移（PRD FR-1-4）：老库的 users 表没有 quota_bytes，而
  // `CREATE TABLE IF NOT EXISTS` 对已存在的表是**空操作**，不会补列 —— 必须显式 ALTER。
  // 放在 exec(schema) **之前**：此刻新库的表还不存在，PRAGMA 返回 0 行 → 跳过 ALTER，
  // 随后由 CREATE 直接带上该列；老库则在这里补列。两种情形都覆盖到。
  {
    std::vector<std::vector<std::string>> cols;
    std::string                           merr;
    if (usersDb.query("PRAGMA table_info(users)", cols, merr) && !cols.empty()) {
      bool has = false;
      for (const auto& r : cols)
        if (r.size() > 1 && r[1] == "quota_bytes") has = true;
      if (!has)
        usersDb.exec("ALTER TABLE users ADD COLUMN quota_bytes INTEGER NOT NULL DEFAULT 0", merr);
    }
  }
  if (!usersDb.exec(kUsersSchemaSql, err)) {
    CV_LOG_ERROR("账号库建表失败: " << err);
    return 1;
  }
  UserRepository usersRepo(usersDb);

  FileRepository repo(db);
  UploadRepository up(db);

  // 恶意文件判定：建表（幂等，按**内容哈希**存判定 ⇒ 同一内容跨文件跨用户只查一次）+ 装配远端查询。
  if (!cv::scan::initSchema(db, err)) CV_LOG_WARN("病毒扫描判定表创建失败: " << err);
  // ⚠️ 远端查询实现**本仓库尚未提供**：服务端是零第三方依赖 + 自研 POSIX socket，
  //    要发 HTTPS 必须自己写 HTTP 客户端并依赖 OpenSSL（本项目里 OpenSSL 是**可选**编译项）。
  //    这里按能力装配：没有实现就留空 ⇒ scan() 走 fail-open 并记录原因，
  //    功能退化为"只读本地缓存"，**不会发起任何外网请求、也不会让上传失败**。
  cv::scan::Fetcher scanFetcher =
      cv::scan::makeVirusTotalFetcher(cfg.scanApiKey, cfg.scanTimeoutMs);
  if (cfg.scanEnabled && !scanFetcher)
    CV_LOG_WARN("已启用恶意文件判定但未提供 API Key —— 只会使用本地缓存判定，不会发起外网请求。");
  if (cfg.scanEnabled)
    CV_LOG_INFO("远端查询：VirusTotal v3（www.virustotal.com，超时 " << cfg.scanTimeoutMs
                                                                 << " ms，只发内容哈希）");
  if (cfg.scanEnabled)
    CV_LOG_INFO("恶意文件判定已启用：按内容哈希查询（文件本身不出服务器），"
                "报毒引擎数 ≥ " << cfg.scanBlockThreshold << " 即拒绝，远端失败时"
                << (cfg.scanFailOpen ? "放行" : "拒绝"));
  ShareRepository shr(db);
  VersionRepository ver(db);
  TagRepository tagRepo(db);
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

  // ---- 启动期：清扫**没有对应会话行**的临时目录（崩溃残骸）----
  // 上面那个会话 GC 只能清"数据库里有的会话"，而崩溃可能死在建目录与插会话行之间，
  // 留下一个无主目录 —— 会话 GC 永远碰不到它，也不在 blobRoot 里（blob_gc 不管 tmp）。
  {
    std::vector<std::int64_t> liveIds;
    if (up.listAllIds(liveIds, err)) {
      const std::set<std::int64_t> active(liveIds.begin(), liveIds.end());
      cv::blobgc::TmpStats tst;
      std::string         terr;
      const auto          nowMs2 = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
      if (cv::blobgc::collectOrphanTmpDirs(cfg.tmpRoot() + "/uploads", active,
                                          /*graceMillis=*/3600LL * 1000LL,
                                          static_cast<std::int64_t>(nowMs2), false, tst, terr)) {
        if (tst.removed > 0)
          CV_LOG_INFO("临时目录清扫：删除 " << tst.removed << " 个无主目录，释放 "
                                            << tst.freedBytes << " 字节（保留：有主 "
                                            << tst.keptActive << "，宽限期内 " << tst.keptFresh << "）");
      } else {
        CV_LOG_WARN("临时目录清扫失败（不影响启动）: " << terr);
      }
    } else {
      CV_LOG_WARN("临时目录清扫跳过：无法取得会话清单 " << err);
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

    // 配额拦截：单发上传也要拦（大文件走分块通道，但这条路径同样能写盘）
    if (!enforceQuota(usersDb, repo, cfg, oid, static_cast<std::int64_t>(data.size()), resp))
      return;
    // 恶意文件判定：同样按内容哈希查
    if (!enforceVirusScan(db, cfg, scanFetcher, contentHash, name, resp)) return;

    std::int64_t id = 0;
    if (nameTaken) {
      // 覆盖：保留原 id 与名称，替换内容与分块清单
      id = sameName.id;
      // ★ 覆盖前先把旧内容存成一条历史版本 —— 否则"覆盖"就是变相的数据丢失，
      //   而且用户在界面上会觉得"我只是传错了一版，居然找不回来了"。零拷贝，不额外占盘。
      std::int64_t snapshotVid = 0;
      err.clear();
      if (!ver.captureCurrent(id, std::string(), "覆盖前备份", snapshotVid, err)) {
        resp.setError(500, std::string("db failed: ") + err);
        return;
      }
      pruneVersionsAndFree(ver, store, cfg, id);   // FR-4-2：覆盖后按策略清理旧版本
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

  // ---- DELETE /api/v1/files/:id （软删除 → 进回收站）----
  // 语义变更（配合回收站落地）：本端点从"永久删除"改为"移入回收站"。
  // 彻底删除改走 POST /api/v1/trash/purge。理由：界面上「删除」按钮的直觉就是
  // 「先扔进回收站，后悔还来得及」，而不是当场灰飞烟灭。
  // 进站**不释放**任何空间：不删镜像、不减引用计数 —— 否则恢复就成了空话。
  // ---- POST /api/v1/files/:id/move （把文件移动到另一个目录）----
  // body: { "dir": "<目标目录，空串 = 根>" }
  // 元数据由仓储层改（纯元数据、单事务）；磁盘镜像用 fs::rename 同步搬移
  // —— 是 O(1) 改名，**不遍历复制任何物理文件**。镜像失败只告警：DB 才是权威。
  server.route("POST", "/api/v1/files/:id/move",
               [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    const std::int64_t oid = scopeOwner(req.authUserId);
    std::int64_t id = 0;
    if (!parseId(req.param("id"), id)) {
      resp.setError(404, "file not found");
      return;
    }
    json::Value body = parseJsonBody(req.body, perr);
    if (!perr.empty()) {
      resp.setError(400, "invalid json: " + perr);
      return;
    }
    const json::Value* pdir = body.find("dir");
    if (!pdir || pdir->type() != json::Value::Type::String) {
      resp.setError(400, "missing dir");
      return;
    }
    std::string newDir;
    if (!sanitizeDirParam(pdir->stringValue(), newDir, resp)) return;   // 空串 = 根，合法

    cv::FileRow before;
    // owner 不匹配一律按"查不到"处理，避免泄漏他人文件是否存在
    if (!repo.findById(id, before, perr) || before.ownerId != oid) {
      resp.setError(404, "file not found");
      return;
    }

    if (!repo.moveFileToDir(oid, id, newDir, perr)) {
      resp.setError(perr == "file not found" ? 404 : 409, perr);
      return;
    }

    std::error_code rec;
    if (before.dir != newDir) {
      fs::rename(mirrorPathFor(cfg, oid, before.dir, before.name),
                 mirrorPathFor(cfg, oid, newDir, before.name), rec);
      if (rec) CV_LOG_WARN("移动文件镜像失败（元数据已更新）: " << rec.message());
    }

    json::Value v = json::Value::object();
    v.set("id", static_cast<long long>(id));
    v.set("dir", newDir);
    resp.setJson(200, json::dump(v));
    CV_LOG_INFO("移动文件 " << before.name << " → " << (newDir.empty() ? "(根)" : newDir));
  });

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
                 const std::int64_t nowMs = nowMillis();
                 if (!repo.trashFile(id, nowMs, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 json::Value v = json::Value::object();
                 v.set("trashed", true);
                 v.set("file_id", static_cast<long long>(id));
                 v.set("name", row.name);
                 v.set("dir", row.dir);
                 v.set("deleted_at", static_cast<long long>(nowMs));
                 resp.setJson(200, json::dump(v));
                 CV_LOG_INFO("移入回收站 id=" << id << " dir=" << row.dir << " name=" << row.name);
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
    // 配额（PRD FR-1-4 / FR-7-6）：客户端据此显示余量并在 ≥80% 时提醒。
    // quota_bytes = 0 表示不限（前端不要拿它做除法）。
    {
      const std::int64_t quota = userQuotaBytes(usersDb, cfg, scopeOwner(req.authUserId));
      v.set("quota_bytes", static_cast<long long>(quota));
      v.set("quota_percent", static_cast<long long>(
                                 quota > 0 ? cv::quota::percentOf(used, quota) : 0));
      v.set("quota_unlimited", static_cast<bool>(quota <= 0));
    }
      }
    }
    resp.setJson(200, json::dump(v));
  });

  // ---- GET /api/v1/files ----
  // ---- GET /api/v1/files ----
  // 可选过滤：dir=<目录>（**空值 = 根目录**）｜ under=<前缀>（含子目录，同步模块用）
  // 可选分页：limit（默认 1000，上限 5000）｜ offset
  // 返回 { items, total, limit, offset, hasMore }
  //
  // ⚠️ 此处**不再固定 LIMIT 500**：那会让"服务端截断 + 客户端拉全表后本地过滤"叠加成
  //    正确性缺陷（>500 文件的账号里，排在 500 名之外的目录显示为空）。
  //    现在 total 是**真实总数**，客户端必须按 hasMore 翻页。
  server.route("GET", "/api/v1/files",
               [&](const net::Request& req, net::Response& resp) {
    std::string err;
    cv::FileRepository::FileListQuery q;

    if (queryHasKey(req.query, "dir")) {
      std::string norm;
      if (!sanitizeDirParam(util::urlDecode(queryParam(req.query, "dir")), norm, resp)) return;
      q.filterByDir = true;
      q.dirExact    = norm;      // 可能为空串 = 根目录（故用 filterByDir 单独置位）
    } else if (queryHasKey(req.query, "under")) {
      std::string norm;
      if (!sanitizeDirParam(util::urlDecode(queryParam(req.query, "under")), norm, resp)) return;
      if (!norm.empty()) {
        q.filterByUnder = true;
        q.underPrefix   = norm;
      }
    }

    q.limit  = kDefaultFileListLimit;
    q.offset = 0;
    if (queryHasKey(req.query, "limit")) {
      const long long v = std::strtoll(queryParam(req.query, "limit").c_str(), nullptr, 10);
      if (v > 0) q.limit = (v > kMaxFileListLimit) ? kMaxFileListLimit : v;
    }
    if (queryHasKey(req.query, "offset")) {
      const long long v = std::strtoll(queryParam(req.query, "offset").c_str(), nullptr, 10);
      if (v > 0) q.offset = v;
    }

    std::vector<FileRow> rows;
    std::int64_t total = 0;
    // 列表按 owner 过滤（SQL 层过滤，避免 C++ 侧泄漏他人元数据）
    if (!repo.listFiles(scopeOwner(req.authUserId), q, rows, total, err)) {
      resp.setError(500, std::string("db failed: ") + err);
      return;
    }
    json::Value arr = json::Value::array();
    for (const FileRow& f : rows) arr.push_back(fileToJson(f));
    json::Value v = json::Value::object();
    v.set("total", static_cast<long long>(total));
    v.set("limit", static_cast<long long>(q.limit));
    v.set("offset", static_cast<long long>(q.offset));
    v.set("hasMore",
          static_cast<bool>(q.offset + static_cast<std::int64_t>(rows.size()) < total));
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

  // ---- POST /api/v1/dirs/move （目录重命名 / 移动）----
  // body: { "from": "...", "to": "..." }（to 为完整目标相对路径）
  // 同一份实现兼作重命名（同父换名）与移动（换父）。**纯元数据**：只改 dir_node.path 与
  // file_dir.dir 的前缀，不遍历复制物理文件；磁盘镜像目录用一次 fs::rename 搬走。
  server.route("POST", "/api/v1/dirs/move",
               [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    const std::int64_t oid = scopeOwner(req.authUserId);
    json::Value body = parseJsonBody(req.body, perr);
    if (!perr.empty()) {
      resp.setError(400, "invalid json: " + perr);
      return;
    }
    const json::Value* pfrom = body.find("from");
    const json::Value* pto   = body.find("to");
    if (!pfrom || !pto || pfrom->type() != json::Value::Type::String ||
        pto->type() != json::Value::Type::String) {
      resp.setError(400, "missing from/to");
      return;
    }
    std::string from, to;
    if (!sanitizeDirParam(pfrom->stringValue(), from, resp)) return;
    if (!sanitizeDirParam(pto->stringValue(), to, resp)) return;

    if (!repo.moveDir(oid, from, to, perr)) {
      resp.setError(409, perr);
      return;
    }

    // 镜像目录同步搬移（同样是一次改名，不是逐个文件复制）
    std::error_code rec;
    const std::string oldMirror = userFilesRoot(cfg, oid) + "/" + from;
    const std::string newMirror = userFilesRoot(cfg, oid) + "/" + to;
    if (fs::exists(oldMirror, rec)) {
      rec.clear();
      fs::create_directories(fs::path(newMirror).parent_path(), rec);
      rec.clear();
      fs::rename(oldMirror, newMirror, rec);
      if (rec) CV_LOG_WARN("移动目录镜像失败（元数据已更新）: " << rec.message());
    }

    json::Value v = json::Value::object();
    v.set("from", from);
    v.set("to", to);
    resp.setJson(200, json::dump(v));
    CV_LOG_INFO("移动/重命名目录 " << from << " → " << to);
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
                  // ---- 秒传前置守卫：先确认"命中"的元数据背后，磁盘上真的有完整内容 ----
                  bool instantOk = repo.findByContentHash(fileHash, existing, perr);
                  std::vector<std::string> ch;
                  std::vector<std::size_t> cs;
                  if (instantOk) {
                    if (!repo.chunkHashesOf(existing.id, ch, perr) ||
                        !repo.chunkSizesOf(ch, cs, perr)) {
                      resp.setError(500, std::string("db failed: ") + perr);
                      return;
                    }
                    // 尺寸不符（断电留下的 0 字节/截断 blob）→ 不认这次秒传，
                    // 退回下方正常上传路径：那里会真正重写分块，顺带覆盖修复坏 blob。
                    if (!chunksIntactOnDisk(store, ch, cs)) {
                      CV_LOG_WARN("秒传守卫：命中内容在磁盘上缺失或尺寸不符，退回正常上传以自愈"
                                  " hash=" << fileHash << " file_id=" << existing.id);
                      instantOk = false;
                      ch.clear();
                      cs.clear();
                    }
                  }
                  if (instantOk) {
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
                     const std::int64_t plainSize =
                         sec ? 0 : static_cast<std::int64_t>(fsize);
                     // plainSize 用于去重时的尺寸校验；本函数内部还会 fsync 目标 blob ——
                     // 必须在此完成，因为紧接着的 insertFile 会提交引用这些 blob 的 DB 事务，
                     // 而 DB 是 synchronous=FULL（真落盘）。顺序颠倒即断电静默损坏。
                     if (!store.putFromFile(cr.sha256, part, plainSize, perr)) {
                       resp.setError(500, std::string("store failed: ") + perr);
                       return;
                     }
                     chunkHashes.push_back(cr.sha256);
                     chunkSizes.push_back(static_cast<std::size_t>(plainSize));
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
                 // 配额拦截（权威点）：这里才知道**真实**字节数。init 时拦的是客户端自报的 size，
                 // 报小了就得靠这一道。⚠️ 覆盖场景按完整新尺寸计（保守），不做"减去旧尺寸"的净额计算。
                 if (!enforceQuota(usersDb, repo, cfg, s.ownerId,
                                   static_cast<std::int64_t>(s.size), resp))
                   return;
                 // 恶意文件判定：用**内容哈希**（计算结果 computed 此时已确定）
                 if (!enforceVirusScan(db, cfg, scanFetcher, computed, s.name, resp)) return;
                 // 覆盖：同目录同名已存在（且归属本会话 owner）且会话声明了 overwrite → 替换原记录内容
                 FileRow sameName;
                 bool replace = overwrite && repo.findByPath(s.ownerId, dir, s.name, sameName, perr);
                 perr.clear();
                 if (replace) {
                   fileId = sameName.id;
                   // ★ 覆盖前把旧内容存成历史版本（与整文件上传路径同一策略；
                   //   分块上传是"大文件覆盖"的主通道，两条路径都必须挂上）
                   std::int64_t snapshotVid = 0;
                   if (!ver.captureCurrent(fileId, std::string(), "覆盖前备份", snapshotVid, perr)) {
                     resp.setError(500, std::string("db failed: ") + perr);
                     return;
                   }
                   pruneVersionsAndFree(ver, store, cfg, fileId);   // FR-4-2
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

  // ================= 回收站（软删除 → 恢复 → 彻底删除） =================
  //
  // 语义总纲（与 meta/schema.h 顶部「回收站语义」严格一致，改这里请同步改那边）：
  //   · 进站   = deleted=1 + deleted_at=now，**保留** file_chunk / file_dir / ref_count
  //   · 恢复   = deleted=0 + deleted_at=0（内容从未被碰过，"恢复"只是改个标志位）
  //   · 彻底删 = 真删 file_chunk、递减 ref_count、清 file_dir / file_tag / 版本快照、删镜像
  //   · 目录同样进回收站：否则"删掉一个文件夹"在回收站里会散成 N 个文件，反直觉。
  //     目录进站时其下文件一并标记；恢复 / 彻底删除同理 —— 让"文件夹"始终是一件事。
  //
  // 计数与配额的取舍（**实测口径，勿凭直觉改**）：
  //   · 磁盘：进站**不释放**任何东西 —— 分块引用计数原样保留、内容库不做 GC，
  //     blob 一个都没少。所以回收站里的文件确实还占着磁盘。
  //   · 「已用空间」：`sumSizeOfOwner` 带 `WHERE deleted = 0` ⇒ 进站后该文件立刻
  //     从 /api/v1/storage 的 used_bytes 里**消失**，界面上的用量会当场下降。
  //   两者会同时成立、且必然不一致 —— 这是"用量"被定义成"可见文件占用量"而非
  //   "磁盘实际占用量"的结果。界面上必须把这两件事分开讲，不能混成一句
  //   "仍然占用空间"（那会和用量数字当场打脸）。
  //
  // ⚠️ 为什么全部走 POST + JSON body，而不是 DELETE /files/dir%3Axxx：
  //    http_server::dispatch 把路径段原样放进 params，**不做百分号解码**，
  //    `dir:照片/2026` 这种含冒号 + 中文的标识塞进路径永远比不中。放 body 里才干净。

  // 彻底清理单个文件的全部痕迹：内容分块 + 版本分块 + 镜像 + blob。返回释放字节数，失败 -1。
  // 顺序不可颠簸：先 purgeFile（清 file_chunk，使当前内容的引用归零），
  // 再 purgeFileVersions（此时只剩版本侧还持有引用，由它释放），最后才删 blob。
  auto purgeOneFile = [&](const FileRow& f, std::string& err) -> std::int64_t {
    std::vector<std::string> orphans;
    if (!repo.purgeFile(f.id, orphans, err)) return -1;
    std::vector<std::string> versionOrphans;
    if (!ver.purgeFileVersions(f.id, versionOrphans, err)) return -1;
    for (const std::string& h : versionOrphans) orphans.push_back(h);

    std::error_code rec;
    fs::remove(mirrorPathFor(cfg, f.ownerId, f.dir, f.name), rec);
    return dropOrphanBlobs(store, orphans);
  };

  // 移入回收站。幂等：trashFile 自带 `AND deleted = 0` 守卫，重复进站不会刷新时间戳。
  auto doTrash = [&](std::int64_t auth, const TrashTarget& t, json::Value& out,
                     std::string& err) -> bool {
    const std::int64_t oid = scopeOwner(auth);
    const std::int64_t nowMs = nowMillis();

    if (!t.isDir) {
      FileRow row;
      if (!repo.findById(t.fileId, row, err) || !ownerVisible(row, auth)) {
        err = "file not found";
        return false;
      }
      if (!repo.trashFile(t.fileId, nowMs, err)) return false;
      out.set("file_id", static_cast<long long>(t.fileId));
      out.set("name", row.name);
      out.set("dir", row.dir);
      out.set("deleted_at", static_cast<long long>(nowMs));
      return true;
    }

    // 目录：自身 + 全部后代目录 + 其下全部未删除文件
    std::vector<std::string> allDirs;
    if (!repo.listDirPathsUnder(oid, t.dir, allDirs, err)) return false;
    if (allDirs.empty()) {
      err = "dir not found";
      return false;
    }
    std::vector<std::int64_t> fileIds;
    if (!repo.listFileIdsUnderDir(oid, t.dir, fileIds, err)) return false;
    if (!repo.setDirsTrashed(oid, allDirs, true, nowMs, err)) return false;
    for (std::int64_t fid : fileIds) {
      if (!repo.trashFile(fid, nowMs, err)) return false;
    }
    out.set("dir", t.dir);
    out.set("dir_count", static_cast<long long>(allDirs.size()));
    out.set("file_count", static_cast<long long>(fileIds.size()));
    out.set("deleted_at", static_cast<long long>(nowMs));
    return true;
  };

  // 从回收站恢复。
  // ⚠️ 关键边界：文件要回到原目录，而那个目录**可能自己也在回收站里**。此时如果只管
  //    把文件置回未删除，结果就是"恢复成功了但列表里死活看不见"——最气人的那种成功。
  //    故这里顺带把父目录链一并复位（不存在则补登记），并在响应里用 dir_restored 说明。
  auto doRestore = [&](std::int64_t auth, const TrashTarget& t, json::Value& out,
                       std::string& err) -> bool {
    const std::int64_t oid = scopeOwner(auth);

    // 确保目录链存在且处于未删除态（幂等）
    auto ensureChain = [&](const std::string& dir) -> bool {
      if (dir.empty()) return true;  // 根目录恒定存在
      const std::vector<std::string> chain = pathPrefixChain(dir);
      for (const std::string& p : chain) {
        bool created = false;
        if (!repo.createDir(oid, p, created, err)) return false;
      }
      return repo.setDirsTrashed(oid, chain, false, 0, err);
    };

    if (!t.isDir) {
      std::string outDir;
      if (!repo.restoreFile(t.fileId, outDir, err)) {
        err = "file not found";
        return false;
      }
      bool dirWasTrashed = false;
      repo.isDirDeleted(oid, outDir, dirWasTrashed, err);
      err.clear();
      if (dirWasTrashed && !outDir.empty()) {
        if (!ensureChain(outDir)) return false;
      }
      out.set("file_id", static_cast<long long>(t.fileId));
      out.set("dir", outDir);
      out.set("dir_restored", dirWasTrashed && !outDir.empty());
      return true;
    }

    std::vector<std::string> allDirs;
    if (!repo.listDirPathsUnder(oid, t.dir, allDirs, err)) return false;
    if (allDirs.empty()) {
      err = "dir not found";
      return false;
    }
    // 先取该目录的进站时间（复位之后就查不到了）—— 用于精确圈定"随它一起进站"的文件
    std::int64_t dirDeletedAt = 0;
    {
      std::vector<TrashDirRow> trashed;
      if (!repo.listTrashDirs(oid, trashed, err)) return false;
      for (const TrashDirRow& d : trashed) {
        if (d.path == t.dir) {
          dirDeletedAt = d.deletedAt;
          break;
        }
      }
    }
    if (!ensureChain(t.dir)) return false;                              // 祖先 + 自身
    if (!repo.setDirsTrashed(oid, allDirs, false, 0, err)) return false;  // 全部后代

    std::vector<FileRow> kids;
    if (!repo.listTrashedFilesUnderDir(oid, t.dir, dirDeletedAt, kids, err)) return false;
    for (const FileRow& k : kids) {
      std::string ignored;
      if (!repo.restoreFile(k.id, ignored, err)) return false;
    }
    out.set("dir", t.dir);
    out.set("dir_count", static_cast<long long>(allDirs.size()));
    out.set("file_count", static_cast<long long>(kids.size()));
    return true;
  };

  // 彻底删除（不可恢复）。文件先清内容与版本，目录先清其下文件再删目录行。
  auto doPurge = [&](std::int64_t auth, const TrashTarget& t, json::Value& out,
                     std::string& err) -> bool {
    const std::int64_t oid = scopeOwner(auth);
    std::int64_t freed = 0;
    std::int64_t removed = 0;

    if (!t.isDir) {
      FileRow row;
      // 回收站里的文件用 findById 查不到（它带 deleted=0 过滤），必须走 findByIdAny
      if (!repo.findByIdAny(t.fileId, row, err) || !ownerVisible(row, auth)) {
        err = "file not found";
        return false;
      }
      const std::int64_t f = purgeOneFile(row, err);
      if (f < 0) return false;
      freed += f;
      removed = 1;
      out.set("file_id", static_cast<long long>(t.fileId));
      out.set("name", row.name);
    } else {
      std::vector<std::string> allDirs;
      if (!repo.listDirPathsUnder(oid, t.dir, allDirs, err)) return false;
      if (allDirs.empty()) {
        err = "dir not found";
        return false;
      }
      // 目录下待清的文件 = 已进站的 + 万一还以未删除状态残留的（异常数据兜底，
      // 反正目录都要没了，不能留一堆悬空文件）
      std::vector<FileRow> kids;
      if (!repo.listTrashedFilesUnderDir(oid, t.dir, 0, kids, err)) return false;
      std::vector<std::int64_t> liveIds;
      if (!repo.listFileIdsUnderDir(oid, t.dir, liveIds, err)) return false;
      for (std::int64_t fid : liveIds) {
        FileRow fr;
        if (repo.findById(fid, fr, err)) kids.push_back(fr);
        err.clear();
      }
      for (const FileRow& k : kids) {
        const std::int64_t f = purgeOneFile(k, err);
        if (f < 0) return false;
        freed += f;
        ++removed;
      }
      if (!repo.deleteDirRows(oid, allDirs, err)) return false;
      out.set("dir", t.dir);
      out.set("dir_count", static_cast<long long>(allDirs.size()));
    }
    out.set("file_count", static_cast<long long>(removed));
    out.set("freed_bytes", static_cast<long long>(freed));
    out.set("disk_free_bytes", static_cast<long long>(diskFreeBytes(cfg.dataDir)));
    return true;
  };

  // 把重定向到的目标错误文本映射成 HTTP 状态码（"not found" → 404，其余 500）
  auto statusForErr = [](const std::string& e) {
    return (e == "file not found" || e == "dir not found") ? 404 : 500;
  };

  // ---- GET /api/v1/trash —— 回收站列表（顶层目录 + 顶层文件）----
  server.route("GET", "/api/v1/trash", [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    const std::int64_t oid = scopeOwner(req.authUserId);
    std::vector<FileRow> files;
    std::vector<TrashDirRow> dirs;
    if (!repo.listTrash(oid, files, dirs, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    json::Value items = json::Value::array();
    std::int64_t usedBytes = 0;
    for (const FileRow& f : files) {
      json::Value o = fileToJson(f);
      o.set("deleted_at", static_cast<long long>(f.deletedAt));
      items.push_back(o);
      usedBytes += f.size;
    }
    json::Value dirArr = json::Value::array();
    for (const TrashDirRow& d : dirs) {
      // 目录占用 = 其下（含子目录）全部已进站文件之和。一次查询搞定，别按文件数 N+1。
      std::vector<FileRow> kids;
      if (!repo.listTrashedFilesUnderDir(oid, d.path, 0, kids, perr)) {
        resp.setError(500, std::string("db failed: ") + perr);
        return;
      }
      std::int64_t dirBytes = 0;
      for (const FileRow& k : kids) dirBytes += k.size;

      json::Value o = json::Value::object();
      o.set("path", d.path);
      o.set("name", treeLastSegment(d.path));
      o.set("size", static_cast<long long>(dirBytes));
      o.set("file_count", static_cast<long long>(kids.size()));
      o.set("deleted_at", static_cast<long long>(d.deletedAt));
      dirArr.push_back(o);
      usedBytes += dirBytes;
    }
    json::Value v = json::Value::object();
    v.set("items", items);
    v.set("dirs", dirArr);
    v.set("total", static_cast<long long>(files.size() + dirs.size()));
    v.set("file_count", static_cast<long long>(files.size()));
    v.set("dir_count", static_cast<long long>(dirs.size()));
    v.set("used_bytes", static_cast<long long>(usedBytes));
    resp.setJson(200, json::dump(v));
  });

  // ---- POST /api/v1/trash —— 移入回收站（body: {"file_id":<id>} 或 {"dir":"a/b"}）----
  server.route("POST", "/api/v1/trash", [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    json::Value body = parseJsonBody(req.body, perr);
    if (!perr.empty()) {
      resp.setError(400, "invalid json: " + perr);
      return;
    }
    TrashTarget t;
    if (!parseTrashTarget(body, t, perr)) {
      resp.setError(400, perr);
      return;
    }
    json::Value v = json::Value::object();
    if (!doTrash(req.authUserId, t, v, perr)) {
      resp.setError(statusForErr(perr), perr);
      return;
    }
    v.set("trashed", true);
    resp.setJson(200, json::dump(v));
  });

  // ---- POST /api/v1/trash/restore —— 恢复（body 同上）----
  server.route("POST", "/api/v1/trash/restore",
               [&](const net::Request& req, net::Response& resp) {
                 std::string perr;
                 json::Value body = parseJsonBody(req.body, perr);
                 if (!perr.empty()) {
                   resp.setError(400, "invalid json: " + perr);
                   return;
                 }
                 TrashTarget t;
                 if (!parseTrashTarget(body, t, perr)) {
                   resp.setError(400, perr);
                   return;
                 }
                 json::Value v = json::Value::object();
                 if (!doRestore(req.authUserId, t, v, perr)) {
                   resp.setError(statusForErr(perr), perr);
                   return;
                 }
                 v.set("restored", true);
                 resp.setJson(200, json::dump(v));
                 CV_LOG_INFO("恢复 "
                             << (t.isDir ? std::string("目录 ") + t.dir
                                         : std::string("文件 id=") + std::to_string(t.fileId)));
               });

  // ---- POST /api/v1/trash/purge —— 彻底删除（不可恢复；body 同上）----
  server.route("POST", "/api/v1/trash/purge", [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    json::Value body = parseJsonBody(req.body, perr);
    if (!perr.empty()) {
      resp.setError(400, "invalid json: " + perr);
      return;
    }
    TrashTarget t;
    if (!parseTrashTarget(body, t, perr)) {
      resp.setError(400, perr);
      return;
    }
    json::Value v = json::Value::object();
    if (!doPurge(req.authUserId, t, v, perr)) {
      resp.setError(statusForErr(perr), perr);
      return;
    }
    v.set("purged", true);
    resp.setJson(200, json::dump(v));
    CV_LOG_INFO("彻底删除 "
                << (t.isDir ? std::string("目录 ") + t.dir
                            : std::string("文件 id=") + std::to_string(t.fileId))
                << "，释放 " << v.find("freed_bytes")->numberValue() << " 字节");
  });

  // ---- POST /api/v1/trash/empty —— 清空回收站（不可恢复）----
  // ⚠️ 路由注册顺序：本行与 /trash/cleanup 段数（4）与 /trash（3）不同，不冲突；
  //    但若将来新增 /trash/:id 形式的通配路由，**必须**排在本行之后，否则会被吃掉。
  server.route("POST", "/api/v1/trash/empty", [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    const std::int64_t oid = scopeOwner(req.authUserId);
    std::vector<std::int64_t> ids;
    if (!repo.listAllTrashedIds(oid, ids, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    std::vector<TrashDirRow> trashedDirs;
    if (!repo.listTrashDirs(oid, trashedDirs, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    std::int64_t freed = 0;
    std::int64_t removed = 0;
    for (std::int64_t id : ids) {
      FileRow row;
      if (!repo.findByIdAny(id, row, perr)) {
        perr.clear();
        continue;
      }
      const std::int64_t f = purgeOneFile(row, perr);
      if (f < 0) {
        resp.setError(500, perr);
        return;
      }
      freed += f;
      ++removed;
    }
    std::vector<std::string> dirPaths;
    for (const TrashDirRow& d : trashedDirs) dirPaths.push_back(d.path);
    if (!repo.deleteDirRows(oid, dirPaths, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    json::Value v = json::Value::object();
    v.set("removed", static_cast<long long>(removed));
    v.set("dirs_removed", static_cast<long long>(dirPaths.size()));
    v.set("freed_bytes", static_cast<long long>(freed));
    v.set("disk_free_bytes", static_cast<long long>(diskFreeBytes(cfg.dataDir)));
    resp.setJson(200, json::dump(v));
    CV_LOG_INFO("清空回收站 owner=" << oid << " 文件 " << removed << " 个 / 目录 "
                                    << dirPaths.size() << " 个，释放 " << freed << " 字节");
  });

  // ---- POST /api/v1/trash/cleanup —— 清理已过保留期的条目（body 可选 {"retention_days":30}）----
  server.route("POST", "/api/v1/trash/cleanup",
               [&](const net::Request& req, net::Response& resp) {
                 std::string perr;
                 json::Value body = parseJsonBody(req.body, perr);
                 perr.clear();  // body 可空：空 body 一律按默认保留期处理，不报错
                 std::int64_t days = 30;
                 const json::Value* pd = body.find("retention_days");
                 if (pd && pd->type() == json::Value::Type::Number) {
                   const double dv = pd->numberValue();
                   if (dv > 0 && dv <= 3650) days = static_cast<std::int64_t>(dv);
                 }
                 const std::int64_t oid = scopeOwner(req.authUserId);
                 const std::int64_t deadline = nowMillis() - days * 24LL * 3600 * 1000;

                 std::vector<std::int64_t> ids;
                 if (!repo.listExpiredTrashIds(oid, deadline, ids, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 std::int64_t freed = 0;
                 std::int64_t removed = 0;
                 for (std::int64_t id : ids) {
                   FileRow row;
                   if (!repo.findByIdAny(id, row, perr)) {
                     perr.clear();
                     continue;
                   }
                   const std::int64_t f = purgeOneFile(row, perr);
                   if (f < 0) {
                     resp.setError(500, perr);
                     return;
                   }
                   freed += f;
                   ++removed;
                 }
                 std::vector<TrashDirRow> trashedDirs;
                 if (!repo.listTrashDirs(oid, trashedDirs, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 std::vector<std::string> dirPaths;
                 for (const TrashDirRow& d : trashedDirs) {
                   if (d.deletedAt > 0 && d.deletedAt < deadline) dirPaths.push_back(d.path);
                 }
                 if (!repo.deleteDirRows(oid, dirPaths, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 json::Value v = json::Value::object();
                 v.set("removed", static_cast<long long>(removed));
                 v.set("dirs_removed", static_cast<long long>(dirPaths.size()));
                 v.set("freed_bytes", static_cast<long long>(freed));
                 v.set("retention_days", static_cast<long long>(days));
                 resp.setJson(200, json::dump(v));
               });

  // ================= 文件版本（快照 / 回滚 / 删除） =================
  //
  // 语义：file_version 只存**历史快照**；文件"当前内容"始终在 file_node 里。
  //   · 列表 = 一条合成的"当前版本" + 全部历史快照（version 号降序）
  //   · 内容被覆盖 / 回滚之前，都会先 captureCurrent() 把旧内容存档 —— 所以"回滚"永远
  //     不会把当下这版弄丢（它会被自动备份成一条"恢复前备份"）
  //   · 回滚 = 把目标版本的分块清单写回 file_node，**零拷贝**（blob 一直在内容库里）

  // ---- GET /api/v1/files/:id/versions ----
  server.route("GET", "/api/v1/files/:id/versions",
               [&](const net::Request& req, net::Response& resp) {
                 std::string perr;
                 const std::int64_t auth = req.authUserId;
                 std::int64_t id = 0;
                 if (!parseId(req.param("id"), id)) {
                   resp.setError(404, "file not found");
                   return;
                 }
                 FileRow row;
                 if (!repo.findById(id, row, perr) || !ownerVisible(row, auth)) {
                   resp.setError(404, "file not found");
                   return;
                 }
                 std::vector<VersionRow> vers;
                 if (!ver.listVersions(id, vers, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 std::int64_t curNo = 1;
                 if (!ver.currentVersionNo(id, curNo, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }

                 json::Value arr = json::Value::array();
                 {
                   // 合成的"当前版本"：内容取自 file_node，不是快照表
                   VersionRow cur;
                   cur.id = 0;
                   cur.fileId = id;
                   cur.versionNo = curNo;
                   cur.contentHash = row.contentHash;
                   cur.size = row.size;
                   cur.chunkCount = row.chunkCount;
                   cur.createdAt = row.createdAt;
                   cur.note = "当前版本";
                   arr.push_back(versionToJson(cur, true));
                 }
                 for (const VersionRow& v : vers) arr.push_back(versionToJson(v, false));

                 json::Value out = json::Value::object();
                 out.set("file_id", static_cast<long long>(id));
                 out.set("name", row.name);
                 out.set("dir", row.dir);
                 out.set("items", arr);
                 out.set("total", static_cast<long long>(vers.size() + 1));
                 out.set("current_version", static_cast<long long>(curNo));
                 out.set("history_count", static_cast<long long>(vers.size()));
                 resp.setJson(200, json::dump(out));
               });

  // ---- POST /api/v1/files/:id/versions —— 手动打一个快照（body 可选 {"note":..,"device":..}）----
  server.route("POST", "/api/v1/files/:id/versions",
               [&](const net::Request& req, net::Response& resp) {
                 std::string perr;
                 const std::int64_t auth = req.authUserId;
                 std::int64_t id = 0;
                 if (!parseId(req.param("id"), id)) {
                   resp.setError(404, "file not found");
                   return;
                 }
                 FileRow row;
                 if (!repo.findById(id, row, perr) || !ownerVisible(row, auth)) {
                   resp.setError(404, "file not found");
                   return;
                 }
                 json::Value body = parseJsonBody(req.body, perr);
                 perr.clear();  // body 可空
                 std::string note = "手动快照";
                 std::string device;
                 const json::Value* pn = body.find("note");
                 if (pn && pn->type() == json::Value::Type::String && !pn->stringValue().empty()) {
                   note = pn->stringValue();
                 }
                 const json::Value* pdev = body.find("device");
                 if (pdev && pdev->type() == json::Value::Type::String) {
                   device = pdev->stringValue();
                 }

                 std::int64_t vid = 0;
                 if (!ver.captureCurrent(id, device, note, vid, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 pruneVersionsAndFree(ver, store, cfg, id);   // FR-4-2：手动快照后同样清理
                 VersionRow vr;
                 if (!ver.findVersion(vid, vr, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 resp.setJson(201, json::dump(versionToJson(vr, false)));
                 CV_LOG_INFO("手动快照 file_id=" << id << " version_id=" << vid);
               });

  // ---- POST /api/v1/files/:id/versions/:vid/restore —— 回滚到某个历史版本 ----
  server.route("POST", "/api/v1/files/:id/versions/:vid/restore",
               [&](const net::Request& req, net::Response& resp) {
                 std::string perr;
                 const std::int64_t auth = req.authUserId;
                 std::int64_t id = 0;
                 std::int64_t vid = 0;
                 if (!parseId(req.param("id"), id) || !parseId(req.param("vid"), vid)) {
                   resp.setError(404, "file or version not found");
                   return;
                 }
                 FileRow row;
                 if (!repo.findById(id, row, perr) || !ownerVisible(row, auth)) {
                   resp.setError(404, "file not found");
                   return;
                 }
                 VersionRow vr;
                 if (!ver.findVersion(vid, vr, perr) || vr.fileId != id) {
                   resp.setError(404, "version not found");
                   return;
                 }
                 // 取目标版本的分块清单（零拷贝：blob 早就在内容库里躺着）
                 std::vector<std::string> hashes;
                 std::vector<std::size_t> sizes;
                 if (!ver.versionChunks(vid, hashes, sizes, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 if (hashes.empty() && vr.size > 0) {
                   resp.setError(500, "version content missing");
                   return;
                 }
                 // 先备份当前内容 —— 否则"回滚"就成了单向删除，回滚一次就永久丢掉刚才那版
                 std::int64_t backupVid = 0;
                 if (!ver.captureCurrent(id, std::string(), "恢复前备份", backupVid, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 pruneVersionsAndFree(ver, store, cfg, id);   // FR-4-2：回滚前备份后同样清理
                 std::vector<std::string> orphans;
                 if (!repo.replaceContent(id, vr.size, vr.contentHash, hashes, sizes, orphans,
                                          perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 const std::int64_t freed = dropOrphanBlobs(store, orphans);
                 // 镜像重建（尽力而为；blob + DB 才是权威）
                 if (mirrorEnabled) {
                   std::string mirrorErr;
                   if (!store.materializeFromChunks(
                           hashes, mirrorPathFor(cfg, row.ownerId, row.dir, row.name),
                           mirrorErr)) {
                     CV_LOG_WARN("回滚后重建镜像失败 id=" << id << ": " << mirrorErr);
                   }
                 }
                 std::int64_t nowNo = 1;
                 ver.currentVersionNo(id, nowNo, perr);

                 json::Value v = json::Value::object();
                 v.set("file_id", static_cast<long long>(id));
                 v.set("restored_from", static_cast<long long>(vr.versionNo));
                 v.set("backup_version_id", static_cast<long long>(backupVid));
                 v.set("now_version", static_cast<long long>(nowNo));
                 v.set("size", static_cast<long long>(vr.size));
                 v.set("freed_bytes", static_cast<long long>(freed));
                 resp.setJson(200, json::dump(v));
                 CV_LOG_INFO("回滚 file_id=" << id << " → v" << vr.versionNo
                                             << "（回滚前已自动备份为 version_id=" << backupVid
                                             << "）");
               });

  // ---- DELETE /api/v1/files/:id/versions/:vid —— 删掉某个历史版本 ----
  server.route("DELETE", "/api/v1/files/:id/versions/:vid",
               [&](const net::Request& req, net::Response& resp) {
                 std::string perr;
                 const std::int64_t auth = req.authUserId;
                 std::int64_t id = 0;
                 std::int64_t vid = 0;
                 if (!parseId(req.param("id"), id) || !parseId(req.param("vid"), vid)) {
                   resp.setError(404, "file or version not found");
                   return;
                 }
                 FileRow row;
                 if (!repo.findById(id, row, perr) || !ownerVisible(row, auth)) {
                   resp.setError(404, "file not found");
                   return;
                 }
                 VersionRow vr;
                 if (!ver.findVersion(vid, vr, perr) || vr.fileId != id) {
                   resp.setError(404, "version not found");
                   return;
                 }
                 std::vector<std::string> orphans;
                 if (!ver.deleteVersion(vid, orphans, perr)) {
                   resp.setError(500, std::string("db failed: ") + perr);
                   return;
                 }
                 const std::int64_t freed = dropOrphanBlobs(store, orphans);
                 json::Value v = json::Value::object();
                 v.set("deleted", true);
                 v.set("version_id", static_cast<long long>(vid));
                 v.set("freed_bytes", static_cast<long long>(freed));
                 resp.setJson(200, json::dump(v));
               });

  // ================= 标签（按 owner 隔离；同名 NOCASE 幂等） =================

  // ---- GET /api/v1/tags —— 标签列表（带关联文件数）----
  server.route("GET", "/api/v1/tags", [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    std::vector<TagRow> tags;
    if (!tagRepo.listTags(scopeOwner(req.authUserId), tags, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    json::Value arr = json::Value::array();
    for (const TagRow& t : tags) arr.push_back(tagToJson(t));
    json::Value v = json::Value::object();
    v.set("items", arr);
    v.set("total", static_cast<long long>(tags.size()));
    resp.setJson(200, json::dump(v));
  });

  // ---- POST /api/v1/tags —— 新建标签（body {"name":..,"color":"#RRGGBB"}）----
  // 同名（NOCASE）已存在 → 200 + created=false（幂等，不当错误报）
  server.route("POST", "/api/v1/tags", [&](const net::Request& req, net::Response& resp) {
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
    std::string name = pname->stringValue();
    if (name.size() > 64) {
      resp.setError(400, "tag name too long (max 64 bytes)");
      return;
    }
    // 颜色校验：#RGB 或 #RRGGBB，非法一律回退到默认强调色（不因配色不合法而拒绝建标签）
    std::string color = "#4C6FFF";
    const json::Value* pc = body.find("color");
    if (pc && pc->type() == json::Value::Type::String) {
      const std::string c = pc->stringValue();
      const bool hexOk = (c.size() == 4 || c.size() == 7) && c[0] == '#';
      bool allHex = hexOk;
      for (std::size_t i = 1; allHex && i < c.size(); ++i) {
        const char ch = c[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
          allHex = false;
        }
      }
      if (allHex) color = c;
    }

    TagRow out;
    bool created = false;
    if (!tagRepo.createTag(scopeOwner(req.authUserId), name, color, out, created, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    json::Value v = tagToJson(out);
    v.set("created", created);
    resp.setJson(created ? 201 : 200, json::dump(v));
  });

  // ---- DELETE /api/v1/tags/:id —— 删标签（级联清理文件关联）----
  server.route("DELETE", "/api/v1/tags/:id", [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    std::int64_t id = 0;
    if (!parseId(req.param("id"), id)) {
      resp.setError(404, "tag not found");
      return;
    }
    if (!tagRepo.deleteTag(scopeOwner(req.authUserId), id, perr)) {
      resp.setError(perr == "tag not found" ? 404 : 500, perr);
      return;
    }
    json::Value v = json::Value::object();
    v.set("deleted", true);
    v.set("tag_id", static_cast<long long>(id));
    resp.setJson(200, json::dump(v));
  });

  // ---- GET /api/v1/files/:id/tags —— 某文件的标签 ----
  server.route("GET", "/api/v1/files/:id/tags", [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    const std::int64_t auth = req.authUserId;
    std::int64_t id = 0;
    if (!parseId(req.param("id"), id)) {
      resp.setError(404, "file not found");
      return;
    }
    FileRow row;
    if (!repo.findById(id, row, perr) || !ownerVisible(row, auth)) {
      resp.setError(404, "file not found");
      return;
    }
    std::vector<TagRow> tags;
    if (!tagRepo.tagsOfFile(id, tags, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    json::Value arr = json::Value::array();
    for (const TagRow& t : tags) arr.push_back(tagToJson(t));
    json::Value v = json::Value::object();
    v.set("file_id", static_cast<long long>(id));
    v.set("items", arr);
    v.set("total", static_cast<long long>(tags.size()));
    resp.setJson(200, json::dump(v));
  });

  // ---- PUT /api/v1/files/:id/tags —— 全量设置某文件的标签（body {"tag_ids":[..]}）----
  server.route("PUT", "/api/v1/files/:id/tags", [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    const std::int64_t auth = req.authUserId;
    std::int64_t id = 0;
    if (!parseId(req.param("id"), id)) {
      resp.setError(404, "file not found");
      return;
    }
    FileRow row;
    if (!repo.findById(id, row, perr) || !ownerVisible(row, auth)) {
      resp.setError(404, "file not found");
      return;
    }
    json::Value body = parseJsonBody(req.body, perr);
    if (!perr.empty()) {
      resp.setError(400, "invalid json: " + perr);
      return;
    }
    std::vector<std::int64_t> tagIds;
    const json::Value* ptags = body.find("tag_ids");
    if (ptags && ptags->type() == json::Value::Type::Array) {
      for (const json::Value& tv : ptags->items()) {
        if (tv.type() == json::Value::Type::Number) {
          const std::int64_t tid = static_cast<std::int64_t>(tv.numberValue());
          if (tid > 0) tagIds.push_back(tid);
        }
      }
    }
    // 归属由仓储层兜底：只会关联属于该 owner 的 tag（跨 owner 的 id 静默忽略）
    if (!tagRepo.setFileTags(scopeOwner(auth), id, tagIds, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    std::vector<TagRow> now;
    tagRepo.tagsOfFile(id, now, perr);
    json::Value arr = json::Value::array();
    for (const TagRow& t : now) arr.push_back(tagToJson(t));
    json::Value v = json::Value::object();
    v.set("file_id", static_cast<long long>(id));
    v.set("items", arr);
    v.set("total", static_cast<long long>(now.size()));
    resp.setJson(200, json::dump(v));
  });

  // ---- POST /api/v1/search —— 检索（body {"keyword":..,"tag_ids":[..],"type":-1|0..7}）----
  // 朴素实现：服务端拉取本 owner 的列表（上限 500），在 C++ 侧做关键词 / 标签 / 类型过滤。
  // 数据量大了要换成带索引的 SQL —— 但现在是个人网盘，够用且好懂优先。
  server.route("POST", "/api/v1/search", [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    const std::int64_t oid = scopeOwner(req.authUserId);
    json::Value body = parseJsonBody(req.body, perr);
    perr.clear();  // body 可空 → 等价于"列出全部"

    std::string keyword;
    const json::Value* pk = body.find("keyword");
    if (pk && pk->type() == json::Value::Type::String) keyword = asciiLower(pk->stringValue());

    int typeFilter = -1;
    const json::Value* pt = body.find("type");
    if (pt && pt->type() == json::Value::Type::Number) {
      typeFilter = static_cast<int>(pt->numberValue());
    }

    std::vector<std::int64_t> tagIds;
    const json::Value* ptags = body.find("tag_ids");
    if (ptags && ptags->type() == json::Value::Type::Array) {
      for (const json::Value& tv : ptags->items()) {
        if (tv.type() == json::Value::Type::Number) {
          const std::int64_t tid = static_cast<std::int64_t>(tv.numberValue());
          if (tid > 0) tagIds.push_back(tid);
        }
      }
    }

    // 标签过滤：先拿到"命中任一标签"的文件 id 白名单（空 tag_ids → 不启用该维度）
    std::vector<std::int64_t> tagHit;
    const bool useTagFilter = !tagIds.empty();
    if (useTagFilter && !tagRepo.listFileIdsByTags(oid, tagIds, tagHit, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    std::set<std::int64_t> tagSet(tagHit.begin(), tagHit.end());

    std::vector<FileRow> rows;
    if (!repo.listFiles(oid, rows, perr)) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    json::Value arr = json::Value::array();
    for (const FileRow& f : rows) {
      if (!keyword.empty() && asciiLower(f.name).find(keyword) == std::string::npos) continue;
      if (useTagFilter && tagSet.find(f.id) == tagSet.end()) continue;
      if (typeFilter >= 0 && fileTypeOf(f.name) != typeFilter) continue;
      arr.push_back(fileToJson(f));
    }
    json::Value v = json::Value::object();
    v.set("items", arr);
    v.set("total", static_cast<long long>(arr.items().size()));
    v.set("keyword", keyword);
    resp.setJson(200, json::dump(v));
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

  // ---- POST /api/v1/shares/cleanup （需鉴权）清除调用者名下的无效分享 ----
  // 必须注册在 DELETE /api/v1/shares/:id 之前，否则 "cleanup" 会被 :id 路由吃掉
  // （parseId("cleanup") 失败回 404）。返回 200 {removed: N}。
  // authUserId == -1（legacy/admin）：清全部无效分享；>=1：只清自己名下文件关联的无效分享。
  server.route("POST", "/api/v1/shares/cleanup", [&](const net::Request& req, net::Response& resp) {
    std::string perr;
    std::int64_t now = cv::nowMillis();
    std::int64_t removed = shr.cleanupInvalid(req.authUserId, now, perr);
    if (removed < 0) {
      resp.setError(500, std::string("db failed: ") + perr);
      return;
    }
    cv::json::Value v = cv::json::Value::object();
    v.set("removed", static_cast<long long>(removed));
    resp.setJson(200, cv::json::dump(v));
    CV_LOG_INFO("清除无效分享 authUserId=" << req.authUserId << " removed=" << removed);
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
    if (!shr.revokeById(id, perr)) {
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

  // ---- 启动期孤儿 blob 回收（store/blob_gc.h）----
  // 放在**开始监听之前**：此刻本进程没有在途上传，不存在「blob 已落地、chunk 行未写」
  // 的窗口，删除是安全的。宽限期再兜一层并发实例的风险。
  if (cfg.blobGcEnabled) {
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    cv::blobgc::Stats gst;
    std::string         gerr;
    const std::int64_t  graceMs =
        static_cast<std::int64_t>(cfg.blobGcGraceMinutes) * 60LL * 1000LL;
    const bool ok = cv::blobgc::collectOrphans(
        store,
        [&repo](const std::string& h, bool& out, std::string& e) {
          return repo.chunkExists(h, out, e);
        },
        graceMs, static_cast<std::int64_t>(nowMs), /*dryRun=*/false, gst, gerr);
    if (!ok) {
      CV_LOG_WARN("孤儿 blob 回收失败（不影响启动）: " << gerr);
    } else {
      CV_LOG_INFO("孤儿 blob 回收：扫描 " << gst.scanned << " 个，删除 " << gst.removed
                                        << " 个，释放 " << gst.freedBytes << " 字节（保留：被引用 "
                                        << gst.keptReferenced << "，宽限期内 " << gst.keptFresh
                                        << "，判定失败 " << gst.keptOnError << "）");
    }
  }

  // ---- 内容库副本：--backup-blobs / --restore-blobs（各自跑完即退出，不启动服务）----
  // ⚠️ 与 --scrub 一样，必须在 setDataKey 之后：加密库要先能解密才能校验/复制。
  if (!cfg.backupBlobsDir.empty()) {
    cv::blobbackup::BackupStats bst;
    std::string                berr;
    if (!cv::blobbackup::mirrorBlobs(store, cfg.backupBlobsDir, /*dryRun=*/false, bst, berr)) {
      CV_LOG_ERROR("内容库备份失败（不是「没问题」，是没跑完）: " << berr);
      return 1;
    }
    CV_LOG_INFO("内容库备份完成 → " << cfg.backupBlobsDir << "：扫描 " << bst.scanned
                                    << " 个块，新复制 " << bst.copied << " 个（" << bst.bytesCopied
                                    << " 字节），已存在跳过 " << bst.skippedExisting
                                    << "，源已损坏拒绝复制 " << bst.skippedBad);
    if (bst.skippedBad > 0)
      CV_LOG_ERROR("有 " << bst.skippedBad
                         << " 个块在源侧就已损坏、已拒绝复制 —— 请先 --scrub 核对，别把坏数据也备一份。");
    return bst.skippedBad > 0 ? 1 : 0;
  }
  if (!cfg.restoreBlobsDir.empty()) {
    std::vector<std::string> expected;
    if (!repo.listReferencedChunkHashes(expected, err)) {
      CV_LOG_ERROR("恢复无法取得分块清单: " << err);
      return 1;
    }
    cv::blobbackup::RestoreStats rst;
    std::string                  rerr;
    if (!cv::blobbackup::restoreBlobs(store, cfg.restoreBlobsDir, expected, /*dryRun=*/false, rst,
                                      rerr)) {
      CV_LOG_ERROR("从副本恢复失败: " << rerr);
      return 1;
    }
    CV_LOG_INFO("从副本恢复完成（副本 " << cfg.restoreBlobsDir << "）：检查 " << rst.checked
                                      << " 个块，本来就正常 " << rst.alreadyOk << "，修复 "
                                      << rst.restored << " 个（" << rst.bytesRestored << " 字节）");
    if (rst.missingInReplica > 0 || rst.badReplica > 0) {
      CV_LOG_ERROR("仍有 " << rst.missingInReplica << " 个块副本里没有、" << rst.badReplica
                           << " 个块副本自身也损坏 ⇒ 这些内容无法自动还原，需从客户端重新上传。");
      return 1;
    }
    return 0;
  }

  // ---- 数据校验模式：--scrub 全量巡检一遍然后退出，不启动服务 ----
  // ⚠️ 必须放在 setDataKey 之后：加密库要能解密才能校验（否则会把每个加密块都误报成损坏）。
  if (cfg.scrubNow) {
    std::vector<std::string> expected;
    if (!repo.listReferencedChunkHashes(expected, err)) {
      CV_LOG_ERROR("巡检无法取得分块清单: " << err);
      return 1;
    }
    cv::scrub::Stats sst;
    std::string      serr;
    if (!cv::scrub::scanHashes(store, expected, /*stopOnFirstBad=*/false, sst, serr)) {
      CV_LOG_ERROR("内容库巡检失败（不是「没问题」，是没能跑完）: " << serr);
      return 1;
    }
    CV_LOG_INFO("内容库巡检完成：校验 " << sst.checked << " 个块，读取 " << sst.bytes
                                      << " 字节；正常 " << sst.ok << "，内容损坏 " << sst.corrupt
                                      << "，读取/解密失败 " << sst.readErrors << "，磁盘缺失 "
                                      << sst.missing);
    if (!sst.badHashes.empty()) {
      const std::size_t show = sst.badHashes.size() < 20 ? sst.badHashes.size() : 20;
      for (std::size_t i = 0; i < show; ++i)
        CV_LOG_ERROR("  受损分块 " << sst.badHashes[i]);
      if (sst.badHashes.size() > show)
        CV_LOG_ERROR("  …另有 " << (sst.badHashes.size() - show) << " 个受损分块未列出");
      // 把"哪些块坏了"翻译成"哪些文件打不开了" —— 前者是给程序看的，后者才是给运维/用户看的
      for (std::size_t i = 0; i < show; ++i) {
        std::vector<std::string> files;
        std::string              ferr;
        if (repo.filesUsingChunk(sst.badHashes[i], 5, files, ferr) && !files.empty()) {
          std::string joined;
          for (std::size_t k = 0; k < files.size(); ++k)
            joined += (k ? "、" : "") + files[k];
          CV_LOG_ERROR("    受影响文件：" << joined);
        }
      }
      CV_LOG_ERROR("受损块合计 " << sst.badHashes.size()
                                 << " 个 —— 这些内容已无法还原，需要从客户端重新上传（重传即自愈）。");
      return 1;
    }
    return 0;
  }

  // ---- 后台周期性巡检（--scrub-interval-hours > 0）----
  // 只操作 ContentStore（纯文件 IO；加密上下文按调用新建），**不碰数据库句柄** ——
  // 因此不需要跟服务端的工作线程争 Db，也就不存在 sqlite3 跨线程使用的风险。
  std::atomic<bool> scrubStop{false};
  std::thread       scrubThread;
  if (cfg.scrubIntervalHours > 0) {
    const int hours = cfg.scrubIntervalHours;
    // 后台线程**自建一条数据库连接**：Db 句柄有线程归属，绝不能与服务端工作线程共用。
    // 这正是把"该有哪些块"的查询放在此处、而不是复用主线程 repo 的原因。
    const std::string scrubDbPath = cfg.dbPath();
    scrubThread     = std::thread([&store, &scrubStop, hours, scrubDbPath]() {
      while (!scrubStop.load()) {
        // 分片睡眠：退出时不用等满一整个周期
        for (int i = 0; i < hours * 60 && !scrubStop.load(); ++i)
          std::this_thread::sleep_for(std::chrono::minutes(1));
        if (scrubStop.load())
          break;
        cv::Db sdb;
        std::string e;
        if (!sdb.open(scrubDbPath, e)) {
          CV_LOG_WARN("后台巡检无法打开元数据库: " << e);
          continue;
        }
        cv::FileRepository       srepo(sdb);
        std::vector<std::string> expected;
        if (!srepo.listReferencedChunkHashes(expected, e)) {
          CV_LOG_WARN("后台巡检无法取得分块清单: " << e);
          continue;
        }
        cv::scrub::Stats sst;
        std::string      serr;
        if (!cv::scrub::scanHashes(store, expected, false, sst, serr)) {
          CV_LOG_WARN("后台巡检未能跑完: " << serr);
          continue;
        }
        CV_LOG_INFO("后台巡检：校验 " << sst.checked << " 个块；损坏 " << sst.corrupt
                                     << "，读取失败 " << sst.readErrors << "，缺失 " << sst.missing);
        if (!sst.badHashes.empty())
          CV_LOG_ERROR("后台巡检发现受损分块 " << sst.badHashes.size() << " 个（详见 --scrub 输出）");
      }
    });
    CV_LOG_INFO("后台巡检已启动：每 " << hours << " 小时全量校验一次内容库");
  }

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

  // 正常情况下 runForever 不返回；真返回了就干净收掉后台巡检线程
  scrubStop = true;
  if (scrubThread.joinable()) scrubThread.join();

  CV_LOG_INFO("服务端已停止");
  return 0;
}

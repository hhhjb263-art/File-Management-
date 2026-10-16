#pragma once

#include <cstddef>
#include <string>
#include <cstdint>
#include <vector>

namespace cv {

// 服务端配置。优先级：命令行 > 环境变量 > 配置文件 > 内置默认值。
struct Config {
  std::string dataDir = "/var/lib/cloudvault";  // 数据根目录（元数据/临时/内容库）
  // 文件树允许根（客户端可见的目录树根）。为空则取 <dataDir>/files。
  // 显式配置而非由进程运行目录推导 —— 服务端从任何 cwd 启动，落点都一致。
  std::string filesRoot = "";
  std::string listenAddr = "0.0.0.0";
  int port = 8080;
  int workers = 4;                 // HTTP 工作线程数
  std::size_t chunkSize = 5u * 1024u * 1024u;  // 分块大小，默认 5MB
  std::string logFile;             // 为空则输出到 stdout
  std::string logLevel = "info";
  // TLS（HTTPS）双模式：tlsPort > 0 且证书/私钥齐全时额外开 HTTPS 监听；
  // 均不配置则纯 HTTP。HTTP 与 HTTPS 可同时运行，共享同一套路由。
  int tlsPort = 0;                 // 0 = 关闭 HTTPS
  std::string tlsCert;             // PEM 证书路径
  std::string tlsKey;              // PEM 私钥路径

  // API 认证：非空则启用 Bearer Token 鉴权（除 /healthz 外全接口强制）。
  // 空 = 不启用（向后兼容）。三通道：--auth-token / CV_AUTH_TOKEN / auth_token。
  std::string authToken = "";
  // 明文 HTTP 监听开关：false 即 --http=off（仅保留 HTTPS）。默认 true（开）。
  bool httpEnabled = true;

  // 静态数据加密密钥文件路径：非空即启用 blob 落盘加密（AES-256-GCM）。
  // 空 = 不加密（向后兼容）。三通道：--data-key / CV_DATA_KEY / data_key。
  // 密钥文件支持 64 个十六进制字符（32 字节）或直接 32 字节原始数据。
  std::string dataKey = "";

  // 版本保留策略（PRD FR-4-2）：**条数与天数双限取先到者**。
  //   keepCount  <= 0 → 不限条数；retentionDays <= 0 → 不限天数；两者都 <=0 → 完全不清理。
  // 为什么必须有：不清理 = 每次覆盖都留一份快照 ⇒ 磁盘只涨不降，且用户看不到增长来源。
  // 三通道：--version-keep / CV_VERSION_KEEP / version_keep；--version-days / CV_VERSION_DAYS / version_days。
  int versionKeepCount = 10;
  int versionRetentionDays = 90;

  // 启动期孤儿 blob 回收（见 store/blob_gc.h）。
  // 默认开启：崩溃可能把分块搬进 blobs/ 后死在写 chunk 行之前，那些 blob 无人引用、
  // 也无人回收（chunk_gc 只看数据库，不碰磁盘）⇒ 磁盘只涨不降。
  // graceMin 是宽限期：mtime 在此之内的 blob 一律不删（本项目无单实例锁，
  // 另一个进程可能正在上传）；0 = 不限（危险）。
  // 三通道：--blob-gc / CV_BLOB_GC / blob_gc；--blob-gc-grace-min / CV_BLOB_GC_GRACE_MIN / blob_gc_grace_min。
  bool blobGcEnabled = true;
  int  blobGcGraceMinutes = 60;

  // 数据库备份模式：非空 → 只做一次一致性热备（VACUUM INTO）然后退出，不启动服务。
  // 三通道：--backup-db=<dir> / CV_BACKUP_DB / backup_db。
  std::string backupDbDir = "";

  // 数据校验（PRD 五.4）：主动巡检，发现**已经损坏**的块（bit rot / 坏道 / 被改）。
  //   scrubNow            → 启动时全量巡检一遍然后退出，不启动服务（供 cron 用）
  //   scrubIntervalHours  → >0 时在后台按此周期巡检（0 = 关闭，默认）
  // 为什么默认关闭：一次全量巡检会把整个内容库读一遍，100GB 的库就是 100GB 读盘。
  // 三通道：--scrub / CV_SCRUB / scrub=now；--scrub-interval-hours / CV_SCRUB_INTERVAL_HOURS / scrub_interval_hours。
  bool scrubNow = false;
  int  scrubIntervalHours = 0;

  // 默认用户配额（PRD FR-1-4）。0 = 不限。
  // 某个用户的 users.quota_bytes 为 0 时回落到这里 —— 于是"全员统一限额"和"逐人设置"两种做法都支持。
  // 三通道：--user-quota=<bytes> / CV_USER_QUOTA / user_quota。
  std::int64_t defaultUserQuotaBytes = 0;

  // 内容库副本（对应清单「五.3 多副本 / 定期快照 / 备份到另一台机器」）。
  //   backupBlobsDir  → 把内容库**增量**镜像到该目录后退出（可指向另一块盘 / NAS 挂载点，供 cron 用）
  //   restoreBlobsDir → 以该目录为副本，修复主库中缺失/损坏的块后退出
  // 三通道：--backup-blobs / CV_BACKUP_BLOBS / backup_blobs；--restore-blobs / CV_RESTORE_BLOBS / restore_blobs。
  // ⚠️ 副本目录必须与 dataDir 在**不同物理盘**上才有意义 —— 同一块盘坏了两份一起没。
  std::string backupBlobsDir  = "";
  std::string restoreBlobsDir = "";

  // 恶意文件判定（清单 8.2 / 8.5）。默认**关闭**：这是可选的尽力而为闸门，不是安全边界。
  // 方案按 **hash 查询**（内容不出服务器），适配 VirusTotal v3 公共 API。
  // scanApiKey 为空则等于未启用（不会发任何外网请求）。
  // 三通道：--scan / CV_SCAN、--scan-api-key / CV_SCAN_API_KEY、--scan-block-threshold、--scan-fail-open。
  bool         scanEnabled        = false;
  std::string  scanApiKey         = "";
  int          scanBlockThreshold = 1;     // 报毒引擎数 ≥ 此值即拒绝
  bool         scanFailOpen       = true;  // 远端查询失败时放行（默认）还是拒绝
  // 单次远端查询超时（毫秒）。⚠️ 这是**同步**调用，超时会把一个上传工作线程占住这么久；
  // 服务端有 workers 个工作线程（默认 4），且同一内容只因**首次**上传付出这一次（之后走缓存）。
  int          scanTimeoutMs      = 3000;

  std::string dbPath() const { return dataDir + "/meta/cloudvault.db"; }
  // 账号库是**独立文件**（与文件元数据分离，备份/权限策略互不影响）。
  // 提成访问器是为了让备份代码与主流程引用同一处路径定义，避免两边各写一份而后漂移。
  std::string usersDbPath() const { return dataDir + "/meta/users.db"; }
  std::string blobRoot() const { return dataDir + "/blobs"; }
  std::string tmpRoot() const { return dataDir + "/tmp"; }
  // 文件树允许根（唯一可被客户端创建目录 / 上传 / 下载的物理目录）
  std::string filesRootDir() const {
    return filesRoot.empty() ? dataDir + "/files" : filesRoot;
  }
};

// 解析 argc/argv、环境变量（CV_*）与 --config 指定的 key=value 文件。
Config loadConfig(int argc, char** argv);

// 加载静态加密密钥文件：支持 64 个十六进制字符（32 字节）或 32 字节原始数据。
// 文件不存在 / 长度不对 → 返回 false 并填充 err。成功则将 32 字节密钥写入 out。
bool loadDataKeyFile(const std::string& path, std::vector<unsigned char>& out, std::string& err);

// 命令行帮助文本。
std::string configUsage(const char* program);

}  // namespace cv

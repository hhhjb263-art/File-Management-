#pragma once

namespace cv {

// MVP 建表脚本。所有表都用 IF NOT EXISTS，重复启动安全。
// 说明：
//   chunk        内容寻址分块（去重引用计数）
//   file_node    文件元数据；content_hash 为整文件哈希（=原始字节 SHA-256），用于秒传判定
//   file_chunk   文件 → 分块的有序清单
//   file_version 版本快照（P1 版本回溯启用，结构先建好）
//   upload_session 分块上传会话（断点续传载体，DB 为真相源）
//   upload_chunk   会话已收分块（upload_id + seq 唯一，重复上传幂等覆盖）
inline const char* kSchemaSql = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;

CREATE TABLE IF NOT EXISTS schema_info (
  version    INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS chunk (
  hash       TEXT PRIMARY KEY,
  size       INTEGER NOT NULL,
  ref_count  INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS file_node (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  name         TEXT NOT NULL,
  size         INTEGER NOT NULL,
  content_hash TEXT NOT NULL,
  chunk_count  INTEGER NOT NULL DEFAULT 0,
  created_at   INTEGER NOT NULL,
  deleted      INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_file_hash ON file_node(content_hash);

CREATE TABLE IF NOT EXISTS file_chunk (
  file_id    INTEGER NOT NULL,
  seq        INTEGER NOT NULL,
  chunk_hash TEXT NOT NULL,
  PRIMARY KEY (file_id, seq)
);

CREATE TABLE IF NOT EXISTS file_version (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  file_id      INTEGER NOT NULL,
  version_no   INTEGER NOT NULL,
  content_hash TEXT NOT NULL,
  size         INTEGER NOT NULL,
  created_at   INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_version_file ON file_version(file_id, version_no);

-- 分块上传会话：DB 为真相源。status ∈ created|uploading|completed|aborted。
-- file_hash 为客户端在 init 时提供的整文件 SHA-256（可为空）；空串表示未提供。
CREATE TABLE IF NOT EXISTS upload_session (
  upload_id   INTEGER PRIMARY KEY AUTOINCREMENT,
  name        TEXT NOT NULL,
  size        INTEGER NOT NULL,
  chunk_size  INTEGER NOT NULL,
  file_hash   TEXT NOT NULL DEFAULT '',
  status      TEXT NOT NULL DEFAULT 'created',
  created_at  INTEGER NOT NULL,
  updated_at  INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_upload_hash ON upload_session(file_hash, status);

-- 会话已收分块；临时落盘于 <dataDir>/tmp/uploads/<upload_id>/<seq>.part，complete 时迁入内容库。
-- sha256 为分块内容的 SHA-256；PRIMARY KEY 保证同 seq 重复上传为幂等覆盖。
CREATE TABLE IF NOT EXISTS upload_chunk (
  upload_id  INTEGER NOT NULL,
  seq        INTEGER NOT NULL,
  size       INTEGER NOT NULL,
  sha256     TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  PRIMARY KEY (upload_id, seq)
);

-- 虚拟目录树（规范化相对路径，'/' 分隔；根目录为空串不落行）。
-- 磁盘镜像树位于 <dataDir>/files/<path>；DB 是"哪些目录存在"的真相源。
CREATE TABLE IF NOT EXISTS dir_node (
  path       TEXT PRIMARY KEY,
  created_at INTEGER NOT NULL
);

-- 文件所属目录（与 file_node 1:1）。独立成表避免对存量库做 ALTER 迁移；
-- dir 为空串表示位于根目录。
CREATE TABLE IF NOT EXISTS file_dir (
  file_id INTEGER PRIMARY KEY,
  dir     TEXT NOT NULL DEFAULT ''
);

-- 分块上传会话的目标目录（init 时登记，complete 时落 file_dir）
CREATE TABLE IF NOT EXISTS upload_dir (
  upload_id INTEGER PRIMARY KEY,
  dir       TEXT NOT NULL DEFAULT ''
);

-- 分块上传会话的附加标志：overwrite=1 表示 complete 时覆盖同目录同名文件
CREATE TABLE IF NOT EXISTS upload_flags (
  upload_id INTEGER PRIMARY KEY,
  overwrite INTEGER NOT NULL DEFAULT 0
);

-- 分享链接：公开下载端点 /s/:token 免鉴权，故提取码(code_hash)绝不明文存储。
-- code_hash = sha256(token + ":" + code)；code 为空 ⇒ need_code=false、code_hash=''。
-- expires_at / created_at 均为 Unix 毫秒，0 表示"永久"；max_downloads=0 表示不限次数。
-- downloads 在"开始回内容之前"自增，先判 max_downloads 再累加，避免"用尽还能下最后一发"。
CREATE TABLE IF NOT EXISTS shares (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  token         TEXT UNIQUE NOT NULL,
  file_id       INTEGER NOT NULL,
  code_hash     TEXT NOT NULL DEFAULT '',
  expires_at    INTEGER NOT NULL DEFAULT 0,
  max_downloads INTEGER NOT NULL DEFAULT 0,
  downloads     INTEGER NOT NULL DEFAULT 0,
  created_at    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_shares_token ON shares(token);
)SQL";

}  // namespace cv

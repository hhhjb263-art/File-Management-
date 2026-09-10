#pragma once

namespace cv {

// MVP 建表脚本。所有表都用 IF NOT EXISTS，重复启动安全。
// 说明：
//   chunk        内容寻址分块（去重引用计数）
//   file_node    文件元数据；content_hash 为整文件哈希，用于秒传判定
//   file_chunk   文件 → 分块的有序清单
//   file_version 版本快照（P1 版本回溯启用，结构先建好）
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
)SQL";

}  // namespace cv

#pragma once

namespace cv {

// MVP 建表脚本。所有表都用 IF NOT EXISTS，重复启动安全。
// 说明：
//   chunk        内容寻址分块（去重引用计数）
//   file_node    文件元数据；content_hash 为整文件哈希（=原始字节 SHA-256），用于秒传判定
//                deleted/deleted_at 共同表达回收站状态（见下）
//   file_chunk   文件 → 分块的有序清单
//   file_version 版本快照（元数据）；内容 = file_version_chunk 指向的分块序列
//   file_version_chunk  版本 → 分块的有序清单（与 file_chunk 同构，共享同一批 chunk 行）
//   tag / file_tag      标签与其与文件的多对多关联
//   upload_session 分块上传会话（断点续传载体，DB 为真相源）
//   upload_chunk   会话已收分块（upload_id + seq 唯一，重复上传幂等覆盖）
//
// ── 回收站语义（重要，三处联动）──
//   deleted = 0                    → 正常
//   deleted = 1 且 deleted_at > 0  → 在回收站（可恢复，保留 file_chunk/file_dir/ref_count）
//   deleted = 1 且 deleted_at = 0  → 历史遗留的"硬删除"标记（旧版本写的），一律按不可恢复处理
//   ⚠️ 关键：进回收站**不释放**分块引用，也**不删 blob** —— 磁盘一个字节都没少。
//      但「已用空间」口径不同：sumSizeOfOwner 带 `AND deleted = 0`，所以进站的条目
//      会立刻从 /api/v1/storage 的 used_bytes 里消失（用量数字当场下降）。
//      即：**磁盘占用未释放，但已用空间已扣除** —— 二者在回收站非空时必然不一致。
//      只有彻底删除（purge）才递减 ref_count 并删 blob，此时磁盘与用量才同时变化。
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
  deleted      INTEGER NOT NULL DEFAULT 0,
  owner_id     INTEGER NOT NULL DEFAULT 0,
  deleted_at   INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_file_hash ON file_node(content_hash);
-- 归属过滤索引：按 owner 列出/统计文件时的加速；deleted 维度用于"未删除"快速过滤。
CREATE INDEX IF NOT EXISTS idx_file_owner ON file_node(owner_id, deleted);
-- 回收站查询索引：按 owner + 删除时间取列表 / 判到期。
CREATE INDEX IF NOT EXISTS idx_file_trash ON file_node(owner_id, deleted, deleted_at);

CREATE TABLE IF NOT EXISTS file_chunk (
  file_id    INTEGER NOT NULL,
  seq        INTEGER NOT NULL,
  chunk_hash TEXT NOT NULL,
  PRIMARY KEY (file_id, seq)
);

-- 版本元数据。content_hash 为该版本的整文件 SHA-256（与 file_node.content_hash 同口径）；
-- 实际内容由 file_version_chunk 的分块序列还原（内容寻址库里只有分块 blob，没有整文件 blob）。
-- note 取值：上传新版本 / 恢复前备份 / 覆盖前备份 / 手动
CREATE TABLE IF NOT EXISTS file_version (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  file_id      INTEGER NOT NULL,
  version_no   INTEGER NOT NULL,
  content_hash TEXT NOT NULL,
  size         INTEGER NOT NULL,
  chunk_count  INTEGER NOT NULL DEFAULT 0,
  device       TEXT NOT NULL DEFAULT '',
  note         TEXT NOT NULL DEFAULT '',
  created_at   INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_version_file ON file_version(file_id, version_no);

-- 版本 → 分块清单。与 file_chunk 同构：同一分块可同时被「当前内容」与「若干历史版本」引用，
-- 引用计数（chunk.ref_count）保证只要还有人引用就不会被 GC —— 这是版本"零拷贝"的基础。
CREATE TABLE IF NOT EXISTS file_version_chunk (
  version_id INTEGER NOT NULL,
  seq        INTEGER NOT NULL,
  chunk_hash TEXT NOT NULL,
  PRIMARY KEY (version_id, seq)
);

-- 标签（按 owner 隔离；同名不区分大小写，靠唯一索引兜底）
CREATE TABLE IF NOT EXISTS tag (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  owner_id   INTEGER NOT NULL DEFAULT 0,
  name       TEXT NOT NULL,
  color      TEXT NOT NULL DEFAULT '#4C6FFF',
  created_at INTEGER NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_tag_owner_name ON tag(owner_id, name COLLATE NOCASE);

-- 文件 ↔ 标签 多对多。删除标签或彻底删除文件时必须清理对应行（无外键，靠代码保证）。
CREATE TABLE IF NOT EXISTS file_tag (
  file_id INTEGER NOT NULL,
  tag_id  INTEGER NOT NULL,
  PRIMARY KEY (file_id, tag_id)
);
CREATE INDEX IF NOT EXISTS idx_file_tag_tag ON file_tag(tag_id);

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
  updated_at  INTEGER NOT NULL,
  owner_id    INTEGER NOT NULL DEFAULT 0
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
-- 按 owner 隔离：同一 path 可存在于不同 owner 下，故主键为 (owner_id, path)。
-- 磁盘镜像树位于 <dataDir>/files/<path>（owner_id=0/legacy）或 <dataDir>/files/u<ownerId>/<path>；
-- DB 是"哪些目录存在"的真相源。
-- deleted/deleted_at：目录也进回收站（否则"删掉一个文件夹"在回收站里会散成一堆文件，反直觉）。
CREATE TABLE IF NOT EXISTS dir_node (
  owner_id   INTEGER NOT NULL DEFAULT 0,
  path       TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  deleted    INTEGER NOT NULL DEFAULT 0,
  deleted_at INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (owner_id, path)
);
CREATE INDEX IF NOT EXISTS idx_dir_trash ON dir_node(owner_id, deleted, deleted_at);

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
  created_at    INTEGER NOT NULL,
  revoked       INTEGER NOT NULL DEFAULT 0  -- 1 = 已撤销（软撤销：列表保留、/s/* 视为不存在）
);
CREATE INDEX IF NOT EXISTS idx_shares_token ON shares(token);
)SQL";

}  // namespace cv

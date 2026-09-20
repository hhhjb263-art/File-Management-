#pragma once

namespace cv {

// 用户账号库（users.db）建表脚本。与既有 cloudvault.db 完全分离（独立数据库、独立连接），
// 仅承载"账号 / 会话"两类数据，避免与文件元数据混淆。所有表用 IF NOT EXISTS，重复启动安全。
//
// 时间约定：全部字段统一为 Unix 毫秒（与项目既有约定一致），由 cv::nowMillis() 产生。
//
// 安全要点：
//   · password_hash / password_salt 绝不返回给客户端；token 仅签发瞬间返回明文，库内只存 SHA-256(token)。
//   · username 用 COLLATE NOCASE 唯一，避免 Alice/alice 变成两个账号。
//   · sessions.token_hash 存 SHA-256(token) 的 hex —— 绝不明文存储令牌，库泄露也无法直接冒用。
//   · token_version 用于"改密 / 强制下线"使该用户全部旧会话失效（校验会话时比对当前值）。
inline const char* kUsersSchemaSql = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;

-- 用户账号
--   id            自增主键
--   username      登录名；COLLATE NOCASE 唯一（大小写不敏感，避免重复账号）
--   display_name  展示名（可为空）
--   password_hash PBKDF2-HMAC-SHA256 派生结果（hex，32 字节 → 64 hex）
--   password_salt 每用户独立随机盐（hex，16 字节 → 32 hex）
--   iterations    迭代次数（逐用户存储，便于将来升级算法）
--   created_at    注册时间（Unix 毫秒）
--   last_login_at 上次成功登录时间（毫秒；0 = 从未登录）
--   failed_attempts 连续登录失败计数（成功登录清零）
--   locked_until  锁定到期时间（毫秒；0 = 未锁定；>now 表示锁定中）
--   status        账号状态：active | disabled
--   token_version 令牌版本（改密/强制下线时 +1，使该用户全部旧会话失效）
CREATE TABLE IF NOT EXISTS users (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  username      TEXT NOT NULL COLLATE NOCASE,
  display_name  TEXT NOT NULL DEFAULT '',
  password_hash TEXT NOT NULL,
  password_salt TEXT NOT NULL,
  iterations    INTEGER NOT NULL,
  created_at    INTEGER NOT NULL,
  last_login_at INTEGER NOT NULL DEFAULT 0,
  failed_attempts INTEGER NOT NULL DEFAULT 0,
  locked_until  INTEGER NOT NULL DEFAULT 0,
  status        TEXT NOT NULL DEFAULT 'active',
  token_version INTEGER NOT NULL DEFAULT 1
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_users_username ON users(username COLLATE NOCASE);

-- 会话
--   id          自增主键
--   token_hash  SHA-256(token) 的 hex（绝不明文存令牌）
--   user_id     所属用户
--   created_at  签发时间（毫秒）
--   expires_at  过期时间（毫秒；默认 7 天，滑动续期时更新）
--   last_seen_at 上次使用时间（毫秒；滑动续期时更新）
--   user_agent  客户端 UA（便于用户在会话列表里识别设备）
--   revoked     是否已撤销（1 = 已撤销；撤销/登出后置 1）
--   token_version 签发时的 users.token_version；与会话校验时用户当前值比对，不等即失效
CREATE TABLE IF NOT EXISTS sessions (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  token_hash  TEXT NOT NULL UNIQUE,
  user_id     INTEGER NOT NULL,
  created_at  INTEGER NOT NULL,
  expires_at  INTEGER NOT NULL,
  last_seen_at INTEGER NOT NULL,
  user_agent  TEXT NOT NULL DEFAULT '',
  revoked     INTEGER NOT NULL DEFAULT 0,
  token_version INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);
CREATE INDEX IF NOT EXISTS idx_sessions_exp ON sessions(expires_at);
)SQL";

}  // namespace cv

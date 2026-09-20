#pragma once

// 用户账号与会话的仓储层（独立数据库 users.db）。
//
// 设计要点（见 server/README.md §4.7）：
//   · 与文件元数据库 cloudvault.db 分离，账号数据的备份/权限策略与文件无关。
//   · 密码只存 PBKDF2-HMAC-SHA256 派生结果 + 每用户独立盐 + 迭代次数，**绝不存明文**。
//   · 会话只存 SHA-256(token) 的 hex，**绝不存明文令牌**（库泄露也无法直接冒用）。
//   · token_version 机制：改密/强制下线时 users.token_version+1 ⇒ 该用户全部旧会话立刻失效。
//   · all timestamps are Unix milliseconds（与项目既有约定一致，见 file_repository.h 的 nowMillis()）。

#include <cstdint>
#include <string>
#include <vector>

#include "meta/db.h"

namespace cv {

struct UserRow {
  std::int64_t id = 0;
  std::string  username;
  std::string  displayName;
  std::string  passwordHash;   // hex（PBKDF2 派生，32 字节 → 64 hex）
  std::string  passwordSalt;   // hex（16 字节 → 32 hex）
  std::int64_t iterations = 0;
  std::int64_t createdAt = 0;      // 毫秒
  std::int64_t lastLoginAt = 0;    // 毫秒；0 = 从未登录
  std::int64_t failedAttempts = 0; // 连续失败计数
  std::int64_t lockedUntil = 0;    // 毫秒；0 = 未锁定；>now 表示锁定中
  std::string  status = "active";  // active | disabled
  std::int64_t tokenVersion = 1;
};

struct SessionRow {
  std::int64_t id = 0;
  std::string  tokenHash;
  std::int64_t userId = 0;
  std::int64_t createdAt = 0;
  std::int64_t expiresAt = 0;
  std::int64_t lastSeenAt = 0;
  std::string  userAgent;
  bool         revoked = false;
  std::int64_t tokenVersion = 1;
};

class UserRepository {
 public:
  explicit UserRepository(Db& db) : db_(db) {}

  // ---- 用户 ----
  bool createUser(const std::string& username, const std::string& displayName,
                  const std::string& passwordHash, const std::string& passwordSalt,
                  std::int64_t iterations, UserRow& out, std::string& err);
  // username 走 COLLATE NOCASE（大小写不敏感）。
  bool findByUsername(const std::string& username, UserRow& out, bool& found, std::string& err);
  bool findById(std::int64_t id, UserRow& out, bool& found, std::string& err);
  bool countUsers(std::int64_t& out, std::string& err);
  // 登录成功：置 last_login_at、清零 failed_attempts / locked_until。
  bool recordLoginSuccess(std::int64_t userId, std::int64_t nowMillis, std::string& err);
  // 登录失败：failed_attempts+1；达到阈值则置 locked_until = now + lockMillis（单条 SQL 原子完成）。
  bool recordLoginFailure(std::int64_t userId, int lockThreshold, std::int64_t lockMillis,
                          std::int64_t nowMillis, std::string& err);
  // 改密：写入新 hash/salt/iterations，并把 token_version+1（使所有旧会话失效）。
  bool setPassword(std::int64_t userId, const std::string& hash, const std::string& salt,
                   std::int64_t iterations, std::string& err);

  // ---- 会话 ----
  bool createSession(std::int64_t userId, const std::string& tokenHash, std::int64_t expiresAt,
                     const std::string& userAgent, std::int64_t tokenVersion, SessionRow& out,
                     std::string& err);
  // 滑动续期：更新 last_seen_at 与 expires_at。
  bool touchSession(std::int64_t sessionId, std::int64_t nowMillis, std::int64_t newExpiresAt,
                    std::string& err);
  // 只能撤自己的会话（带 user_id 约束）。
  bool revokeSession(std::int64_t sessionId, std::int64_t userId, std::string& err);
  // 按令牌哈希撤销（登出用：只撤销当前这一条会话）
  bool revokeSessionByTokenHash(const std::string& tokenHash, std::string& err);
  bool revokeAllSessions(std::int64_t userId, std::string& err);
  bool listSessions(std::int64_t userId, std::vector<SessionRow>& out, std::string& err);
  // 清理已过期或已撤销的会话行（启动时调一次即可）。
  bool purgeDeadSessions(std::int64_t nowMillis, std::string& err);

  // 会话校验（**鉴权钩子唯一入口**）：token_hash 有效且未撤销、未过期、
  // 且会话 token_version == 用户当前 token_version、且用户 status=='active'
  // → 返回 true 并写入 outUserId。
  bool validateSession(const std::string& tokenHash, std::int64_t nowMillis,
                       std::int64_t& outUserId, std::string& err);

 private:
  Db& db_;
};

}  // namespace cv

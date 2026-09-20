#include "meta/user_repository.h"

#include <sqlite3.h>

#include "meta/file_repository.h"   // nowMillis()（毫秒，与项目既有约定一致）

namespace cv {

namespace {
// 统一的“绑定失败”错误串（与 share_repository.cpp 风格一致）
const char* kBindFailed = "bind failed";
}  // namespace

// ---------------------------------------------------------------------------
//  用户
// ---------------------------------------------------------------------------
bool UserRepository::createUser(const std::string& username, const std::string& displayName,
                                const std::string& passwordHash, const std::string& passwordSalt,
                                std::int64_t iterations, UserRow& out, std::string& err) {
  Stmt stmt(db_.handle(),
            "INSERT INTO users (username, display_name, password_hash, password_salt, "
            "iterations, created_at, last_login_at, failed_attempts, locked_until, status, "
            "token_version) VALUES (?, ?, ?, ?, ?, ?, 0, 0, 0, 'active', 1)",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, username) || !stmt.bind(2, displayName) || !stmt.bind(3, passwordHash) ||
      !stmt.bind(4, passwordSalt) || !stmt.bind(5, iterations) ||
      !stmt.bind(6, static_cast<std::int64_t>(nowMillis()))) {
    err = kBindFailed;
    return false;
  }
  if (stmt.step(err) != SQLITE_DONE) return false;

  out.id           = db_.lastInsertId();
  out.username     = username;
  out.displayName  = displayName;
  out.passwordHash = passwordHash;
  out.passwordSalt = passwordSalt;
  out.iterations   = iterations;
  out.createdAt    = nowMillis();
  out.status       = "active";
  out.tokenVersion = 1;
  return true;
}

bool UserRepository::findByUsername(const std::string& username, UserRow& out, bool& found,
                                    std::string& err) {
  found = false;
  Stmt stmt(db_.handle(),
            "SELECT id, username, display_name, password_hash, password_salt, iterations, "
            "created_at, last_login_at, failed_attempts, locked_until, status, token_version "
            "FROM users WHERE username = ?",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, username)) {
    err = kBindFailed;
    return false;
  }
  const int rc = stmt.step(err);
  if (rc == SQLITE_DONE) return true;   // 未找到：found=false
  if (rc != SQLITE_ROW) return false;
  out.id             = stmt.int64(0);
  out.username       = stmt.text(1);
  out.displayName    = stmt.text(2);
  out.passwordHash   = stmt.text(3);
  out.passwordSalt   = stmt.text(4);
  out.iterations     = stmt.int64(5);
  out.createdAt      = stmt.int64(6);
  out.lastLoginAt    = stmt.int64(7);
  out.failedAttempts = stmt.int64(8);
  out.lockedUntil    = stmt.int64(9);
  out.status         = stmt.text(10);
  out.tokenVersion   = stmt.int64(11);
  found = true;
  return true;
}

bool UserRepository::findById(std::int64_t id, UserRow& out, bool& found, std::string& err) {
  found = false;
  Stmt stmt(db_.handle(),
            "SELECT id, username, display_name, password_hash, password_salt, iterations, "
            "created_at, last_login_at, failed_attempts, locked_until, status, token_version "
            "FROM users WHERE id = ?",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, id)) {
    err = kBindFailed;
    return false;
  }
  const int rc = stmt.step(err);
  if (rc == SQLITE_DONE) return true;
  if (rc != SQLITE_ROW) return false;
  out.id             = stmt.int64(0);
  out.username       = stmt.text(1);
  out.displayName    = stmt.text(2);
  out.passwordHash   = stmt.text(3);
  out.passwordSalt   = stmt.text(4);
  out.iterations     = stmt.int64(5);
  out.createdAt      = stmt.int64(6);
  out.lastLoginAt    = stmt.int64(7);
  out.failedAttempts = stmt.int64(8);
  out.lockedUntil    = stmt.int64(9);
  out.status         = stmt.text(10);
  out.tokenVersion   = stmt.int64(11);
  found = true;
  return true;
}

bool UserRepository::countUsers(std::int64_t& out, std::string& err) {
  out = 0;
  Stmt stmt(db_.handle(), "SELECT COUNT(*) FROM users", err);
  if (!stmt.ok()) return false;
  const int rc = stmt.step(err);
  if (rc != SQLITE_ROW) return false;
  out = stmt.int64(0);
  return true;
}

bool UserRepository::recordLoginSuccess(std::int64_t userId, std::int64_t nowMillis,
                                       std::string& err) {
  Stmt stmt(db_.handle(),
            "UPDATE users SET last_login_at = ?, failed_attempts = 0, locked_until = 0 "
            "WHERE id = ?",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, nowMillis) || !stmt.bind(2, userId)) {
    err = kBindFailed;
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool UserRepository::recordLoginFailure(std::int64_t userId, int lockThreshold,
                                       std::int64_t lockMillis, std::int64_t nowMillis,
                                       std::string& err) {
  // 单条 SQL 内完成“累加 + 按阈值锁定”，避免"先读后写"的并发竞态。
  Stmt stmt(db_.handle(),
            "UPDATE users SET failed_attempts = failed_attempts + 1, "
            "locked_until = CASE WHEN failed_attempts + 1 >= ? THEN ? ELSE locked_until END "
            "WHERE id = ?",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, static_cast<std::int64_t>(lockThreshold)) ||
      !stmt.bind(2, nowMillis + lockMillis) || !stmt.bind(3, userId)) {
    err = kBindFailed;
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool UserRepository::setPassword(std::int64_t userId, const std::string& hash,
                                 const std::string& salt, std::int64_t iterations,
                                 std::string& err) {
  // token_version + 1 ⇒ 该用户已签发的所有会话立刻失效（改密即强制下线）。
  Stmt stmt(db_.handle(),
            "UPDATE users SET password_hash = ?, password_salt = ?, iterations = ?, "
            "token_version = token_version + 1 WHERE id = ?",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, hash) || !stmt.bind(2, salt) || !stmt.bind(3, iterations) ||
      !stmt.bind(4, userId)) {
    err = kBindFailed;
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

// ---------------------------------------------------------------------------
//  会话
// ---------------------------------------------------------------------------
bool UserRepository::createSession(std::int64_t userId, const std::string& tokenHash,
                                   std::int64_t expiresAt, const std::string& userAgent,
                                   std::int64_t tokenVersion, SessionRow& out,
                                   std::string& err) {
  const std::int64_t now = nowMillis();
  Stmt stmt(db_.handle(),
            "INSERT INTO sessions (token_hash, user_id, created_at, expires_at, last_seen_at, "
            "user_agent, revoked, token_version) VALUES (?, ?, ?, ?, ?, ?, 0, ?)",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, tokenHash) || !stmt.bind(2, userId) || !stmt.bind(3, now) ||
      !stmt.bind(4, expiresAt) || !stmt.bind(5, now) || !stmt.bind(6, userAgent) ||
      !stmt.bind(7, tokenVersion)) {
    err = kBindFailed;
    return false;
  }
  if (stmt.step(err) != SQLITE_DONE) return false;

  out.id           = db_.lastInsertId();
  out.tokenHash    = tokenHash;
  out.userId       = userId;
  out.createdAt    = now;
  out.expiresAt    = expiresAt;
  out.lastSeenAt   = now;
  out.userAgent    = userAgent;
  out.revoked      = false;
  out.tokenVersion = tokenVersion;
  return true;
}

bool UserRepository::touchSession(std::int64_t sessionId, std::int64_t nowMillis,
                                  std::int64_t newExpiresAt, std::string& err) {
  Stmt stmt(db_.handle(),
            "UPDATE sessions SET last_seen_at = ?, expires_at = ? WHERE id = ? AND revoked = 0",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, nowMillis) || !stmt.bind(2, newExpiresAt) || !stmt.bind(3, sessionId)) {
    err = kBindFailed;
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool UserRepository::revokeSession(std::int64_t sessionId, std::int64_t userId,
                                  std::string& err) {
  // 带 user_id 约束：只能撤销自己的会话（防越权撤销他人会话）。
  Stmt stmt(db_.handle(), "UPDATE sessions SET revoked = 1 WHERE id = ? AND user_id = ?", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, sessionId) || !stmt.bind(2, userId)) {
    err = kBindFailed;
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool UserRepository::revokeSessionByTokenHash(const std::string& tokenHash,
                                              std::string& err) {
  Stmt stmt(db_.handle(), "UPDATE sessions SET revoked = 1 WHERE token_hash = ?", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, tokenHash)) {
    err = kBindFailed;
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool UserRepository::revokeAllSessions(std::int64_t userId, std::string& err) {
  Stmt stmt(db_.handle(), "UPDATE sessions SET revoked = 1 WHERE user_id = ?", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, userId)) {
    err = kBindFailed;
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool UserRepository::listSessions(std::int64_t userId, std::vector<SessionRow>& out,
                                 std::string& err) {
  out.clear();
  Stmt stmt(db_.handle(),
            "SELECT id, token_hash, user_id, created_at, expires_at, last_seen_at, user_agent, "
            "revoked, token_version FROM sessions WHERE user_id = ? AND revoked = 0 "
            "ORDER BY created_at DESC",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, userId)) {
    err = kBindFailed;
    return false;
  }
  while (true) {
    const int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    SessionRow s;
    s.id           = stmt.int64(0);
    s.tokenHash    = stmt.text(1);
    s.userId       = stmt.int64(2);
    s.createdAt    = stmt.int64(3);
    s.expiresAt    = stmt.int64(4);
    s.lastSeenAt   = stmt.int64(5);
    s.userAgent    = stmt.text(6);
    s.revoked      = stmt.int64(7) != 0;
    s.tokenVersion = stmt.int64(8);
    out.push_back(s);
  }
  return true;
}

bool UserRepository::purgeDeadSessions(std::int64_t nowMillis, std::string& err) {
  Stmt stmt(db_.handle(), "DELETE FROM sessions WHERE revoked = 1 OR expires_at <= ?", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, nowMillis)) {
    err = kBindFailed;
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool UserRepository::validateSession(const std::string& tokenHash, std::int64_t nowMillis,
                                     std::int64_t& outUserId, std::string& err) {
  outUserId = -1;
  // 单条 SQL 内一次性校验：未撤销 + 未过期 + 会话 token_version == 用户当前值 + 账号 active。
  // token_version 比对放在 SQL 里（而不是取出来再比对），避免"校验与使用之间"的时间窗。
  Stmt stmt(db_.handle(),
            "SELECT s.user_id FROM sessions s JOIN users u ON u.id = s.user_id "
            "WHERE s.token_hash = ? AND s.revoked = 0 AND s.expires_at > ? "
            "AND s.token_version = u.token_version AND u.status = 'active'",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, tokenHash) || !stmt.bind(2, nowMillis)) {
    err = kBindFailed;
    return false;
  }
  const int rc = stmt.step(err);
  if (rc == SQLITE_DONE) return true;   // 无效会话：outUserId 保持 -1
  if (rc != SQLITE_ROW) return false;
  outUserId = stmt.int64(0);
  return true;
}

}  // namespace cv

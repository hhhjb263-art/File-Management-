#include "app/auth_routes.h"

#include <cstdio>
#include <string>
#include <vector>

#include "core/isotime.h"
#include "core/json.h"
#include "core/pbkdf2.h"
#include "core/sha256.h"
#include "meta/file_repository.h"    // nowMillis()
#include "meta/share_repository.h"   // randomHex()
#include "meta/user_repository.h"

namespace cv {
namespace {

bool validUsername(const std::string& u) {
  if (u.size() < 3 || u.size() > 32) return false;
  for (char c : u) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bool validPassword(const std::string& p) { return p.size() >= 8 && p.size() <= 200; }

json::Value parseBody(const std::string& body) {
  json::Value v;
  if (body.empty()) return v;
  std::string perr;
  if (!json::parse(body, v, perr)) return json::Value();
  return v;
}

std::string field(const json::Value& v, const char* key) {
  const json::Value* p = v.find(key);
  return p ? p->stringValue() : std::string();
}

std::string bearerToken(const net::Request& req) {
  const std::string hdr    = req.header("authorization");
  const std::string prefix = "Bearer ";
  if (hdr.size() > prefix.size() && hdr.compare(0, prefix.size(), prefix) == 0)
    return hdr.substr(prefix.size());
  return req.header("x-cv-token");
}

json::Value userJson(const UserRow& u) {
  json::Value v = json::Value::object();
  v.set("id", static_cast<long long>(u.id));
  v.set("username", u.username);
  v.set("display_name", u.displayName);
  v.set("created_at", isotime::fromMillis(u.createdAt));
  v.set("last_login_at", isotime::fromMillis(u.lastLoginAt));
  return v;
}

// 登录失败统一文案：**绝不区分"用户不存在"与"密码错误"**，否则可被用来枚举用户名。
const char* kLoginFailed = "用户名或密码错误";

// 用户不存在时也跑一次等价代价的 PBKDF2，抹平响应时间差异（防基于时间的用户名枚举）。
void dummyKdf(const AuthConfig& cfg) {
  (void)pbkdf2::deriveHex("dummy-password-for-timing", std::string(32, '0'), cfg.iterations, 32);
}

const char* kDbFailed = "db failed: ";

}  // namespace

void registerAuthRoutes(net::HttpServer& server, UserRepository& users, const AuthConfig& cfg) {
  // ---------------------------------------------------------------------
  //  注册
  // ---------------------------------------------------------------------
  server.route("POST", "/api/v1/auth/register",
               [&users, cfg](const net::Request& req, net::Response& resp) {
                 const json::Value body = parseBody(req.body);
                 const std::string username    = field(body, "username");
                 const std::string password    = field(body, "password");
                 const std::string displayName = field(body, "display_name");
                 if (!validUsername(username) || !validPassword(password)) {
                   resp.setError(400, "用户名需 3-32 位（字母/数字/._-），密码至少 8 位");
                   return;
                 }
                 std::string perr;
                 UserRow existing;
                 bool found = false;
                 if (!users.findByUsername(username, existing, found, perr)) {
                   resp.setError(500, std::string(kDbFailed) + perr);
                   return;
                 }
                 if (found) {
                   resp.setError(409, "用户名已存在");
                   return;
                 }
                 const std::string salt = randomHex(static_cast<std::size_t>(cfg.saltBytes));
                 const std::string hash = pbkdf2::deriveHex(password, salt, cfg.iterations, 32);
                 UserRow created;
                 if (!users.createUser(username, displayName, hash, salt, cfg.iterations, created,
                                       perr)) {
                   resp.setError(500, std::string(kDbFailed) + perr);
                   return;
                 }
                 json::Value v = json::Value::object();
                 v.set("id", static_cast<long long>(created.id));
                 v.set("username", created.username);
                 v.set("display_name", created.displayName);
                 resp.setJson(201, json::dump(v));
               });

  // ---------------------------------------------------------------------
  //  登录
  // ---------------------------------------------------------------------
  server.route("POST", "/api/v1/auth/login",
               [&users, cfg](const net::Request& req, net::Response& resp) {
                 const json::Value body = parseBody(req.body);
                 const std::string username = field(body, "username");
                 const std::string password = field(body, "password");
                 const std::string userAgent = field(body, "user_agent");
                 if (username.empty() || password.empty()) {
                   resp.setError(400, "缺少 username 或 password");
                   return;
                 }

                 const std::int64_t now = nowMillis();
                 std::string perr;
                 UserRow u;
                 bool found = false;
                 if (!users.findByUsername(username, u, found, perr)) {
                   resp.setError(500, std::string(kDbFailed) + perr);
                   return;
                 }
                 if (!found) {
                   dummyKdf(cfg);  // 等代价，防时序枚举
                   resp.setError(401, kLoginFailed);
                   return;
                 }
                 if (u.status != "active") {
                   resp.setError(403, "账号已被禁用");
                   return;
                 }
                 if (u.lockedUntil > now) {
                   const std::int64_t remainSec = (u.lockedUntil - now + 999) / 1000;
                   resp.extraHeaders["retry-after"] = std::to_string(remainSec);
                   resp.setError(429, "尝试次数过多，账号已锁定，请稍后再试");
                   return;
                 }

                 const std::string computed =
                     pbkdf2::deriveHex(password, u.passwordSalt,
                                       static_cast<int>(u.iterations), 32);
                 if (!pbkdf2::constantTimeEqualHex(computed, u.passwordHash)) {
                   if (!users.recordLoginFailure(u.id, cfg.lockThreshold, cfg.lockMillis, now,
                                                 perr)) {
                     resp.setError(500, std::string(kDbFailed) + perr);
                     return;
                   }
                   resp.setError(401, kLoginFailed);
                   return;
                 }

                 if (!users.recordLoginSuccess(u.id, now, perr)) {
                   resp.setError(500, std::string(kDbFailed) + perr);
                   return;
                 }
                 // 明文令牌只在此处出现一次；库内只落 sha256(token) 的 hex。
                 const std::string token     = randomHex(32);
                 const std::string tokenHash = Sha256::of(token);
                 SessionRow s;
                 if (!users.createSession(u.id, tokenHash, now + cfg.sessionTtlMillis, userAgent,
                                          u.tokenVersion, s, perr)) {
                   resp.setError(500, std::string(kDbFailed) + perr);
                   return;
                 }
                 // 重新读一次以拿到刚更新的 last_login_at（响应里要给客户端）。
                 UserRow fresh;
                 bool f2 = false;
                 if (!users.findById(u.id, fresh, f2, perr) || !f2) fresh = u;

                 json::Value v = json::Value::object();
                 v.set("token", token);
                 v.set("expires_at", isotime::fromMillis(s.expiresAt));
                 v.set("user", userJson(fresh));
                 resp.setJson(200, json::dump(v));
               });

  // ---------------------------------------------------------------------
  //  登出（撤销当前这一条会话；best-effort，失败也回 200 以免客户端卡住）
  // ---------------------------------------------------------------------
  server.route("POST", "/api/v1/auth/logout",
               [&users](const net::Request& req, net::Response& resp) {
                 const std::string token = bearerToken(req);
                 if (!token.empty()) {
                   std::string perr;
                   if (!users.revokeSessionByTokenHash(Sha256::of(token), perr)) {
                     resp.setError(500, std::string(kDbFailed) + perr);
                     return;
                   }
                 }
                 resp.setJson(200, "{}");
               });

  // ---------------------------------------------------------------------
  //  当前用户
  // ---------------------------------------------------------------------
  server.route("GET", "/api/v1/auth/me",
               [&users](const net::Request& req, net::Response& resp) {
                 if (req.authUserId < 1) {
                   // 未启用账号体系（静态 token / 无鉴权）→ 501：
                   // 客户端据此回退"手动访问令牌"的 legacy 模式，不显示登录门。
                   resp.setError(501, "accounts not enabled");
                   return;
                 }
                 std::string perr;
                 UserRow u;
                 bool found = false;
                 if (!users.findById(req.authUserId, u, found, perr)) {
                   resp.setError(500, std::string(kDbFailed) + perr);
                   return;
                 }
                 if (!found) {
                   resp.setError(401, "会话无效");
                   return;
                 }
                 resp.setJson(200, json::dump(userJson(u)));
               });

  // ---------------------------------------------------------------------
  //  改密（成功后 token_version+1 ⇒ 该用户全部旧会话失效）
  // ---------------------------------------------------------------------
  server.route("POST", "/api/v1/auth/password",
               [&users, cfg](const net::Request& req, net::Response& resp) {
                 if (req.authUserId < 1) {
                   resp.setError(401, "需要登录");
                   return;
                 }
                 const json::Value body = parseBody(req.body);
                 const std::string oldPwd = field(body, "old_password");
                 const std::string newPwd = field(body, "new_password");
                 if (!validPassword(newPwd)) {
                   resp.setError(400, "新密码至少 8 位");
                   return;
                 }
                 std::string perr;
                 UserRow u;
                 bool found = false;
                 if (!users.findById(req.authUserId, u, found, perr)) {
                   resp.setError(500, std::string(kDbFailed) + perr);
                   return;
                 }
                 if (!found) {
                   resp.setError(401, "会话无效");
                   return;
                 }
                 const std::string computed =
                     pbkdf2::deriveHex(oldPwd, u.passwordSalt,
                                       static_cast<int>(u.iterations), 32);
                 if (!pbkdf2::constantTimeEqualHex(computed, u.passwordHash)) {
                   // ⚠️ 刻意用 400 而非 401：客户端把任何 401 当作"会话过期"并自动登出，
                   // 而这里只是"原密码填错"，不该把用户踢出登录态。
                   resp.setError(400, "原密码不正确");
                   return;
                 }
                 const std::string salt = randomHex(static_cast<std::size_t>(cfg.saltBytes));
                 const std::string hash = pbkdf2::deriveHex(newPwd, salt, cfg.iterations, 32);
                 if (!users.setPassword(u.id, hash, salt, cfg.iterations, perr)) {
                   resp.setError(500, std::string(kDbFailed) + perr);
                   return;
                 }
                 resp.setJson(200, "{}");
               });

  // ---------------------------------------------------------------------
  //  会话列表 / 撤销指定会话（都只能操作自己的）
  // ---------------------------------------------------------------------
  server.route("GET", "/api/v1/auth/sessions",
               [&users](const net::Request& req, net::Response& resp) {
                 if (req.authUserId < 1) {
                   resp.setError(401, "需要登录");
                   return;
                 }
                 std::string perr;
                 std::vector<SessionRow> rows;
                 if (!users.listSessions(req.authUserId, rows, perr)) {
                   resp.setError(500, std::string(kDbFailed) + perr);
                   return;
                 }
                 const std::string curHash = Sha256::of(bearerToken(req));
                 json::Value arr = json::Value::array();
                 for (const SessionRow& s : rows) {
                   json::Value v = json::Value::object();
                   v.set("id", static_cast<long long>(s.id));
                   v.set("created_at", isotime::fromMillis(s.createdAt));
                   v.set("last_seen_at", isotime::fromMillis(s.lastSeenAt));
                   v.set("user_agent", s.userAgent);
                   v.set("current", s.tokenHash == curHash);
                   arr.push_back(v);
                 }
                 resp.setJson(200, json::dump(arr));
               });

  server.route("DELETE", "/api/v1/auth/sessions/:id",
               [&users](const net::Request& req, net::Response& resp) {
                 if (req.authUserId < 1) {
                   resp.setError(401, "需要登录");
                   return;
                 }
                 const std::string idStr = req.param("id");
                 std::int64_t sid = 0;
                 try {
                   sid = std::stoll(idStr);
                 } catch (...) {
                   resp.setError(400, "无效的会话 id");
                   return;
                 }
                 std::string perr;
                 // 仓储层带 user_id 约束 ⇒ 无法撤销他人会话。
                 if (!users.revokeSession(sid, req.authUserId, perr)) {
                   resp.setError(500, std::string(kDbFailed) + perr);
                   return;
                 }
                 resp.setJson(200, "{}");
               });
}

}  // namespace cv

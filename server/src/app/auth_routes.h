#pragma once

// 账号相关 HTTP 路由（/api/v1/auth/*）。
//
// 与客户端的契约（两端已对齐，勿单方面改动）：
//   POST /api/v1/auth/register  {username,password,display_name?}
//        → 201 {id, username, display_name}
//   POST /api/v1/auth/login     {username,password,user_agent?}
//        → 200 {token, expires_at, user:{id,username,display_name,created_at,last_login_at}}
//   POST /api/v1/auth/logout    (Bearer 会话)                 → 200 {}
//   GET  /api/v1/auth/me        (Bearer 会话)                 → 200 {id,username,display_name,created_at,last_login_at}
//   POST /api/v1/auth/password  {old_password,new_password}   → 200 {}（改密后旧会话全部失效）
//   GET  /api/v1/auth/sessions  (Bearer 会话)                 → 200 [{id,created_at,last_seen_at,user_agent,current}]
//   DELETE /api/v1/auth/sessions/:id (Bearer 会话，仅自己)     → 200 {}
//
// 时间字段一律 **ISO-8601 UTC 字符串**（客户端按字符串直接展示），
// 而 DB 内一律 Unix 毫秒。
//
// 错误码语义（客户端按 HTTP 状态码生成 [unauthorized]/[forbidden]/[too-many-requests] 等前缀）：
//   400 参数不合规 | 401 登录失败（**统一文案，不区分用户名/密码错，防用户名枚举**）
//   403 账号被禁用 | 409 用户名已存在 | 429 失败过多被锁定（带 Retry-After）
//   501 服务端未启用账号体系（客户端据此回退"手动访问令牌"legacy 模式）

#include <cstdint>

#include "net/http_server.h"

namespace cv {

class UserRepository;

struct AuthConfig {
  std::int64_t sessionTtlMillis = 7LL * 24 * 60 * 60 * 1000;  // 会话有效期：7 天（滑动续期）
  int          lockThreshold    = 5;                          // 连续失败几次触发锁定
  std::int64_t lockMillis       = 5LL * 60 * 1000;            // 锁定时长：5 分钟
  int          iterations       = 210000;                     // PBKDF2 迭代次数
  int          saltBytes        = 16;                         // 每用户随机盐长度
};

// 注册全部账号路由。`users` 的生命周期必须长于 server。
void registerAuthRoutes(net::HttpServer& server, UserRepository& users, const AuthConfig& cfg);

}  // namespace cv

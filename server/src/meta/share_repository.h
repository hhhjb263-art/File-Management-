#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "meta/db.h"

namespace cv {

// 分享链接元数据（与 schema.h 的 shares 表一一对应）
struct Share {
  std::int64_t id = 0;
  std::string token;             // 32 位十六进制随机串（公开端点路径的一部分）
  std::int64_t fileId = 0;       // 关联 file_node.id
  std::string codeHash;          // sha256(token + ":" + code)；空串表示无提取码
  std::int64_t expiresAt = 0;    // Unix 毫秒，0 = 永久
  std::int64_t maxDownloads = 0; // 0 = 不限次数
  std::int64_t downloads = 0;    // 已下载次数
  std::int64_t createdAt = 0;    // 毫秒
};

// 生成 nBytes 字节的随机十六进制串（共 2*nBytes 个 hex 字符）。
// 优先用系统随机源 /dev/urandom；不可用时退回 std::random_device（绝不使用 rand()/时间戳）。
// 分享 token 用 randomHex(16) 得到 32 位十六进制。
std::string randomHex(std::size_t nBytes);

// 提取码哈希：code 为空返回空串（need_code=false）；否则返回 sha256(token + ":" + code)。
// 校验时用恒定时间比较（constantTimeEqual）比对存储值，避免时序侧信道。
std::string shareCodeHash(const std::string& token, const std::string& code);

// 分享链接的元数据读写。所有操作直接作用于 Db 句柄。
class ShareRepository {
 public:
  explicit ShareRepository(Db& db) : db_(db) {}

  // 新建分享。token 由调用方经 randomHex(16) 生成；expireDays=0 表示永久（expires_at=0）。
  bool create(std::int64_t fileId, const std::string& codeHash,
              std::int64_t expireDays, std::int64_t maxDownloads,
              const std::string& token, Share& out, std::string& err);

  // 按 id 查（管理接口用）；found=false 表示不存在。err 仅表示 DB 错误。
  bool findById(std::int64_t id, Share& out, bool& found, std::string& err);
  // 按 token 查（公开端点用）；found=false 表示不存在（已撤销或从未存在）。
  bool findByToken(const std::string& token, Share& out, bool& found, std::string& err);

  // 列出全部分享，最新在前（created_at DESC）。
  bool listAll(std::vector<Share>& out, std::string& err);

  // 撤销（删除）某分享；成功返回 true。err 仅表示 DB 错误（不存在也返回 true）。
  bool removeById(std::int64_t id, std::string& err);

  // 下载计数 +1（在“开始回内容之前”调用）。SQL 端原子条件自增：
  // 仅当 (max_downloads = 0 OR downloads < max_downloads) 才 +1，并发 worker 各发独立 UPDATE，
  // 杜绝突破上限（预检“先判后加”非原子，此处为权威判定）。
  // 0 行受影响 ⇒ 已达上限（用尽），置 *outExhausted=true 且返回 true（绝不当成 DB 错误）；
  // 仅 DB 真错误才返回 false。outExhausted 可空。
  bool incrDownloads(std::int64_t id, std::string& err, bool* outExhausted = nullptr);

 private:
  bool rowToShare(Stmt& stmt, Share& out);

  Db& db_;
};

}  // namespace cv

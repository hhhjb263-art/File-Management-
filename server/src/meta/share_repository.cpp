#include "meta/share_repository.h"

#include <cstdio>
#include <random>

#include "core/sha256.h"
#include "meta/file_repository.h"  // nowMillis()

namespace cv {

// 与 SELECT 语句列顺序保持一致：id, token, file_id, code_hash, expires_at, max_downloads, downloads, created_at
namespace {
const char* kShareCols =
    "id, token, file_id, code_hash, expires_at, max_downloads, downloads, created_at";

constexpr std::int64_t kMillisPerDay = 24LL * 60 * 60 * 1000;
}  // namespace

bool ShareRepository::rowToShare(Stmt& stmt, Share& out) {
  out.id = stmt.int64(0);
  out.token = stmt.text(1);
  out.fileId = stmt.int64(2);
  out.codeHash = stmt.text(3);
  out.expiresAt = stmt.int64(4);
  out.maxDownloads = stmt.int64(5);
  out.downloads = stmt.int64(6);
  out.createdAt = stmt.int64(7);
  return true;
}

std::string randomHex(std::size_t nBytes) {
  std::vector<unsigned char> buf(nBytes);
  bool fromSystem = false;
#ifdef __linux__
  // 系统随机源：/dev/urandom（非阻塞、适合 token 生成）。仅 Linux target 启用。
  std::FILE* f = std::fopen("/dev/urandom", "rb");
  if (f) {
    fromSystem = (std::fread(buf.data(), 1, nBytes, f) == nBytes);
    std::fclose(f);
  }
#endif
  if (!fromSystem) {
    // 退回：随机设备 + 64 位梅森旋转（跨平台，绝不退化为 rand()/时间戳）
    std::random_device rd;
    std::mt19937_64 gen(rd());
    for (std::size_t i = 0; i < nBytes; ++i) {
      buf[i] = static_cast<unsigned char>(gen() & 0xFF);
    }
  }
  static const char* hexd = "0123456789abcdef";
  std::string out;
  out.reserve(nBytes * 2);
  for (unsigned char c : buf) {
    out.push_back(hexd[c >> 4]);
    out.push_back(hexd[c & 0xF]);
  }
  return out;
}

std::string shareCodeHash(const std::string& token, const std::string& code) {
  if (code.empty()) return "";  // 无提取码：need_code=false，code_hash 存空串
  return Sha256::of(token + ":" + code);
}

bool ShareRepository::create(std::int64_t fileId, const std::string& codeHash,
                             std::int64_t expireDays, std::int64_t maxDownloads,
                             const std::string& token, Share& out, std::string& err) {
  std::int64_t now = nowMillis();
  std::int64_t expiresAt = (expireDays > 0) ? (now + expireDays * kMillisPerDay) : 0;
  Stmt stmt(db_.handle(),
            "INSERT INTO shares (token, file_id, code_hash, expires_at, "
            "max_downloads, downloads, created_at) VALUES (?, ?, ?, ?, ?, 0, ?)",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, token) || !stmt.bind(2, fileId) || !stmt.bind(3, codeHash) ||
      !stmt.bind(4, expiresAt) || !stmt.bind(5, maxDownloads) || !stmt.bind(6, now)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_DONE) return false;
  out.id = db_.lastInsertId();
  out.token = token;
  out.fileId = fileId;
  out.codeHash = codeHash;
  out.expiresAt = expiresAt;
  out.maxDownloads = maxDownloads;
  out.downloads = 0;
  out.createdAt = now;
  return true;
}

bool ShareRepository::findById(std::int64_t id, Share& out, bool& found, std::string& err) {
  found = false;
  Stmt stmt(db_.handle(),
            std::string("SELECT ") + kShareCols + " FROM shares WHERE id = ? LIMIT 1", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, id)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return true;  // 无记录：found=false
  found = true;
  return rowToShare(stmt, out);
}

bool ShareRepository::findByToken(const std::string& token, Share& out, bool& found,
                                  std::string& err) {
  found = false;
  Stmt stmt(db_.handle(),
            std::string("SELECT ") + kShareCols + " FROM shares WHERE token = ? LIMIT 1", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, token)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return true;  // 无记录：found=false
  found = true;
  return rowToShare(stmt, out);
}

bool ShareRepository::listAll(std::vector<Share>& out, std::string& err) {
  out.clear();
    // created_at 是毫秒，快速连建多个分享会并列；用 id DESC 兜底，
    // 保证「最新在前」在并列时也是确定顺序（否则列表顺序随机、界面会跳）。
    Stmt stmt(db_.handle(),
              std::string("SELECT ") + kShareCols +
                  " FROM shares ORDER BY created_at DESC, id DESC",
              err);
  if (!stmt.ok()) return false;
  while (true) {
    int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    Share s;
    if (!rowToShare(stmt, s)) return false;
    out.push_back(s);
  }
  return true;
}

bool ShareRepository::removeById(std::int64_t id, std::string& err) {
  Stmt stmt(db_.handle(), "DELETE FROM shares WHERE id = ?", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, id)) {
    err = "bind failed";
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool ShareRepository::incrDownloads(std::int64_t id, std::string& err, bool* outExhausted) {
  if (outExhausted) *outExhausted = false;
  // 原子条件自增：SQL 端一次性判定 (max_downloads = 0 OR downloads < max_downloads)，
  // 并发 worker 各发独立 UPDATE，只有仍满足条件的行才 +1，绝不越过上限。
  Stmt stmt(db_.handle(),
            "UPDATE shares SET downloads = downloads + 1 "
            "WHERE id = ? AND (max_downloads = 0 OR downloads < max_downloads)",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, id)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_DONE) return false;  // 真 DB 错误才返回 false
  // sqlite3_changes 取最近一条语句的实际改动行数；0 行 = 已达上限（用尽），而非错误。
  if (outExhausted && sqlite3_changes(db_.handle()) == 0) *outExhausted = true;
  return true;
}

}  // namespace cv

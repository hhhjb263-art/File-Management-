#include "meta/upload_repository.h"

#include <chrono>

#include "meta/file_repository.h"  // nowMillis() 声明

namespace cv {

// 与 SELECT 语句列顺序保持一致
namespace {
const char* kSessionCols =
    "upload_id, name, size, chunk_size, file_hash, status, created_at, updated_at";
}  // namespace

bool UploadRepository::rowToSession(Stmt& stmt, UploadSession& out) {
  out.id = stmt.int64(0);
  out.name = stmt.text(1);
  out.size = stmt.int64(2);
  out.chunkSize = stmt.int64(3);
  out.fileHash = stmt.text(4);
  out.status = stmt.text(5);
  out.createdAt = stmt.int64(6);
  out.updatedAt = stmt.int64(7);
  return true;
}

bool UploadRepository::create(const std::string& name, std::int64_t size,
                              std::int64_t chunkSize, const std::string& fileHash,
                              std::int64_t& id, std::string& err) {
  std::int64_t now = nowMillis();
  Stmt stmt(db_.handle(),
            "INSERT INTO upload_session (name, size, chunk_size, file_hash, status, "
            "created_at, updated_at) VALUES (?, ?, ?, ?, 'created', ?, ?)",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, name) || !stmt.bind(2, size) || !stmt.bind(3, chunkSize) ||
      !stmt.bind(4, fileHash) || !stmt.bind(5, now) || !stmt.bind(6, now)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_DONE) return false;
  id = db_.lastInsertId();
  return true;
}

bool UploadRepository::findById(std::int64_t id, UploadSession& out, std::string& err) {
  Stmt stmt(db_.handle(),
            std::string("SELECT ") + kSessionCols +
                " FROM upload_session WHERE upload_id = ? LIMIT 1",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, id)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return false;
  return rowToSession(stmt, out);
}

bool UploadRepository::findChunk(std::int64_t id, std::int64_t seq, UploadChunkRow& out,
                                bool& found, std::string& err) {
  found = false;
  Stmt stmt(db_.handle(),
            "SELECT seq, size, sha256, created_at FROM upload_chunk "
            "WHERE upload_id = ? AND seq = ? LIMIT 1",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, id) || !stmt.bind(2, seq)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return true;  // 无记录：found=false
  found = true;
  out.seq = stmt.int64(0);
  out.size = stmt.int64(1);
  out.sha256 = stmt.text(2);
  out.createdAt = stmt.int64(3);
  return true;
}

bool UploadRepository::findResumable(const std::string& fileHash, std::int64_t size,
                                     std::int64_t chunkSize, UploadSession& out,
                                     std::string& err) {
  Stmt stmt(db_.handle(),
            std::string("SELECT ") + kSessionCols +
                " FROM upload_session WHERE file_hash = ? AND size = ? AND "
                "chunk_size = ? AND status NOT IN ('completed','aborted') "
                "ORDER BY upload_id DESC LIMIT 1",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, fileHash) || !stmt.bind(2, size) || !stmt.bind(3, chunkSize)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return false;
  return rowToSession(stmt, out);
}

bool UploadRepository::listChunks(std::int64_t id, std::vector<UploadChunkRow>& out,
                                 std::string& err) {
  out.clear();
  Stmt stmt(db_.handle(),
            "SELECT seq, size, sha256, created_at FROM upload_chunk "
            "WHERE upload_id = ? ORDER BY seq ASC",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, id)) {
    err = "bind failed";
    return false;
  }
  while (true) {
    int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    UploadChunkRow row;
    row.seq = stmt.int64(0);
    row.size = stmt.int64(1);
    row.sha256 = stmt.text(2);
    row.createdAt = stmt.int64(3);
    out.push_back(row);
  }
  return true;
}

bool UploadRepository::putChunk(std::int64_t id, std::int64_t seq, std::int64_t size,
                                const std::string& sha256, std::string& err) {
  // ON CONFLICT 覆盖：同 seq 重复上传（内容可能不同）幂等更新，不产生重复记录
  Stmt stmt(db_.handle(),
            "INSERT INTO upload_chunk (upload_id, seq, size, sha256, created_at) "
            "VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(upload_id, seq) DO UPDATE SET size = excluded.size, "
            "sha256 = excluded.sha256, created_at = excluded.created_at",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, id) || !stmt.bind(2, seq) || !stmt.bind(3, size) ||
      !stmt.bind(4, sha256) || !stmt.bind(5, nowMillis())) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_DONE) return false;
  // 刷新会话 updated_at：每次分块落盘都算活跃，避免长会话在重启后被启动期 GC 误删
  Stmt touch(db_.handle(),
             "UPDATE upload_session SET updated_at = ? WHERE upload_id = ?", err);
  if (!touch.ok()) return false;
  if (!touch.bind(1, nowMillis()) || !touch.bind(2, id)) {
    err = "bind failed";
    return false;
  }
  return touch.step(err) == SQLITE_DONE;
}

bool UploadRepository::deleteChunks(std::int64_t id, std::string& err) {
  Stmt stmt(db_.handle(), "DELETE FROM upload_chunk WHERE upload_id = ?", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, id)) {
    err = "bind failed";
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool UploadRepository::deleteChunk(std::int64_t id, std::int64_t seq, std::string& err) {
  Stmt stmt(db_.handle(),
            "DELETE FROM upload_chunk WHERE upload_id = ? AND seq = ?", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, id) || !stmt.bind(2, seq)) {
    err = "bind failed";
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool UploadRepository::setStatus(std::int64_t id, const std::string& status,
                                std::string& err) {
  Stmt stmt(db_.handle(),
            "UPDATE upload_session SET status = ?, updated_at = ? WHERE upload_id = ?",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, status) || !stmt.bind(2, nowMillis()) || !stmt.bind(3, id)) {
    err = "bind failed";
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool UploadRepository::removeSession(std::int64_t id, std::string& err) {
  if (!deleteChunks(id, err)) return false;
  Stmt stmt(db_.handle(), "DELETE FROM upload_session WHERE upload_id = ?", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, id)) {
    err = "bind failed";
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool UploadRepository::listStaleIds(std::int64_t thresholdMs,
                                    std::vector<std::int64_t>& out, std::string& err) {
  out.clear();
  Stmt stmt(db_.handle(),
            "SELECT upload_id FROM upload_session WHERE status IN ('created','uploading') "
            "AND updated_at < ?",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, thresholdMs)) {
    err = "bind failed";
    return false;
  }
  while (true) {
    int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    out.push_back(stmt.int64(0));
  }
  return true;
}

}  // namespace cv

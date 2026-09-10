#include "meta/file_repository.h"

#include <chrono>

namespace cv {

std::int64_t nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

namespace {

const char* kSelectFile =
    "SELECT id, name, size, content_hash, chunk_count, created_at "
    "FROM file_node WHERE deleted = 0";

}  // namespace

bool FileRepository::findByContentHash(const std::string& hash, FileRow& out,
                                       std::string& err) {
  std::string sql = std::string(kSelectFile) + " AND content_hash = ? LIMIT 1";
  Stmt stmt(db_.handle(), sql, err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, hash)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return false;
  out.id = stmt.int64(0);
  out.name = stmt.text(1);
  out.size = stmt.int64(2);
  out.contentHash = stmt.text(3);
  out.chunkCount = static_cast<int>(stmt.int64(4));
  out.createdAt = stmt.int64(5);
  return true;
}

bool FileRepository::findById(std::int64_t id, FileRow& out, std::string& err) {
  std::string sql = std::string(kSelectFile) + " AND id = ? LIMIT 1";
  Stmt stmt(db_.handle(), sql, err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, id)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return false;
  out.id = stmt.int64(0);
  out.name = stmt.text(1);
  out.size = stmt.int64(2);
  out.contentHash = stmt.text(3);
  out.chunkCount = static_cast<int>(stmt.int64(4));
  out.createdAt = stmt.int64(5);
  return true;
}

bool FileRepository::listFiles(std::vector<FileRow>& out, std::string& err) {
  std::string sql = std::string(kSelectFile) + " ORDER BY id DESC LIMIT 500";
  Stmt stmt(db_.handle(), sql, err);
  if (!stmt.ok()) return false;
  while (true) {
    int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    FileRow row;
    row.id = stmt.int64(0);
    row.name = stmt.text(1);
    row.size = stmt.int64(2);
    row.contentHash = stmt.text(3);
    row.chunkCount = static_cast<int>(stmt.int64(4));
    row.createdAt = stmt.int64(5);
    out.push_back(row);
  }
  return true;
}

bool FileRepository::chunkHashesOf(std::int64_t fileId, std::vector<std::string>& out,
                                   std::string& err) {
  out.clear();
  Stmt stmt(db_.handle(),
            "SELECT chunk_hash FROM file_chunk WHERE file_id = ? ORDER BY seq", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, fileId)) {
    err = "bind failed";
    return false;
  }
  while (true) {
    int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    out.push_back(stmt.text(0));
  }
  return true;
}

bool FileRepository::addChunkIfAbsent(const std::string& hash, std::int64_t size,
                                      std::string& err) {
  {
    Stmt upd(db_.handle(), "UPDATE chunk SET ref_count = ref_count + 1 WHERE hash = ?",
             err);
    if (!upd.ok()) return false;
    if (!upd.bind(1, hash)) {
      err = "bind failed";
      return false;
    }
    int rc = upd.step(err);
    if (rc != SQLITE_DONE) return false;
    if (sqlite3_changes(db_.handle()) > 0) return true;
  }
  Stmt ins(db_.handle(),
           "INSERT INTO chunk (hash, size, ref_count, created_at) VALUES (?, ?, 1, ?)",
           err);
  if (!ins.ok()) return false;
  if (!ins.bind(1, hash) || !ins.bind(2, size) || !ins.bind(3, nowMillis())) {
    err = "bind failed";
    return false;
  }
  return ins.step(err) == SQLITE_DONE;
}

bool FileRepository::insertFile(const std::string& name, std::int64_t size,
                                const std::string& contentHash,
                                const std::vector<std::string>& chunkHashes,
                                const std::vector<std::size_t>& chunkSizes,
                                std::int64_t& id, std::string& err) {
  if (!db_.exec("BEGIN IMMEDIATE", err)) return false;

  std::int64_t newId = 0;
  {
    Stmt ins(db_.handle(),
             "INSERT INTO file_node (name, size, content_hash, chunk_count, created_at, "
             "deleted) VALUES (?, ?, ?, ?, ?, 0)",
             err);
    if (!ins.ok()) {
      db_.exec("ROLLBACK", err);
      return false;
    }
    if (!ins.bind(1, name) || !ins.bind(2, size) || !ins.bind(3, contentHash) ||
        !ins.bind(4, static_cast<std::int64_t>(chunkHashes.size())) ||
        !ins.bind(5, nowMillis())) {
      err = "bind failed";
      db_.exec("ROLLBACK", err);
      return false;
    }
    if (ins.step(err) != SQLITE_DONE) {
      db_.exec("ROLLBACK", err);
      return false;
    }
    newId = db_.lastInsertId();
  }

  for (std::size_t i = 0; i < chunkHashes.size(); ++i) {
    std::int64_t chunkSize =
        i < chunkSizes.size() ? static_cast<std::int64_t>(chunkSizes[i]) : 0;
    if (!addChunkIfAbsent(chunkHashes[i], chunkSize, err)) {
      db_.exec("ROLLBACK", err);
      return false;
    }
    Stmt link(db_.handle(),
              "INSERT INTO file_chunk (file_id, seq, chunk_hash) VALUES (?, ?, ?)", err);
    if (!link.ok()) {
      db_.exec("ROLLBACK", err);
      return false;
    }
    if (!link.bind(1, newId) || !link.bind(2, static_cast<std::int64_t>(i)) ||
        !link.bind(3, chunkHashes[i])) {
      err = "bind failed";
      db_.exec("ROLLBACK", err);
      return false;
    }
    if (link.step(err) != SQLITE_DONE) {
      db_.exec("ROLLBACK", err);
      return false;
    }
  }

  if (!db_.exec("COMMIT", err)) {
    db_.exec("ROLLBACK", err);
    return false;
  }
  id = newId;
  return true;
}

}  // namespace cv

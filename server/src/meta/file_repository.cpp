#include "meta/file_repository.h"

#include <chrono>
#include <set>
#include <string>

namespace cv {

std::int64_t nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

namespace {

const char* kSelectFile =
    "SELECT f.id, f.name, f.size, f.content_hash, f.chunk_count, f.created_at, "
    "COALESCE(d.dir, '') FROM file_node f "
    "LEFT JOIN file_dir d ON d.file_id = f.id WHERE f.deleted = 0";

void readFileRow(Stmt& stmt, FileRow& out) {
  out.id = stmt.int64(0);
  out.name = stmt.text(1);
  out.size = stmt.int64(2);
  out.contentHash = stmt.text(3);
  out.chunkCount = static_cast<int>(stmt.int64(4));
  out.createdAt = stmt.int64(5);
  out.dir = stmt.text(6);
}

}  // namespace

bool FileRepository::findByContentHash(const std::string& hash, FileRow& out,
                                       std::string& err) {
  std::string sql = std::string(kSelectFile) + " AND f.content_hash = ? LIMIT 1";
  Stmt stmt(db_.handle(), sql, err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, hash)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return false;
  readFileRow(stmt, out);
  return true;
}

bool FileRepository::findById(std::int64_t id, FileRow& out, std::string& err) {
  std::string sql = std::string(kSelectFile) + " AND f.id = ? LIMIT 1";
  Stmt stmt(db_.handle(), sql, err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, id)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return false;
  readFileRow(stmt, out);
  return true;
}

bool FileRepository::findByPath(const std::string& dir, const std::string& name,
                                FileRow& out, std::string& err) {
  std::string sql =
      std::string(kSelectFile) + " AND COALESCE(d.dir, '') = ? AND f.name = ? LIMIT 1";
  Stmt stmt(db_.handle(), sql, err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, dir) || !stmt.bind(2, name)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return false;
  readFileRow(stmt, out);
  return true;
}

bool FileRepository::listFiles(std::vector<FileRow>& out, std::string& err) {
  std::string sql = std::string(kSelectFile) + " ORDER BY f.id DESC LIMIT 500";
  Stmt stmt(db_.handle(), sql, err);
  if (!stmt.ok()) return false;
  while (true) {
    int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    FileRow row;
    readFileRow(stmt, row);
    out.push_back(row);
  }
  return true;
}

bool FileRepository::createDir(const std::string& path, bool& created, std::string& err) {
  created = false;
  Stmt ins(db_.handle(),
           "INSERT INTO dir_node (path, created_at) VALUES (?, ?) "
           "ON CONFLICT(path) DO NOTHING",
           err);
  if (!ins.ok()) return false;
  if (!ins.bind(1, path) || !ins.bind(2, nowMillis())) {
    err = "bind failed";
    return false;
  }
  if (ins.step(err) != SQLITE_DONE) return false;
  created = sqlite3_changes(db_.handle()) > 0;
  return true;
}

bool FileRepository::listDirs(std::vector<std::string>& out, std::string& err) {
  out.clear();
  Stmt stmt(db_.handle(), "SELECT path FROM dir_node ORDER BY path", err);
  if (!stmt.ok()) return false;
  while (true) {
    int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    out.push_back(stmt.text(0));
  }
  return true;
}

bool FileRepository::listDirsAll(std::vector<std::string>& out, std::string& err) {
  std::set<std::string> dirs;  // 自动去重 + 排序
  // 1) 已显式登记的目录（dir_node）
  {
    std::vector<std::string> registered;
    if (!listDirs(registered, err)) return false;
    for (const std::string& d : registered) dirs.insert(d);
  }
  // 2) 文件所属目录及其全部祖先（有些目录仅通过文件登记、未走 createDir）
  {
    Stmt stmt(db_.handle(), "SELECT DISTINCT dir FROM file_dir", err);
    if (!stmt.ok()) return false;
    while (true) {
      int rc = stmt.step(err);
      if (rc == SQLITE_DONE) break;
      if (rc != SQLITE_ROW) return false;
      std::string cur = stmt.text(0);
      // 把 cur 及其每一级父目录（直到根 ''）都登记进集合
      while (true) {
        dirs.insert(cur);
        if (cur.empty()) break;
        std::size_t slash = cur.find_last_of('/');
        cur = (slash == std::string::npos) ? std::string() : cur.substr(0, slash);
      }
    }
  }
  out.assign(dirs.begin(), dirs.end());
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

bool FileRepository::nameExists(const std::string& dir, const std::string& name,
                                std::int64_t excludeId, bool& exists, std::string& err) {
  exists = false;
  Stmt stmt(db_.handle(),
            "SELECT COUNT(*) FROM file_node f LEFT JOIN file_dir d ON d.file_id = f.id "
            "WHERE f.deleted = 0 AND COALESCE(d.dir, '') = ? AND f.name = ? AND f.id != ?",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, dir) || !stmt.bind(2, name) || !stmt.bind(3, excludeId)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return false;
  exists = stmt.int64(0) > 0;
  return true;
}

bool FileRepository::renameFile(std::int64_t id, const std::string& newName,
                                std::string& err) {
  Stmt stmt(db_.handle(), "UPDATE file_node SET name = ? WHERE id = ?", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, newName) || !stmt.bind(2, id)) {
    err = "bind failed";
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool FileRepository::softDelete(std::int64_t id, std::string& err) {
  if (!db_.exec("BEGIN IMMEDIATE", err)) return false;
  {
    // 先递减引用计数（blob 保留，交由后续 GC 回收）
    Stmt dec(db_.handle(),
             "UPDATE chunk SET ref_count = ref_count - 1 WHERE hash IN "
             "(SELECT chunk_hash FROM file_chunk WHERE file_id = ?)",
             err);
    if (!dec.ok() || !dec.bind(1, id) || dec.step(err) != SQLITE_DONE) {
      err = err.empty() ? "chunk ref decrement failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
    Stmt stmt(db_.handle(), "UPDATE file_node SET deleted = 1 WHERE id = ?", err);
    if (!stmt.ok() || !stmt.bind(1, id) || stmt.step(err) != SQLITE_DONE) {
      err = err.empty() ? "mark deleted failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
  }
  if (!db_.exec("COMMIT", err)) {
    db_.exec("ROLLBACK", err);
    return false;
  }
  return true;
}

bool FileRepository::replaceContent(std::int64_t id, std::int64_t size,
                                    const std::string& contentHash,
                                    const std::vector<std::string>& chunkHashes,
                                    const std::vector<std::size_t>& chunkSizes,
                                    std::string& err) {
  if (!db_.exec("BEGIN IMMEDIATE", err)) return false;
  {
    Stmt dec(db_.handle(),
             "UPDATE chunk SET ref_count = ref_count - 1 WHERE hash IN "
             "(SELECT chunk_hash FROM file_chunk WHERE file_id = ?)",
             err);
    if (!dec.ok() || !dec.bind(1, id) || dec.step(err) != SQLITE_DONE) {
      err = err.empty() ? "chunk ref decrement failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
    Stmt del(db_.handle(), "DELETE FROM file_chunk WHERE file_id = ?", err);
    if (!del.ok() || !del.bind(1, id) || del.step(err) != SQLITE_DONE) {
      err = err.empty() ? "clear file_chunk failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
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
    if (!link.ok() || !link.bind(1, id) || !link.bind(2, static_cast<std::int64_t>(i)) ||
        !link.bind(3, chunkHashes[i]) || link.step(err) != SQLITE_DONE) {
      err = err.empty() ? "link chunk failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
  }
  {
    Stmt upd(db_.handle(),
             "UPDATE file_node SET content_hash = ?, size = ?, chunk_count = ? WHERE id = ?",
             err);
    if (!upd.ok() || !upd.bind(1, contentHash) || !upd.bind(2, size) ||
        !upd.bind(3, static_cast<std::int64_t>(chunkHashes.size())) ||
        !upd.bind(4, id) || upd.step(err) != SQLITE_DONE) {
      err = err.empty() ? "update file_node failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
  }
  if (!db_.exec("COMMIT", err)) {
    db_.exec("ROLLBACK", err);
    return false;
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

bool FileRepository::insertFile(const std::string& name, const std::string& dir,
                                std::int64_t size, const std::string& contentHash,
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

  {
    // 所属目录（dir 已由调用方 sanitizeRelPath 清洗；'' = 根目录）
    Stmt d(db_.handle(), "INSERT INTO file_dir (file_id, dir) VALUES (?, ?)", err);
    if (!d.ok() || !d.bind(1, newId) || !d.bind(2, dir)) {
      err = err.empty() ? "bind failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
    if (d.step(err) != SQLITE_DONE) {
      db_.exec("ROLLBACK", err);
      return false;
    }
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

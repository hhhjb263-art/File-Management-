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
    "COALESCE(d.dir, ''), f.owner_id FROM file_node f "
    "LEFT JOIN file_dir d ON d.file_id = f.id WHERE f.deleted = 0";

void readFileRow(Stmt& stmt, FileRow& out) {
  out.id = stmt.int64(0);
  out.name = stmt.text(1);
  out.size = stmt.int64(2);
  out.contentHash = stmt.text(3);
  out.chunkCount = static_cast<int>(stmt.int64(4));
  out.createdAt = stmt.int64(5);
  out.dir = stmt.text(6);
  out.ownerId = stmt.int64(7);
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

bool FileRepository::findByPath(std::int64_t ownerId, const std::string& dir,
                                const std::string& name, FileRow& out, std::string& err) {
  std::string sql =
      std::string(kSelectFile) +
      " AND COALESCE(d.dir, '') = ? AND f.name = ? AND f.owner_id = ? LIMIT 1";
  Stmt stmt(db_.handle(), sql, err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, dir) || !stmt.bind(2, name) || !stmt.bind(3, ownerId)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return false;
  readFileRow(stmt, out);
  return true;
}

bool FileRepository::listFiles(std::int64_t ownerId, std::vector<FileRow>& out,
                              std::string& err) {
  std::string sql = std::string(kSelectFile) + " AND f.owner_id = ? ORDER BY f.id DESC LIMIT 500";
  Stmt stmt(db_.handle(), sql, err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, ownerId)) {
    err = "bind failed";
    return false;
  }
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

bool FileRepository::chunkSizesOf(const std::vector<std::string>& hashes,
                                 std::vector<std::size_t>& out, std::string& err) {
  out.clear();
  out.reserve(hashes.size());
  for (const std::string& h : hashes) {
    Stmt stmt(db_.handle(), "SELECT size FROM chunk WHERE hash = ? LIMIT 1", err);
    if (!stmt.ok()) return false;
    if (!stmt.bind(1, h)) {
      err = "bind failed";
      return false;
    }
    std::size_t sz = 0;
    if (stmt.step(err) == SQLITE_ROW) sz = static_cast<std::size_t>(stmt.int64(0));
    out.push_back(sz);
  }
  return true;
}

bool FileRepository::createDir(std::int64_t ownerId, const std::string& path, bool& created,
                              std::string& err) {
  created = false;
  Stmt ins(db_.handle(),
           "INSERT INTO dir_node (owner_id, path, created_at) VALUES (?, ?, ?) "
           "ON CONFLICT(owner_id, path) DO NOTHING",
           err);
  if (!ins.ok()) return false;
  if (!ins.bind(1, ownerId) || !ins.bind(2, path) || !ins.bind(3, nowMillis())) {
    err = "bind failed";
    return false;
  }
  if (ins.step(err) != SQLITE_DONE) return false;
  created = sqlite3_changes(db_.handle()) > 0;
  return true;
}

bool FileRepository::listDirs(std::int64_t ownerId, std::vector<std::string>& out,
                             std::string& err) {
  out.clear();
  Stmt stmt(db_.handle(),
            "SELECT path FROM dir_node WHERE owner_id = ? ORDER BY path", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, ownerId)) {
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

bool FileRepository::sumSizeOfOwner(std::int64_t ownerId, std::int64_t& total,
                                   std::string& err) {
  total = 0;
  Stmt stmt(db_.handle(),
            "SELECT COALESCE(SUM(size), 0) FROM file_node WHERE owner_id = ? AND deleted = 0",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, ownerId)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return false;
  total = stmt.int64(0);
  return true;
}

bool FileRepository::listDirsAll(std::int64_t ownerId, std::vector<std::string>& out,
                                 std::string& err) {
  std::set<std::string> dirs;  // 自动去重 + 排序
  // 1) 已显式登记的目录（dir_node，按 owner 隔离）
  {
    std::vector<std::string> registered;
    if (!listDirs(ownerId, registered, err)) return false;
    for (const std::string& d : registered) dirs.insert(d);
  }
  // 2) 文件所属目录及其全部祖先（有些目录仅通过文件登记、未走 createDir）；
  //    仅取归属在该 owner 下文件的目录，避免看到他人目录。
  {
    Stmt stmt(db_.handle(),
              "SELECT DISTINCT d.dir FROM file_dir d "
              "JOIN file_node f ON f.id = d.file_id WHERE f.owner_id = ?", err);
    if (!stmt.ok()) return false;
    if (!stmt.bind(1, ownerId)) {
      err = "bind failed";
      return false;
    }
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

bool FileRepository::nameExists(std::int64_t ownerId, const std::string& dir,
                                const std::string& name, std::int64_t excludeId,
                                bool& exists, std::string& err) {
  exists = false;
  Stmt stmt(db_.handle(),
            "SELECT COUNT(*) FROM file_node f LEFT JOIN file_dir d ON d.file_id = f.id "
            "WHERE f.deleted = 0 AND COALESCE(d.dir, '') = ? AND f.name = ? "
            "AND f.owner_id = ? AND f.id != ?",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, dir) || !stmt.bind(2, name) || !stmt.bind(3, ownerId) ||
      !stmt.bind(4, excludeId)) {
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

// 清理 ref_count <= 0 的分块行，返回其 hash（blob 由调用方删除）
bool FileRepository::purgeZeroRefChunks(std::vector<std::string>& orphans, std::string& err) {
  orphans.clear();
  {
    // 安全网：只回收「引用计数归零 且 已无任何 file_chunk 引用」的分块，
    // 防止引用计数万一漂移时误删仍在被使用的 blob
    Stmt sel(db_.handle(),
             "SELECT hash FROM chunk WHERE ref_count <= 0 "
             "AND hash NOT IN (SELECT chunk_hash FROM file_chunk)",
             err);
    if (!sel.ok()) return false;
    while (true) {
      const int rc = sel.step(err);
      if (rc == SQLITE_DONE) break;
      if (rc != SQLITE_ROW) return false;
      orphans.push_back(sel.text(0));
    }
  }
  if (orphans.empty()) return true;
  Stmt del(db_.handle(),
           "DELETE FROM chunk WHERE ref_count <= 0 "
           "AND hash NOT IN (SELECT chunk_hash FROM file_chunk)",
           err);
  if (!del.ok()) return false;
  return del.step(err) == SQLITE_DONE;
}

bool FileRepository::listOrphanChunkHashes(std::vector<std::string>& out, std::string& err) {
  out.clear();
  Stmt sel(db_.handle(),
           "SELECT hash FROM chunk WHERE ref_count <= 0 "
           "AND hash NOT IN (SELECT chunk_hash FROM file_chunk)",
           err);
  if (!sel.ok()) return false;
  while (true) {
    const int rc = sel.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    out.push_back(sel.text(0));
  }
  return true;
}

bool FileRepository::deleteChunkRows(const std::vector<std::string>& hashes,
                                     std::string& err) {
  for (const std::string& h : hashes) {
    Stmt del(db_.handle(), "DELETE FROM chunk WHERE hash = ?", err);
    if (!del.ok() || !del.bind(1, h) || del.step(err) != SQLITE_DONE) {
      err = err.empty() ? "delete chunk row failed" : err;
      return false;
    }
  }
  return true;
}

bool FileRepository::softDelete(std::int64_t id, std::vector<std::string>& orphanHashes,
                                std::string& err) {
  orphanHashes.clear();
  if (!db_.exec("BEGIN IMMEDIATE", err)) return false;
  {
    // 1) 收集该文件的分块 hash
    std::vector<std::string> hashes;
    {
      Stmt sel(db_.handle(), "SELECT chunk_hash FROM file_chunk WHERE file_id = ?", err);
      if (!sel.ok() || !sel.bind(1, id)) {
        err = err.empty() ? "select file_chunk failed" : err;
        db_.exec("ROLLBACK", err);
        return false;
      }
      while (true) {
        const int rc = sel.step(err);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
          db_.exec("ROLLBACK", err);
          return false;
        }
        hashes.push_back(sel.text(0));
      }
    }
    // 2) 删除 file_chunk 链接（否则引用计数/GC 会重复处理同一批分块）
    {
      Stmt del(db_.handle(), "DELETE FROM file_chunk WHERE file_id = ?", err);
      if (!del.ok() || !del.bind(1, id) || del.step(err) != SQLITE_DONE) {
        err = err.empty() ? "delete file_chunk failed" : err;
        db_.exec("ROLLBACK", err);
        return false;
      }
    }
    // 3) 逐个递减引用计数
    for (const std::string& h : hashes) {
      Stmt dec(db_.handle(), "UPDATE chunk SET ref_count = ref_count - 1 WHERE hash = ?",
               err);
      if (!dec.ok() || !dec.bind(1, h) || dec.step(err) != SQLITE_DONE) {
        err = err.empty() ? "chunk ref decrement failed" : err;
        db_.exec("ROLLBACK", err);
        return false;
      }
    }
    // 4) 标记文件删除 + 清理所属目录关联行
    {
      Stmt stmt(db_.handle(), "UPDATE file_node SET deleted = 1 WHERE id = ?", err);
      if (!stmt.ok() || !stmt.bind(1, id) || stmt.step(err) != SQLITE_DONE) {
        err = err.empty() ? "mark deleted failed" : err;
        db_.exec("ROLLBACK", err);
        return false;
      }
      Stmt dirDel(db_.handle(), "DELETE FROM file_dir WHERE file_id = ?", err);
      if (!dirDel.ok() || !dirDel.bind(1, id) || dirDel.step(err) != SQLITE_DONE) {
        err = err.empty() ? "delete file_dir failed" : err;
        db_.exec("ROLLBACK", err);
        return false;
      }
    }
    // 5) 清理引用计数归零的分块行 → hash 交调用方删 blob（真正释放磁盘空间）
    if (!purgeZeroRefChunks(orphanHashes, err)) {
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
                                    std::vector<std::string>& orphanHashes,
                                    std::string& err) {
  orphanHashes.clear();
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
                                std::int64_t ownerId, std::int64_t size,
                                const std::string& contentHash,
                                const std::vector<std::string>& chunkHashes,
                                const std::vector<std::size_t>& chunkSizes,
                                std::int64_t& id, std::string& err) {
  if (!db_.exec("BEGIN IMMEDIATE", err)) return false;

  std::int64_t newId = 0;
  {
    Stmt ins(db_.handle(),
             "INSERT INTO file_node (name, size, content_hash, chunk_count, owner_id, "
             "created_at, deleted) VALUES (?, ?, ?, ?, ?, ?, 0)",
             err);
    if (!ins.ok()) {
      db_.exec("ROLLBACK", err);
      return false;
    }
    if (!ins.bind(1, name) || !ins.bind(2, size) || !ins.bind(3, contentHash) ||
        !ins.bind(4, static_cast<std::int64_t>(chunkHashes.size())) ||
        !ins.bind(5, ownerId) || !ins.bind(6, nowMillis())) {
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

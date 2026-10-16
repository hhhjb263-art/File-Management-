#include "meta/version_repository.h"

#include "meta/chunk_gc.h"
#include "meta/file_repository.h"  // nowMillis()

#include <cstddef>
#include <string>

namespace cv {

namespace {

const char* kSelectVersion =
    "SELECT id, file_id, version_no, content_hash, size, chunk_count, device, note, "
    "created_at FROM file_version";

void readVersionRow(Stmt& stmt, VersionRow& out) {
  out.id = stmt.int64(0);
  out.fileId = stmt.int64(1);
  out.versionNo = stmt.int64(2);
  out.contentHash = stmt.text(3);
  out.size = stmt.int64(4);
  out.chunkCount = stmt.int64(5);
  out.device = stmt.text(6);
  out.note = stmt.text(7);
  out.createdAt = stmt.int64(8);
}

// 一次性释放某「版本集合」的分块引用并删净版本行。
//   sqlDecrement   : 递减引用计数（含两个占位符，均绑定同一个 key）
//   sqlDeleteLinks : 删 file_version_chunk 引用行（一个占位符）
//   sqlDeleteVersions : 删 file_version 版本行（一个占位符）
// 顺序不可颠倒：递减必须在引用行还在时做，否则 COUNT(*) 数不出次数。
constexpr const char* kDecrementByVersion =
    "UPDATE chunk SET ref_count = ref_count - ("
    "  SELECT COUNT(*) FROM file_version_chunk"
    "  WHERE version_id = ? AND chunk_hash = chunk.hash"
    ") WHERE hash IN (SELECT chunk_hash FROM file_version_chunk WHERE version_id = ?)";
constexpr const char* kDeleteLinksByVersion =
    "DELETE FROM file_version_chunk WHERE version_id = ?";
constexpr const char* kDeleteVersionById = "DELETE FROM file_version WHERE id = ?";

constexpr const char* kDecrementByFile =
    "UPDATE chunk SET ref_count = ref_count - ("
    "  SELECT COUNT(*) FROM file_version_chunk vc JOIN file_version v ON v.id = vc.version_id"
    "  WHERE v.file_id = ? AND vc.chunk_hash = chunk.hash"
    ") WHERE hash IN ("
    "  SELECT vc.chunk_hash FROM file_version_chunk vc JOIN file_version v ON v.id = vc.version_id"
    "  WHERE v.file_id = ?"
    ")";
constexpr const char* kDeleteLinksByFile =
    "DELETE FROM file_version_chunk WHERE version_id IN "
    "(SELECT id FROM file_version WHERE file_id = ?)";
constexpr const char* kDeleteVersionsByFile = "DELETE FROM file_version WHERE file_id = ?";

}  // namespace

bool VersionRepository::listVersions(std::int64_t fileId, std::vector<VersionRow>& out,
                                     std::string& err) {
  out.clear();
  Stmt stmt(db_.handle(),
            std::string(kSelectVersion) + " WHERE file_id = ? ORDER BY version_no DESC", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, fileId)) {
    err = "bind failed";
    return false;
  }
  while (true) {
    const int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    VersionRow v;
    readVersionRow(stmt, v);
    out.push_back(v);
  }
  return true;
}

bool VersionRepository::currentVersionNo(std::int64_t fileId, std::int64_t& out,
                                         std::string& err) {
  out = 1;
  Stmt stmt(db_.handle(),
            "SELECT COALESCE(MAX(version_no), 0) + 1 FROM file_version WHERE file_id = ?", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, fileId)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return false;
  out = stmt.int64(0);
  return true;
}

bool VersionRepository::findVersion(std::int64_t versionId, VersionRow& out, std::string& err) {
  Stmt stmt(db_.handle(), std::string(kSelectVersion) + " WHERE id = ? LIMIT 1", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, versionId)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return false;
  readVersionRow(stmt, out);
  return true;
}

bool VersionRepository::captureCurrent(std::int64_t fileId, const std::string& device,
                                       const std::string& note, std::int64_t& newVersionId,
                                       std::string& err) {
  newVersionId = 0;
  if (!db_.exec("BEGIN IMMEDIATE", err)) return false;

  std::string contentHash;
  std::int64_t size = 0;
  std::int64_t chunkCount = 0;
  {
    Stmt sel(db_.handle(),
             "SELECT content_hash, size, chunk_count FROM file_node WHERE id = ? LIMIT 1", err);
    if (!sel.ok() || !sel.bind(1, fileId)) {
      err = err.empty() ? "bind failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
    if (sel.step(err) != SQLITE_ROW) {
      err = err.empty() ? "file not found" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
    contentHash = sel.text(0);
    size = sel.int64(1);
    chunkCount = sel.int64(2);
  }

  std::int64_t nextNo = 1;
  {
    Stmt sel(db_.handle(),
             "SELECT COALESCE(MAX(version_no), 0) + 1 FROM file_version WHERE file_id = ?", err);
    if (!sel.ok() || !sel.bind(1, fileId) || sel.step(err) != SQLITE_ROW) {
      err = err.empty() ? "compute next version_no failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
    nextNo = sel.int64(0);
  }

  std::int64_t vid = 0;
  {
    Stmt ins(db_.handle(),
             "INSERT INTO file_version (file_id, version_no, content_hash, size, chunk_count, "
             "device, note, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
             err);
    if (!ins.ok() || !ins.bind(1, fileId) || !ins.bind(2, nextNo) || !ins.bind(3, contentHash) ||
        !ins.bind(4, size) || !ins.bind(5, chunkCount) || !ins.bind(6, device) ||
        !ins.bind(7, note) || !ins.bind(8, nowMillis()) || ins.step(err) != SQLITE_DONE) {
      err = err.empty() ? "insert file_version failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
    vid = db_.lastInsertId();
  }

  // 分块清单：与当前内容同构地拷一份（零拷贝 —— 共享同一批 chunk 行）
  {
    Stmt cp(db_.handle(),
            "INSERT INTO file_version_chunk (version_id, seq, chunk_hash) "
            "SELECT ?, seq, chunk_hash FROM file_chunk WHERE file_id = ?",
            err);
    if (!cp.ok() || !cp.bind(1, vid) || !cp.bind(2, fileId) || cp.step(err) != SQLITE_DONE) {
      err = err.empty() ? "copy chunk list failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
  }

  // 引用计数 +出现次数（不是 +1；重复数据块必须每个 seq 都算一次）
  {
    Stmt inc(db_.handle(),
             "UPDATE chunk SET ref_count = ref_count + ("
             "  SELECT COUNT(*) FROM file_version_chunk"
             "  WHERE version_id = ? AND chunk_hash = chunk.hash"
             ") WHERE hash IN (SELECT chunk_hash FROM file_version_chunk WHERE version_id = ?)",
             err);
    if (!inc.ok() || !inc.bind(1, vid) || !inc.bind(2, vid) || inc.step(err) != SQLITE_DONE) {
      err = err.empty() ? "increment ref_count failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
  }

  if (!db_.exec("COMMIT", err)) {
    db_.exec("ROLLBACK", err);
    return false;
  }
  newVersionId = vid;
  return true;
}

bool VersionRepository::versionChunks(std::int64_t versionId, std::vector<std::string>& hashes,
                                      std::vector<std::size_t>& sizes, std::string& err) {
  hashes.clear();
  sizes.clear();
  Stmt stmt(db_.handle(),
            "SELECT vc.chunk_hash, COALESCE(c.size, 0) FROM file_version_chunk vc "
            "LEFT JOIN chunk c ON c.hash = vc.chunk_hash WHERE vc.version_id = ? "
            "ORDER BY vc.seq",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, versionId)) {
    err = "bind failed";
    return false;
  }
  while (true) {
    const int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    hashes.push_back(stmt.text(0));
    sizes.push_back(static_cast<std::size_t>(stmt.int64(1)));
  }
  return true;
}

bool VersionRepository::releaseAndDelete(const char* sqlDecrement, const char* sqlDeleteLinks,
                                         const char* sqlDeleteVersions, std::int64_t key,
                                         std::vector<std::string>& orphans, std::string& err) {
  orphans.clear();
  if (!db_.exec("BEGIN IMMEDIATE", err)) return false;
  {
    Stmt dec(db_.handle(), sqlDecrement, err);
    if (!dec.ok() || !dec.bind(1, key) || !dec.bind(2, key) || dec.step(err) != SQLITE_DONE) {
      err = err.empty() ? "decrement ref_count failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
  }
  {
    Stmt del(db_.handle(), sqlDeleteLinks, err);
    if (!del.ok() || !del.bind(1, key) || del.step(err) != SQLITE_DONE) {
      err = err.empty() ? "delete version links failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
  }
  {
    Stmt del(db_.handle(), sqlDeleteVersions, err);
    if (!del.ok() || !del.bind(1, key) || del.step(err) != SQLITE_DONE) {
      err = err.empty() ? "delete version rows failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
  }
  if (!purgeZeroRefChunks(db_.handle(), orphans, err)) {
    db_.exec("ROLLBACK", err);
    return false;
  }
  if (!db_.exec("COMMIT", err)) {
    db_.exec("ROLLBACK", err);
    return false;
  }
  return true;
}

bool VersionRepository::deleteVersion(std::int64_t versionId, std::vector<std::string>& orphans,
                                      std::string& err) {
  return releaseAndDelete(kDecrementByVersion, kDeleteLinksByVersion, kDeleteVersionById,
                          versionId, orphans, err);
}

bool VersionRepository::purgeFileVersions(std::int64_t fileId,
                                          std::vector<std::string>& orphans, std::string& err) {
  return releaseAndDelete(kDecrementByFile, kDeleteLinksByFile, kDeleteVersionsByFile, fileId,
                          orphans, err);
}

bool VersionRepository::pruneVersions(std::int64_t fileId, std::int64_t keep,
                                      std::int64_t cutoffMillis,
                                      std::vector<std::string>& orphans, std::string& err) {
  orphans.clear();
  if (keep <= 0 && cutoffMillis <= 0) return true;  // 两项都不限：什么都不做

  // 一次取全（按版本号倒序）后在 C++ 侧判"双限取先到者"，比拼一个复杂 SQL 更好读；
  // 版本数天然有上限（本函数的产物），全量取出的代价可以忽略。
  struct Ver {
    std::int64_t id = 0;
    std::int64_t createdAt = 0;
  };
  std::vector<Ver> vers;
  {
    Stmt stmt(db_.handle(),
              "SELECT id, created_at FROM file_version WHERE file_id = ? "
              "ORDER BY version_no DESC",
              err);
    if (!stmt.ok()) return false;
    if (!stmt.bind(1, fileId)) {
      err = "bind failed";
      return false;
    }
    while (true) {
      const int rc = stmt.step(err);
      if (rc == SQLITE_DONE) break;
      if (rc != SQLITE_ROW) return false;
      Ver v;
      v.id        = stmt.int64(0);
      v.createdAt = stmt.int64(1);
      vers.push_back(v);
    }
  }

  std::vector<std::int64_t> stale;
  for (std::size_t i = 0; i < vers.size(); ++i) {
    const bool overCount = (keep > 0 && static_cast<std::int64_t>(i) >= keep);
    const bool tooOld    = (cutoffMillis > 0 && vers[i].createdAt < cutoffMillis);
    if (overCount || tooOld) stale.push_back(vers[i].id);
  }

  for (std::int64_t vid : stale) {
    std::vector<std::string> part;
    if (!deleteVersion(vid, part, err)) return false;
    for (const std::string& hh : part) orphans.push_back(hh);
  }
  return true;
}

bool VersionRepository::countVersions(std::int64_t fileId, std::int64_t& out, std::string& err) {
  out = 0;
  Stmt stmt(db_.handle(), "SELECT COUNT(*) FROM file_version WHERE file_id = ?", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, fileId)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return false;
  out = stmt.int64(0);
  return true;
}

}  // namespace cv

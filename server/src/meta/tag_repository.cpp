#include "meta/tag_repository.h"

#include "meta/file_repository.h"  // nowMillis()

#include <string>

namespace cv {

namespace {

// 列表/单查共用的列与计数子查询（fileCount 只数未删除文件，与配额口径一致）
const char* kSelectTag =
    "SELECT t.id, t.owner_id, t.name, t.color, t.created_at, "
    "(SELECT COUNT(*) FROM file_tag ft JOIN file_node f ON f.id = ft.file_id "
    " WHERE ft.tag_id = t.id AND f.deleted = 0) AS cnt "
    "FROM tag t";

void readTagRow(Stmt& stmt, TagRow& out) {
  out.id = stmt.int64(0);
  out.ownerId = stmt.int64(1);
  out.name = stmt.text(2);
  out.color = stmt.text(3);
  out.createdAt = stmt.int64(4);
  out.fileCount = stmt.int64(5);
}

}  // namespace

bool TagRepository::listTags(std::int64_t ownerId, std::vector<TagRow>& out, std::string& err) {
  out.clear();
  Stmt stmt(db_.handle(),
            std::string(kSelectTag) +
                " WHERE t.owner_id = ? ORDER BY t.name COLLATE NOCASE ASC",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, ownerId)) {
    err = "bind failed";
    return false;
  }
  while (true) {
    const int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    TagRow t;
    readTagRow(stmt, t);
    out.push_back(t);
  }
  return true;
}

bool TagRepository::findTag(std::int64_t ownerId, std::int64_t tagId, TagRow& out,
                            std::string& err) {
  Stmt stmt(db_.handle(), std::string(kSelectTag) + " WHERE t.id = ? AND t.owner_id = ? LIMIT 1",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, tagId) || !stmt.bind(2, ownerId)) {
    err = "bind failed";
    return false;
  }
  if (stmt.step(err) != SQLITE_ROW) return false;
  readTagRow(stmt, out);
  return true;
}

bool TagRepository::createTag(std::int64_t ownerId, const std::string& name,
                              const std::string& color, TagRow& out, bool& created,
                              std::string& err) {
  created = false;

  // 先查同名（NOCASE）—— 幂等：已有就直接返回，不报错
  {
    Stmt sel(db_.handle(),
             std::string(kSelectTag) +
                 " WHERE t.owner_id = ? AND t.name = ? COLLATE NOCASE LIMIT 1",
             err);
    if (!sel.ok()) return false;
    if (!sel.bind(1, ownerId) || !sel.bind(2, name)) {
      err = "bind failed";
      return false;
    }
    const int rc = sel.step(err);
    if (rc == SQLITE_ROW) {
      readTagRow(sel, out);
      return true;  // created 保持 false
    }
    if (rc != SQLITE_DONE) return false;
  }

  if (!db_.exec("BEGIN IMMEDIATE", err)) return false;
  {
    Stmt ins(db_.handle(),
             "INSERT INTO tag (owner_id, name, color, created_at) VALUES (?, ?, ?, ?)", err);
    if (!ins.ok() || !ins.bind(1, ownerId) || !ins.bind(2, name) || !ins.bind(3, color) ||
        !ins.bind(4, nowMillis()) || ins.step(err) != SQLITE_DONE) {
      err = err.empty() ? "insert tag failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
  }
  const std::int64_t newId = db_.lastInsertId();
  if (!db_.exec("COMMIT", err)) {
    db_.exec("ROLLBACK", err);
    return false;
  }

  created = true;
  out.id = newId;
  out.ownerId = ownerId;
  out.name = name;
  out.color = color;
  out.fileCount = 0;
  return true;
}

bool TagRepository::renameTag(std::int64_t ownerId, std::int64_t tagId, const std::string& name,
                              const std::string& color, std::string& err) {
  TagRow exist;
  if (!findTag(ownerId, tagId, exist, err)) {
    err = err.empty() ? "tag not found" : err;
    return false;
  }
  if (exist.name != name) {
    Stmt dup(db_.handle(),
             "SELECT 1 FROM tag WHERE owner_id = ? AND name = ? COLLATE NOCASE AND id <> ? LIMIT 1",
             err);
    if (!dup.ok() || !dup.bind(1, ownerId) || !dup.bind(2, name) || !dup.bind(3, tagId)) {
      err = err.empty() ? "bind failed" : err;
      return false;
    }
    if (dup.step(err) == SQLITE_ROW) {
      err = "name exists";
      return false;
    }
    err.clear();
  }
  Stmt upd(db_.handle(), "UPDATE tag SET name = ?, color = ? WHERE id = ? AND owner_id = ?", err);
  if (!upd.ok() || !upd.bind(1, name) || !upd.bind(2, color) || !upd.bind(3, tagId) ||
      !upd.bind(4, ownerId) || upd.step(err) != SQLITE_DONE) {
    err = err.empty() ? "update tag failed" : err;
    return false;
  }
  return true;
}

bool TagRepository::deleteTag(std::int64_t ownerId, std::int64_t tagId, std::string& err) {
  TagRow exist;
  if (!findTag(ownerId, tagId, exist, err)) {
    err = err.empty() ? "tag not found" : err;
    return false;
  }
  if (!db_.exec("BEGIN IMMEDIATE", err)) return false;
  {
    // 级联清理关联行（无外键，必须显式删，否则留下悬空 tag_id）
    Stmt del(db_.handle(), "DELETE FROM file_tag WHERE tag_id = ?", err);
    if (!del.ok() || !del.bind(1, tagId) || del.step(err) != SQLITE_DONE) {
      err = err.empty() ? "delete file_tag failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
  }
  {
    Stmt del(db_.handle(), "DELETE FROM tag WHERE id = ? AND owner_id = ?", err);
    if (!del.ok() || !del.bind(1, tagId) || !del.bind(2, ownerId) ||
        del.step(err) != SQLITE_DONE) {
      err = err.empty() ? "delete tag failed" : err;
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

bool TagRepository::tagsOfFile(std::int64_t fileId, std::vector<TagRow>& out, std::string& err) {
  out.clear();
  Stmt stmt(db_.handle(),
            std::string(kSelectTag) +
                " JOIN file_tag ft ON ft.tag_id = t.id WHERE ft.file_id = ? "
                "ORDER BY t.name COLLATE NOCASE ASC",
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
    TagRow t;
    readTagRow(stmt, t);
    out.push_back(t);
  }
  return true;
}

bool TagRepository::setFileTags(std::int64_t ownerId, std::int64_t fileId,
                                const std::vector<std::int64_t>& tagIds, std::string& err) {
  if (!db_.exec("BEGIN IMMEDIATE", err)) return false;
  {
    Stmt del(db_.handle(), "DELETE FROM file_tag WHERE file_id = ?", err);
    if (!del.ok() || !del.bind(1, fileId) || del.step(err) != SQLITE_DONE) {
      err = err.empty() ? "clear file_tag failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
  }
  for (std::int64_t tid : tagIds) {
    if (tid <= 0) continue;
    // 只关联属于该 owner 的标签：跨 owner / 不存在的 id 会被 SELECT 过滤掉（静默跳过）
    Stmt ins(db_.handle(),
             "INSERT OR IGNORE INTO file_tag (file_id, tag_id) "
             "SELECT ?, id FROM tag WHERE owner_id = ? AND id = ?",
             err);
    if (!ins.ok() || !ins.bind(1, fileId) || !ins.bind(2, ownerId) || !ins.bind(3, tid) ||
        ins.step(err) != SQLITE_DONE) {
      err = err.empty() ? "insert file_tag failed" : err;
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

bool TagRepository::listFileIdsByTags(std::int64_t ownerId,
                                      const std::vector<std::int64_t>& tagIds,
                                      std::vector<std::int64_t>& out, std::string& err) {
  out.clear();
  if (tagIds.empty()) return true;  // 空集，不返回全集（避免"没选标签"变成"列出全部"）

  // tagIds 是整数，拼接无注入风险；用 IN 列表 + DISTINCT 天然实现"命中任一标签"
  std::string in;
  for (std::size_t i = 0; i < tagIds.size(); ++i) {
    if (tagIds[i] <= 0) continue;
    if (!in.empty()) in += ",";
    in += std::to_string(tagIds[i]);
  }
  if (in.empty()) return true;

  Stmt stmt(db_.handle(),
            "SELECT DISTINCT f.id FROM file_node f JOIN file_tag ft ON ft.file_id = f.id "
            "WHERE f.owner_id = ? AND f.deleted = 0 AND ft.tag_id IN (" +
                in + ") ORDER BY f.id DESC",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, ownerId)) {
    err = "bind failed";
    return false;
  }
  while (true) {
    const int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    out.push_back(stmt.int64(0));
  }
  return true;
}

}  // namespace cv

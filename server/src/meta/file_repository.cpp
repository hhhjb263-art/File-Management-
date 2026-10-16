#include "meta/file_repository.h"

#include "meta/chunk_gc.h"

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
    "COALESCE(d.dir, ''), f.owner_id, f.deleted_at FROM file_node f "
    "LEFT JOIN file_dir d ON d.file_id = f.id WHERE f.deleted = 0";

// 同 kSelectFile 的列顺序，但**不过滤 deleted** —— 回收站相关查询用
const char* kSelectFileAny =
    "SELECT f.id, f.name, f.size, f.content_hash, f.chunk_count, f.created_at, "
    "COALESCE(d.dir, ''), f.owner_id, f.deleted_at FROM file_node f "
    "LEFT JOIN file_dir d ON d.file_id = f.id WHERE 1=1";

void readFileRow(Stmt& stmt, FileRow& out) {
  out.id = stmt.int64(0);
  out.name = stmt.text(1);
  out.size = stmt.int64(2);
  out.contentHash = stmt.text(3);
  out.chunkCount = static_cast<int>(stmt.int64(4));
  out.createdAt = stmt.int64(5);
  out.dir = stmt.text(6);
  out.ownerId = stmt.int64(7);
  out.deletedAt = stmt.int64(8);
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

bool FileRepository::findByIdAny(std::int64_t id, FileRow& out, std::string& err) {
  std::string sql = std::string(kSelectFileAny) + " AND f.id = ? LIMIT 1";
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

// 转义 LIKE 的通配符：目录名里含 % 或 _ 时若不转义，前缀匹配会**扩大**范围
// （例如 dir 叫 "a_b" 时 `a_b/%` 会连 "axb/..." 一起匹配进来）。
// 前向声明：escapeLikePattern 的定义在本文件后半（listFiles 之前），
// 而 moveDir 需要用它。放在这里比把定义整体前移风险小。
std::string escapeLikePattern(const std::string& s);

// 取相对路径的父目录段（'' 表示根）；也用于"to 的父目录是否合法"
std::string dirParentOf(const std::string& rel) {
  const std::size_t p = rel.rfind('/');
  return (p == std::string::npos) ? std::string() : rel.substr(0, p);
}

bool FileRepository::dirExists(std::int64_t ownerId, const std::string& dir, bool& out,
                               std::string& err) {
  out = false;
  if (dir.empty()) {           // 根目录恒存在
    out = true;
    return true;
  }
  Stmt stmt(db_.handle(),
            "SELECT 1 FROM dir_node WHERE owner_id = ? AND path = ? AND deleted = 0 LIMIT 1", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, ownerId) || !stmt.bind(2, dir)) {
    err = "bind failed";
    return false;
  }
  const int rc = stmt.step(err);
  if (rc == SQLITE_DONE) return true;
  if (rc != SQLITE_ROW) return false;
  out = true;
  return true;
}

bool FileRepository::moveFileToDir(std::int64_t ownerId, std::int64_t id,
                                   const std::string& newDir, std::string& err) {
  FileRow row;
  // findById 已带 deleted=0 过滤；owner 不匹配一律按"查不到"处理，不泄漏他人元数据
  if (!findById(id, row, err)) return false;
  if (row.ownerId != ownerId) {
    err = "file not found";
    return false;
  }

  bool dExists = false;
  if (!dirExists(ownerId, newDir, dExists, err)) return false;
  if (!dExists) {
    err = "目标目录不存在";
    return false;
  }

  // 同名冲突（把自己排除在外，否则"移到当前目录"会误判为冲突）
  bool dup = false;
  if (!nameExists(ownerId, newDir, row.name, id, dup, err)) return false;
  if (dup) {
    err = "目标目录下已存在同名条目";
    return false;
  }

  Stmt stmt(db_.handle(),
            "INSERT INTO file_dir (file_id, dir) VALUES (?, ?) "
            "ON CONFLICT(file_id) DO UPDATE SET dir = excluded.dir",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, id) || !stmt.bind(2, newDir)) {
    err = "bind failed";
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool FileRepository::moveDir(std::int64_t ownerId, const std::string& from,
                             const std::string& to, std::string& err) {
  if (from.empty()) {
    err = "不能移动根目录";
    return false;
  }
  if (to.empty()) {
    err = "目标路径不能为空";
    return false;
  }
  if (from == to) return true;   // 空操作视为成功

  // ---- 1) 源目录必须存在 ----
  bool fromExists = false;
  if (!dirExists(ownerId, from, fromExists, err)) return false;
  if (!fromExists) {
    err = "源目录不存在";
    return false;
  }

  // ---- 2) 不能移到自己或自己的子孙里 ----
  // 否则这棵子树会从树上"脱落"：数据都还在库里，但没有任何路径能到达它 —— 用户看到的是文件凭空消失。
  if (to.rfind(from + "/", 0) == 0) {
    err = "不能把目录移动到它自己或它的子目录里";
    return false;
  }

  // ---- 3) 目标父目录必须存在 ----
  const std::string toParent = dirParentOf(to);
  if (!toParent.empty()) {
    bool pExists = false;
    if (!dirExists(ownerId, toParent, pExists, err)) return false;
    if (!pExists) {
      err = "目标父目录不存在";
      return false;
    }
  }

  // ---- 4) 目标位置不能已被占用（目录占位或同名文件占位）----
  {
    bool dupDir = false;
    if (!dirExists(ownerId, to, dupDir, err)) return false;
    if (dupDir) {
      err = "目标目录已存在";
      return false;
    }
    const std::size_t slash = to.rfind('/');
    const std::string toName = (slash == std::string::npos) ? to : to.substr(slash + 1);
    bool dupFile = false;
    if (!nameExists(ownerId, toParent, toName, 0, dupFile, err)) return false;
    if (dupFile) {
      err = "目标位置已存在同名条目";
      return false;
    }
  }

  // ---- 5) 纯元数据改写（一个事务内，保证不会改一半）----
  // 只做前缀替换：dir_node.path 与 file_dir.dir。
  // 注意 `substr(x, len+1)` 对"恰好等于 from"的行会得到空串，于是 to||'' == to —— 一条 SQL 覆盖两者。
  const std::string like  = escapeLikePattern(from) + "/%";
  const std::string likeT = escapeLikePattern(to) + "/%";
  if (!db_.exec("BEGIN IMMEDIATE", err)) return false;

  // ⚠️ 绑定顺序必须与 SQL 占位符**逐一对应**：SET(to) || substr(from) WHERE (from OR likeFrom) AND owner。
  //    这里一度漏绑了第 3 个（path = ?）而把 likeFrom 顶了上去，结果 WHERE 永远匹配不到行 ——
  //    "移动成功"却什么都没发生（harness 才把它抓出来）。
  const auto run = [&](const char* sql, const std::string& toVal, const std::string& fromVal,
                       const std::string& likeFromVal) -> bool {
    Stmt st(db_.handle(), sql, err);
    if (!st.ok()) return false;
    int i = 1;
    if (!st.bind(i++, toVal) ||      // SET 新前缀
        !st.bind(i++, fromVal) ||    // length(?) —— 与 from 同长，故也传 from
        !st.bind(i++, fromVal) ||    // path/dir = ?
        !st.bind(i++, likeFromVal) ||// path/dir LIKE ?
        !st.bind(i++, ownerId)) {    // owner_id
      err = "bind failed";
      return false;
    }
    return st.step(err) == SQLITE_DONE;
  };

  // dir_node：连**回收站里的子目录**一起改（否则从回收站恢复出来会落在已经不存在的路径上）
  if (!run("UPDATE dir_node SET path = ? || substr(path, length(?) + 1) "
           "WHERE (path = ? OR path LIKE ? ESCAPE '\\') AND owner_id = ?",
           to, from, like)) {
    db_.exec("ROLLBACK", err);
    return false;
  }
  // file_dir：同样连回收站里的文件一起改；只限本 owner 的文件
  if (!run("UPDATE file_dir SET dir = ? || substr(dir, length(?) + 1) "
           "WHERE (dir = ? OR dir LIKE ? ESCAPE '\\') "
           "  AND file_id IN (SELECT id FROM file_node WHERE owner_id = ?)",
           to, from, like)) {
    db_.exec("ROLLBACK", err);
    return false;
  }
  if (!db_.exec("COMMIT", err)) {
    db_.exec("ROLLBACK", err);
    return false;
  }
  return true;
}

bool FileRepository::listReferencedChunkHashes(std::vector<std::string>& out, std::string& err) {
  out.clear();
  Stmt stmt(db_.handle(), "SELECT hash FROM chunk WHERE ref_count > 0", err);
  if (!stmt.ok()) return false;
  while (true) {
    const int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    out.push_back(stmt.text(0));
  }
  return true;
}

bool FileRepository::filesUsingChunk(const std::string& hash, int maxLabels,
                                     std::vector<std::string>& out, std::string& err) {
  out.clear();
  Stmt stmt(db_.handle(),
            "SELECT COALESCE(d.dir, ''), f.name FROM file_chunk fc "
            "JOIN file_node f ON f.id = fc.file_id "
            "LEFT JOIN file_dir d ON d.file_id = f.id "
            "WHERE fc.chunk_hash = ? ORDER BY f.id LIMIT ?",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, hash) || !stmt.bind(2, static_cast<std::int64_t>(maxLabels > 0 ? maxLabels : 1))) {
    err = "bind failed";
    return false;
  }
  while (true) {
    const int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    const std::string dir  = stmt.text(0);
    const std::string name = stmt.text(1);
    out.push_back(dir.empty() ? name : (dir + "/" + name));
  }
  return true;
}

bool FileRepository::chunkExists(const std::string& hash, bool& out, std::string& err) {
  out = false;
  Stmt stmt(db_.handle(), "SELECT 1 FROM chunk WHERE hash = ? LIMIT 1", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, hash)) {
    err = "bind failed";
    return false;
  }
  const int rc = stmt.step(err);
  if (rc == SQLITE_DONE) return true;    // 无行 ⇒ 未被引用
  if (rc != SQLITE_ROW) return false;
  out = true;
  return true;
}

std::string escapeLikePattern(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '%' || c == '_' || c == '\\') o.push_back('\\');
    o.push_back(c);
  }
  return o;
}

// 兼容旧签名：不加过滤、不设上限（**不再固定 500** —— 那正是丢条目的根因）。
bool FileRepository::listFiles(std::int64_t ownerId, std::vector<FileRow>& out,
                              std::string& err) {
  FileListQuery q;
  std::int64_t total = 0;
  return listFiles(ownerId, q, out, total, err);
}

bool FileRepository::listFiles(std::int64_t ownerId, const FileListQuery& q,
                               std::vector<FileRow>& out, std::int64_t& outTotal,
                               std::string& err) {
  out.clear();
  outTotal = 0;

  // 过滤片段：一律走占位符绑定（dir 来自客户端，绝不拼字符串进 SQL）
  std::string where = " AND f.owner_id = ?";
  const bool byUnder = (!q.filterByDir && q.filterByUnder && !q.underPrefix.empty());
  if (q.filterByDir) {
    where += " AND COALESCE(d.dir, '') = ?";
  } else if (byUnder) {
    where += " AND (COALESCE(d.dir, '') = ? OR COALESCE(d.dir, '') LIKE ? ESCAPE '\\')";
  }

  // ---- 总数：不受 limit/offset 影响，供 hasMore 与界面显示 ----
  {
    std::string sql =
        "SELECT COUNT(*) FROM file_node f LEFT JOIN file_dir d ON d.file_id = f.id "
        "WHERE f.deleted = 0" + where;
    Stmt stmt(db_.handle(), sql, err);
    if (!stmt.ok()) return false;
    int idx = 1;
    if (!stmt.bind(idx++, ownerId)) { err = "bind failed"; return false; }
    if (q.filterByDir) {
      if (!stmt.bind(idx++, q.dirExact)) { err = "bind failed"; return false; }
    } else if (byUnder) {
      const std::string likeArg = escapeLikePattern(q.underPrefix) + "/%";
      if (!stmt.bind(idx++, q.underPrefix) || !stmt.bind(idx++, likeArg)) {
        err = "bind failed";
        return false;
      }
    }
    if (stmt.step(err) != SQLITE_ROW) return false;
    outTotal = stmt.int64(0);
  }

  // ---- 分页取数 ----
  {
    std::string sql = std::string(kSelectFile) + where + " ORDER BY f.id DESC";
    if (q.limit > 0) sql += " LIMIT ? OFFSET ?";
    Stmt stmt(db_.handle(), sql, err);
    if (!stmt.ok()) return false;
    int idx = 1;
    if (!stmt.bind(idx++, ownerId)) { err = "bind failed"; return false; }
    if (q.filterByDir) {
      if (!stmt.bind(idx++, q.dirExact)) { err = "bind failed"; return false; }
    } else if (byUnder) {
      const std::string likeArg = escapeLikePattern(q.underPrefix) + "/%";
      if (!stmt.bind(idx++, q.underPrefix) || !stmt.bind(idx++, likeArg)) {
        err = "bind failed";
        return false;
      }
    }
    if (q.limit > 0) {
      if (!stmt.bind(idx++, q.limit) || !stmt.bind(idx++, q.offset < 0 ? 0 : q.offset)) {
        err = "bind failed";
        return false;
      }
    }
    while (true) {
      const int rc = stmt.step(err);
      if (rc == SQLITE_DONE) break;
      if (rc != SQLITE_ROW) return false;
      FileRow row;
      readFileRow(stmt, row);
      out.push_back(row);
    }
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
            "SELECT path FROM dir_node WHERE owner_id = ? AND deleted = 0 ORDER BY path", err);
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
              "JOIN file_node f ON f.id = d.file_id "
              "WHERE f.owner_id = ? AND f.deleted = 0", err);
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

// 清理 ref_count <= 0 的分块行，返回其 hash（blob 由调用方删除）。
// 判据与版本侧共用（meta/chunk_gc.h）—— 两侧必须完全一致，否则会误删对方还在用的 blob。
bool FileRepository::purgeZeroRefChunks(std::vector<std::string>& orphans, std::string& err) {
  return cv::purgeZeroRefChunks(db_.handle(), orphans, err);
}

bool FileRepository::listOrphanChunkHashes(std::vector<std::string>& out, std::string& err) {
  out.clear();
  Stmt sel(db_.handle(),
           "SELECT hash FROM chunk WHERE ref_count <= 0 "
           "AND hash NOT IN (SELECT chunk_hash FROM file_chunk) "
           "AND hash NOT IN (SELECT chunk_hash FROM file_version_chunk)",
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

// 彻底删除（不可恢复）。历史名称 softDelete —— 它其实是"硬删除"：
// 置 deleted=1 且 deleted_at 保持 0，并真正解除分块引用。
// 回收站的软删除走 trashFile()，二者语义不同，勿混用。
bool FileRepository::purgeFile(std::int64_t id, std::vector<std::string>& orphanHashes,
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
    // 4) 标记文件删除（deleted_at 归零 = 明确标记为"硬删除"，不会出现在回收站）
    //    + 清理所属目录关联行 + 清理标签关联行（同库同事务，保证不留悬空引用）
    {
      Stmt stmt(db_.handle(),
                "UPDATE file_node SET deleted = 1, deleted_at = 0 WHERE id = ?", err);
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
      Stmt tagDel(db_.handle(), "DELETE FROM file_tag WHERE file_id = ?", err);
      if (!tagDel.ok() || !tagDel.bind(1, id) || tagDel.step(err) != SQLITE_DONE) {
        err = err.empty() ? "delete file_tag failed" : err;
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
    // ⚠️ 引用计数必须**按出现次数**递减，不能写成
    //      UPDATE chunk SET ref_count = ref_count - 1
    //       WHERE hash IN (SELECT chunk_hash FROM file_chunk WHERE file_id = ?)
    //    因为 IN (子查询) 是**集合**语义：同一文件内不同 seq 若分块内容相同（重复数据块，
    //    例如全零块或重复段落），该 hash 只出现一次 ⇒ 只减 1，而 addChunkIfAbsent 是按
    //    出现次数逐次 +1。差值会永久留在 ref_count 上，purgeZeroRefChunks 永远收不走，
    //    对应 blob 就成了谁都不引用却又删不掉的泄漏（磁盘只涨不降）。
    //    这里与 purgeFile() 保持一致：先收集 hash 列表（保留重复项），再逐个递减。
    //    （VersionRepository 用相关子查询 COUNT(*) 实现同一效果，见其 kDecrementBy* 常量。）
    std::vector<std::string> oldHashes;
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
        oldHashes.push_back(sel.text(0));
      }
    }
    Stmt del(db_.handle(), "DELETE FROM file_chunk WHERE file_id = ?", err);
    if (!del.ok() || !del.bind(1, id) || del.step(err) != SQLITE_DONE) {
      err = err.empty() ? "clear file_chunk failed" : err;
      db_.exec("ROLLBACK", err);
      return false;
    }
    for (const std::string& h : oldHashes) {
      Stmt dec(db_.handle(), "UPDATE chunk SET ref_count = ref_count - 1 WHERE hash = ?", err);
      if (!dec.ok() || !dec.bind(1, h) || dec.step(err) != SQLITE_DONE) {
        err = err.empty() ? "chunk ref decrement failed" : err;
        db_.exec("ROLLBACK", err);
        return false;
      }
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
  // 收尾：把引用计数归零的分块行清掉，并把它们的 hash 回传给调用方删 blob。
  // ⚠️ 这一步原先**漏了**：函数头注释承诺"旧内容中引用计数归零的分块 hash 通过
  //    orphanHashes 返回，由调用方删 blob"，但实现里 orphanHashes 一直是空的 ⇒
  //    覆盖上传 / 版本回滚之后，旧内容的 blob 文件永远收不到删除指令。
  //    （chunk 行本身会被后续任何一次 purgeFile/purgeFileVersions 全局扫走，
  //      所以表现为"空间迟迟不释放"，而不是立刻报错 —— 更难被发现。）
  //    这里与 purgeFile() 对齐：同事务内 sweep 一次，把 hash 交给调用方。
  if (!purgeZeroRefChunks(orphanHashes, err)) {
    db_.exec("ROLLBACK", err);
    return false;
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

// ===========================================================================
//  回收站（软删除 / 恢复 / 彻底删除）
// ===========================================================================

// 进回收站。**刻意保留** file_chunk / file_dir / chunk.ref_count：
//   · 保留 file_chunk + ref_count → 内容仍被引用，去重共享的 blob 不会被 GC 掉，恢复才有可能
//   · 保留 file_dir              → 恢复时能回到原位置（PRD FR-8-2）
// 代价：磁盘没释放（内容库不 GC）。但**注意口径**：sumSizeOfOwner 带 `WHERE deleted = 0`，
//   所以进站条目会立刻从「已用空间」里消失 —— 磁盘占用与用量数字在回收站非空时必然不一致。
//   只有 purgeFile 才递减 ref_count 并让 blob 真正可回收。
bool FileRepository::trashFile(std::int64_t id, std::int64_t nowMillis, std::string& err) {
  Stmt stmt(db_.handle(),
            "UPDATE file_node SET deleted = 1, deleted_at = ? WHERE id = ? AND deleted = 0",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, nowMillis) || !stmt.bind(2, id)) {
    err = "bind failed";
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool FileRepository::restoreFile(std::int64_t id, std::string& outDir, std::string& err) {
  outDir.clear();
  // 取原目录（用于调用方判断是否需要重建目录链）
  {
    Stmt sel(db_.handle(), "SELECT COALESCE(dir, '') FROM file_dir WHERE file_id = ? LIMIT 1",
             err);
    if (!sel.ok()) return false;
    if (!sel.bind(1, id)) {
      err = "bind failed";
      return false;
    }
    if (sel.step(err) == SQLITE_ROW) outDir = sel.text(0);
  }
  Stmt stmt(db_.handle(),
            "UPDATE file_node SET deleted = 0, deleted_at = 0 WHERE id = ? AND deleted = 1",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, id)) {
    err = "bind failed";
    return false;
  }
  return stmt.step(err) == SQLITE_DONE;
}

bool FileRepository::listTrash(std::int64_t ownerId, std::vector<FileRow>& files,
                               std::vector<TrashDirRow>& dirs, std::string& err) {
  files.clear();
  dirs.clear();

  // ---- 顶层目录 ----
  // 取全部在回收站的目录，再在 C++ 侧剔除"父目录也在回收站"的（父在站内的子项随父显示）。
  // 不用 SQL 拼父路径 —— SQLite 没有 reverse()，靠 substr/instr 拼出来既难读又易错。
  std::vector<TrashDirRow> allTrashDirs;
  if (!listTrashDirs(ownerId, allTrashDirs, err)) return false;
  {
    std::set<std::string> trashedSet;
    for (const TrashDirRow& d : allTrashDirs) trashedSet.insert(d.path);
    for (const TrashDirRow& d : allTrashDirs) {
      const std::size_t slash = d.path.find_last_of('/');
      const std::string parent = (slash == std::string::npos) ? std::string()
                                                             : d.path.substr(0, slash);
      if (trashedSet.find(parent) == trashedSet.end()) dirs.push_back(d);
    }
  }

  // ---- 顶层文件 ----
  // 自身在回收站，且所属目录不在回收站（否则它随目录一起显示）。
  {
    std::set<std::string> trashedSet;
    for (const TrashDirRow& d : allTrashDirs) trashedSet.insert(d.path);

    std::string sql = std::string(kSelectFileAny) +
                      " AND f.owner_id = ? AND f.deleted = 1 AND f.deleted_at > 0 "
                      "ORDER BY f.deleted_at DESC";
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
      if (trashedSet.find(row.dir) != trashedSet.end()) continue;  // 随目录显示，跳过
      files.push_back(row);
    }
  }
  return true;
}

bool FileRepository::listAllTrashedIds(std::int64_t ownerId, std::vector<std::int64_t>& out,
                                       std::string& err) {
  out.clear();
  Stmt stmt(db_.handle(),
            "SELECT id FROM file_node WHERE owner_id = ? AND deleted = 1 AND deleted_at > 0",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, ownerId)) {
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

bool FileRepository::listExpiredTrashIds(std::int64_t ownerId, std::int64_t deadline,
                                         std::vector<std::int64_t>& out, std::string& err) {
  out.clear();
  Stmt stmt(db_.handle(),
            "SELECT id FROM file_node WHERE owner_id = ? AND deleted = 1 "
            "AND deleted_at > 0 AND deleted_at < ?",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, ownerId) || !stmt.bind(2, deadline)) {
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

// ===========================================================================
//  目录（含回收站状态）
// ===========================================================================

bool FileRepository::isDirDeleted(std::int64_t ownerId, const std::string& path,
                                  bool& deleted, std::string& err) {
  // 目录不存在 → 视为"需要重建"（返回 deleted=true），与"已删除"同一条恢复路径
  deleted = true;
  Stmt stmt(db_.handle(),
            "SELECT deleted FROM dir_node WHERE owner_id = ? AND path = ? LIMIT 1", err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, ownerId) || !stmt.bind(2, path)) {
    err = "bind failed";
    return false;
  }
  const int rc = stmt.step(err);
  if (rc == SQLITE_DONE) return true;  // 不存在
  if (rc != SQLITE_ROW) return false;
  deleted = stmt.int64(0) != 0;
  return true;
}

bool FileRepository::listDirPathsUnder(std::int64_t ownerId, const std::string& path,
                                       std::vector<std::string>& out, std::string& err) {
  out.clear();
  // 自身 + 以 "path/" 开头的全部后代（LIKE 前缀匹配；转义 % _ 以避免路径里的通配符误命中）
  std::string prefix = path.empty() ? std::string() : path + "/";
  std::string esc;
  esc.reserve(prefix.size());
  for (char c : prefix) {
    if (c == '%' || c == '_' || c == '\\') esc.push_back('\\');
    esc.push_back(c);
  }
  Stmt stmt(db_.handle(),
            "SELECT path FROM dir_node WHERE owner_id = ? AND (path = ? OR path LIKE ? ESCAPE '\\') "
            "ORDER BY path",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, ownerId) || !stmt.bind(2, path) || !stmt.bind(3, esc + "%")) {
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

bool FileRepository::listFileIdsUnderDir(std::int64_t ownerId, const std::string& path,
                                        std::vector<std::int64_t>& out, std::string& err) {
  out.clear();
  std::string prefix = path.empty() ? std::string() : path + "/";
  std::string esc;
  esc.reserve(prefix.size());
  for (char c : prefix) {
    if (c == '%' || c == '_' || c == '\\') esc.push_back('\\');
    esc.push_back(c);
  }
  Stmt stmt(db_.handle(),
            "SELECT f.id FROM file_node f JOIN file_dir d ON d.file_id = f.id "
            "WHERE f.owner_id = ? AND f.deleted = 0 "
            "AND (COALESCE(d.dir,'') = ? OR COALESCE(d.dir,'') LIKE ? ESCAPE '\\')",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, ownerId) || !stmt.bind(2, path) || !stmt.bind(3, esc + "%")) {
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

bool FileRepository::listTrashedFilesUnderDir(std::int64_t ownerId, const std::string& path,
                                              std::int64_t sinceMillis,
                                              std::vector<FileRow>& out, std::string& err) {
  out.clear();
  std::string prefix = path.empty() ? std::string() : path + "/";
  std::string esc;
  esc.reserve(prefix.size());
  for (char c : prefix) {
    if (c == '%' || c == '_' || c == '\\') esc.push_back('\\');
    esc.push_back(c);
  }
  std::string sql = std::string(kSelectFileAny) +
                    " AND f.owner_id = ? AND f.deleted = 1 AND f.deleted_at > 0 "
                    "AND (COALESCE(d.dir,'') = ? OR COALESCE(d.dir,'') LIKE ? ESCAPE '\\')";
  if (sinceMillis > 0) sql += " AND f.deleted_at >= ?";
  sql += " ORDER BY f.id DESC";
  Stmt stmt(db_.handle(), sql, err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, ownerId) || !stmt.bind(2, path) || !stmt.bind(3, esc + "%")) {
    err = "bind failed";
    return false;
  }
  if (sinceMillis > 0 && !stmt.bind(4, sinceMillis)) {
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

bool FileRepository::setDirsTrashed(std::int64_t ownerId, const std::vector<std::string>& paths,
                                    bool trashed, std::int64_t nowMillis, std::string& err) {
  if (paths.empty()) return true;
  if (!db_.exec("BEGIN IMMEDIATE", err)) return false;
  for (const std::string& p : paths) {
    Stmt stmt(db_.handle(),
              "UPDATE dir_node SET deleted = ?, deleted_at = ? WHERE owner_id = ? AND path = ?",
              err);
    if (!stmt.ok() || !stmt.bind(1, trashed ? 1 : 0) || !stmt.bind(2, trashed ? nowMillis : 0) ||
        !stmt.bind(3, ownerId) || !stmt.bind(4, p) || stmt.step(err) != SQLITE_DONE) {
      err = err.empty() ? "update dir_node failed" : err;
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

bool FileRepository::deleteDirRows(std::int64_t ownerId, const std::vector<std::string>& paths,
                                   std::string& err) {
  if (paths.empty()) return true;
  if (!db_.exec("BEGIN IMMEDIATE", err)) return false;
  for (const std::string& p : paths) {
    Stmt stmt(db_.handle(), "DELETE FROM dir_node WHERE owner_id = ? AND path = ?", err);
    if (!stmt.ok() || !stmt.bind(1, ownerId) || !stmt.bind(2, p) ||
        stmt.step(err) != SQLITE_DONE) {
      err = err.empty() ? "delete dir_node failed" : err;
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

bool FileRepository::listTrashDirs(std::int64_t ownerId, std::vector<TrashDirRow>& out,
                                   std::string& err) {
  out.clear();
  Stmt stmt(db_.handle(),
            "SELECT path, deleted_at FROM dir_node WHERE owner_id = ? "
            "AND deleted = 1 AND deleted_at > 0 ORDER BY deleted_at DESC",
            err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, ownerId)) {
    err = "bind failed";
    return false;
  }
  while (true) {
    int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    TrashDirRow d;
    d.path = stmt.text(0);
    d.deletedAt = stmt.int64(1);
    out.push_back(d);
  }
  return true;
}

bool FileRepository::listChildDirPaths(std::int64_t ownerId, const std::string& parent,
                                       std::vector<std::string>& out, std::string& err) {
  out.clear();
  std::vector<std::string> all;
  if (!listDirs(ownerId, all, err)) return false;
  const std::string prefix = parent.empty() ? std::string() : parent + "/";
  for (const std::string& p : all) {
    if (p.size() <= prefix.size()) continue;
    if (p.compare(0, prefix.size(), prefix) != 0) continue;
    // 只取直接子目录：剩余部分不含 '/'
    if (p.find('/', prefix.size()) != std::string::npos) continue;
    out.push_back(p);
  }
  return true;
}

}  // namespace cv

#include "meta/db.h"

#include <filesystem>

#include "meta/schema.h"

namespace cv {
namespace fs = std::filesystem;

Db::~Db() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

bool Db::open(const std::string& path, std::string& err) {
  std::error_code ec;
  fs::path p(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  if (ec) {
    err = "create db dir failed: " + ec.message();
    return false;
  }
  if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
    err = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
    return false;
  }
  sqlite3_busy_timeout(db_, 5000);
  return true;
}

bool Db::initSchema(std::string& err) {
  // ⚠️ 顺序很关键：**先迁移既有表，再跑完整建表脚本**。
  // 反过来的话，在老库上会崩（真机实测 main.cpp:825「建表失败: no such column: owner_id」）：
  //   kSchemaSql 里的 `CREATE TABLE IF NOT EXISTS file_node` 在老库上是**空操作**
  //   （表已存在且没有 owner_id），紧接着 `CREATE INDEX ... ON file_node(owner_id, deleted)`
  //   就因列不存在而失败，initSchema 直接 return false，**迁移根本轮不到执行**。
  // 改为：migrateSchema 只负责"已存在但缺列"的表（老库），
  //       kSchemaSql 负责"建缺失的表 + 建索引"（新库直接建出含 owner_id 的结构）。
  if (!migrateSchema(db_, err)) return false;
  return exec(kSchemaSql, err);
}

namespace {

// PRAGMA table_info 探测某表是否含指定列（幂等迁移判据）
bool tableHasColumn(sqlite3* db, const std::string& table, const std::string& col,
                    std::string& err) {
  std::string sql = "PRAGMA table_info(" + table + ")";
  cv::Stmt stmt(db, sql, err);
  if (!stmt.ok()) return false;
  while (true) {
    int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;     // 游标结束
    if (rc != SQLITE_ROW) return false;  // 真错误
    if (stmt.text(1) == col) return true;  // PRAGMA table_info 第 2 列为列名
  }
  return false;
}

// 表是否存在（迁移用：**表不存在 ⇒ 交给建表脚本创建含 owner_id 的新结构，不迁移**）
bool tableExists(sqlite3* db, const std::string& table, std::string& err) {
  std::string sql = "SELECT name FROM sqlite_master WHERE type='table' AND name=?";
  cv::Stmt stmt(db, sql, err);
  if (!stmt.ok()) return false;
  if (!stmt.bind(1, table)) {
    err = "bind failed";
    return false;
  }
  const int rc = stmt.step(err);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  return false;
}

// 在事务内执行一批 SQL；任何一条失败回滚并返回 false。
bool execAll(sqlite3* db, const std::vector<std::string>& stmts, std::string& err) {
  if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
    err = "begin failed";
    return false;
  }
  for (const std::string& s : stmts) {
    char* msg = nullptr;
    if (sqlite3_exec(db, s.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
      err = msg ? msg : "sqlite exec failed";
      sqlite3_free(msg);
      sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      return false;
    }
  }
  if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
    err = "commit failed";
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    return false;
  }
  return true;
}

}  // namespace

bool migrateSchema(sqlite3* db, std::string& err) {
  if (!db) {
    err = "null db handle";
    return false;
  }

  // 1) file_node.owner_id（缺失才加；NOT NULL + DEFAULT 0 让老行自动归 0）
  if (tableExists(db, "file_node", err) && !tableHasColumn(db, "file_node", "owner_id", err)) {
    std::vector<std::string> stmts = {
        "ALTER TABLE file_node ADD COLUMN owner_id INTEGER NOT NULL DEFAULT 0",
        "CREATE INDEX IF NOT EXISTS idx_file_owner ON file_node(owner_id, deleted)",
    };
    if (!execAll(db, stmts, err)) return false;
  }
  // 索引幂等兜底（老库补上；新库由 kSchemaSql 创建）——仅在表存在时执行
  if (tableExists(db, "file_node", err)) {
    char* msg = nullptr;
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_file_owner ON file_node(owner_id, deleted)",
                 nullptr, nullptr, &msg);
    if (msg) sqlite3_free(msg);
  }

  // 1.5) shares.revoked（软撤销标记；老库补列，新库由 kSchemaSql 直接带上）
  if (tableExists(db, "shares", err) && !tableHasColumn(db, "shares", "revoked", err)) {
    std::vector<std::string> stmts = {
        "ALTER TABLE shares ADD COLUMN revoked INTEGER NOT NULL DEFAULT 0",
    };
    if (!execAll(db, stmts, err)) return false;
  }

  // 2) upload_session.owner_id
  if (tableExists(db, "upload_session", err) && !tableHasColumn(db, "upload_session", "owner_id", err)) {
    std::vector<std::string> stmts = {
        "ALTER TABLE upload_session ADD COLUMN owner_id INTEGER NOT NULL DEFAULT 0",
    };
    if (!execAll(db, stmts, err)) return false;
  }

  // 3) dir_node：重建为 PRIMARY KEY(owner_id, path)。
  //    SQLite 不能 ALTER 主键，必须"建新表→拷数据→删旧表→改名"。
  //    detection：旧表无 owner_id 列即视为待迁移。
  if (tableExists(db, "dir_node", err) && !tableHasColumn(db, "dir_node", "owner_id", err)) {
    std::vector<std::string> stmts = {
        "CREATE TABLE dir_node_new ("
        "owner_id INTEGER NOT NULL DEFAULT 0, "
        "path TEXT NOT NULL, "
        "created_at INTEGER NOT NULL, "
        "PRIMARY KEY (owner_id, path))",
        "INSERT OR IGNORE INTO dir_node_new (owner_id, path, created_at) "
        "SELECT 0, path, created_at FROM dir_node",
        "DROP TABLE dir_node",
        "ALTER TABLE dir_node_new RENAME TO dir_node",
    };
    if (!execAll(db, stmts, err)) return false;
  }

  // 4) schema_info.version 标记（幂等标记）。表可能还不存在（全新库）→ 跳过，由建表脚本创建。
  if (tableExists(db, "schema_info", err)) {
    char* msg = nullptr;
    sqlite3_exec(db,
                 "UPDATE schema_info SET version = 2 WHERE version < 2",
                 nullptr, nullptr, &msg);
    if (msg) sqlite3_free(msg);
    sqlite3_exec(db,
                 "INSERT INTO schema_info(version) SELECT 2 WHERE "
                 "(SELECT COUNT(*) FROM schema_info) = 0",
                 nullptr, nullptr, &msg);
    if (msg) sqlite3_free(msg);
  }

  return true;
}

bool Db::exec(const std::string& sql, std::string& err) {
  char* msg = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
    err = msg ? msg : "sqlite exec failed";
    sqlite3_free(msg);
    return false;
  }
  return true;
}

bool Db::query(const std::string& sql, std::vector<std::vector<std::string>>& rows,
               std::string& err) {
  rows.clear();
  Stmt stmt(db_, sql, err);
  if (!stmt.ok()) return false;
  while (true) {
    int rc = stmt.step(err);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) return false;
    std::vector<std::string> row;
    int cols = stmt.columnCount();
    for (int i = 0; i < cols; ++i) {
      row.push_back(stmt.isNull(i) ? std::string() : stmt.text(i));
    }
    rows.push_back(row);
  }
  return true;
}

std::int64_t Db::lastInsertId() const {
  return db_ ? static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_)) : 0;
}

Stmt::Stmt(sqlite3* db, const std::string& sql, std::string& err) {
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
    err = db ? sqlite3_errmsg(db) : "prepare failed";
    stmt_ = nullptr;
  }
}

Stmt::~Stmt() {
  if (stmt_) sqlite3_finalize(stmt_);
}

bool Stmt::bind(int index, const std::string& value) {
  return sqlite3_bind_text(stmt_, index, value.data(),
                           static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

bool Stmt::bind(int index, std::int64_t value) {
  return sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(value)) == SQLITE_OK;
}

int Stmt::step(std::string& err) {
  int rc = sqlite3_step(stmt_);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    err = sqlite3_errmsg(sqlite3_db_handle(stmt_));
    return -1;
  }
  return rc;
}

std::string Stmt::text(int col) const {
  const unsigned char* v = sqlite3_column_text(stmt_, col);
  return v ? reinterpret_cast<const char*>(v) : std::string();
}

std::int64_t Stmt::int64(int col) const {
  return static_cast<std::int64_t>(sqlite3_column_int64(stmt_, col));
}

bool Stmt::isNull(int col) const {
  return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

int Stmt::columnCount() const {
  return stmt_ ? sqlite3_column_count(stmt_) : 0;
}

}  // namespace cv

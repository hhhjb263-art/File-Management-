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

bool Db::initSchema(std::string& err) { return exec(kSchemaSql, err); }

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

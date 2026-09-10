#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <string>
#include <vector>

namespace cv {

// SQLite 薄封装：打开、建表、执行、查询、预编译语句 RAII。
class Db {
 public:
  Db() = default;
  ~Db();
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  bool open(const std::string& path, std::string& err);
  bool initSchema(std::string& err);
  bool exec(const std::string& sql, std::string& err);
  bool query(const std::string& sql, std::vector<std::vector<std::string>>& rows,
             std::string& err);

  sqlite3* handle() const { return db_; }
  std::int64_t lastInsertId() const;
  bool isOpen() const { return db_ != nullptr; }

 private:
  sqlite3* db_ = nullptr;
};

// 预编译语句 RAII。step() 返回 SQLITE_ROW / SQLITE_DONE，出错返回 -1。
class Stmt {
 public:
  Stmt(sqlite3* db, const std::string& sql, std::string& err);
  ~Stmt();
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  bool ok() const { return stmt_ != nullptr; }
  bool bind(int index, const std::string& value);
  bool bind(int index, std::int64_t value);
  int step(std::string& err);

  std::string text(int col) const;
  std::int64_t int64(int col) const;
  bool isNull(int col) const;
  int columnCount() const;

 private:
  sqlite3_stmt* stmt_ = nullptr;
};

}  // namespace cv

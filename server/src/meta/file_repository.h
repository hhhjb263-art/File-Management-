#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "meta/db.h"

namespace cv {

struct FileRow {
  std::int64_t id = 0;
  std::string name;
  std::int64_t size = 0;
  std::string contentHash;
  int chunkCount = 0;
  std::int64_t createdAt = 0;
  std::string dir;   // 所属目录（规范化相对路径，'' = 根目录）
};

// 文件元数据与分块清单的读写。所有操作都对 Db 句柄直接执行，
// 批量写入（insertFile）自带事务。
class FileRepository {
 public:
  explicit FileRepository(Db& db) : db_(db) {}

  // 秒传判定：同内容哈希是否已有文件
  bool findByContentHash(const std::string& hash, FileRow& out, std::string& err);
  bool findById(std::int64_t id, FileRow& out, std::string& err);
  bool listFiles(std::vector<FileRow>& out, std::string& err);
  bool chunkHashesOf(std::int64_t fileId, std::vector<std::string>& out, std::string& err);

  // 按虚拟路径（目录 + 文件名）查找已记录文件（按路径下载用）
  bool findByPath(const std::string& dir, const std::string& name, FileRow& out,
                  std::string& err);

  // 写入文件记录 + 所属目录 + 分块清单 + 分块引用计数（同一事务）
  bool insertFile(const std::string& name, const std::string& dir, std::int64_t size,
                  const std::string& contentHash,
                  const std::vector<std::string>& chunkHashes,
                  const std::vector<std::size_t>& chunkSizes, std::int64_t& id,
                  std::string& err);

  // 目录登记：不存在则插入（created=true），已存在则幂等（created=false）
  bool createDir(const std::string& path, bool& created, std::string& err);
  bool listDirs(std::vector<std::string>& out, std::string& err);

  // 全部目录路径（已登记目录 dir_node + 文件所属目录 file_dir 及其所有祖先，
  // 含根 ''），用于文件树建树。祖先展开在 C++ 侧完成，不写递归 SQL。
  bool listDirsAll(std::vector<std::string>& out, std::string& err);

  bool addChunkIfAbsent(const std::string& hash, std::int64_t size, std::string& err);

 private:
  Db& db_;
};

std::int64_t nowMillis();

}  // namespace cv

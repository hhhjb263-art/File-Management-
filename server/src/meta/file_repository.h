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

  // 同目录下是否已存在同名文件（excludeId > 0 时排除自身，用于重命名自检）
  bool nameExists(const std::string& dir, const std::string& name,
                  std::int64_t excludeId, bool& exists, std::string& err);

  // 重命名（只改 file_node.name；物理镜像由调用方处理）
  bool renameFile(std::int64_t id, const std::string& newName, std::string& err);

  // 删除：置 deleted=1、删除 file_chunk 链接、递减分块引用计数，并清掉引用计数归零的
  // 分块行——它们的 hash 通过 orphanHashes 返回，由调用方删除 blob（真正释放磁盘空间）。
  bool softDelete(std::int64_t id, std::vector<std::string>& orphanHashes, std::string& err);

  // 列出/删除引用计数归零的分块行（GC 用：回收历史残留）
  bool listOrphanChunkHashes(std::vector<std::string>& out, std::string& err);
  bool deleteChunkRows(const std::vector<std::string>& hashes, std::string& err);

  // 覆盖内容：保留 id 与名称，替换 content_hash/size 与分块清单（引用计数同步增减）；
  // 旧内容中引用计数归零的分块 hash 通过 orphanHashes 返回，由调用方删 blob。
  bool replaceContent(std::int64_t id, std::int64_t size, const std::string& contentHash,
                      const std::vector<std::string>& chunkHashes,
                      const std::vector<std::size_t>& chunkSizes,
                      std::vector<std::string>& orphanHashes, std::string& err);

  // 全部目录路径（已登记目录 dir_node + 文件所属目录 file_dir 及其所有祖先，
  // 含根 ''），用于文件树建树。祖先展开在 C++ 侧完成，不写递归 SQL。
  bool listDirsAll(std::vector<std::string>& out, std::string& err);

  bool addChunkIfAbsent(const std::string& hash, std::int64_t size, std::string& err);

 private:
  // 清理 ref_count <= 0 的分块行，返回其 hash（调用方删 blob）
  bool purgeZeroRefChunks(std::vector<std::string>& orphans, std::string& err);

  Db& db_;
};

std::int64_t nowMillis();

}  // namespace cv

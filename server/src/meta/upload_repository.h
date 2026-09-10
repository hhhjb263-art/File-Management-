#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "meta/db.h"

namespace cv {

// 分块上传会话（断点续传的载体，DB 为真相源）
struct UploadSession {
  std::int64_t id = 0;          // 即 upload_id
  std::string name;             // 文件名（已做防穿越清洗）
  std::int64_t size = 0;        // 整文件字节数
  std::int64_t chunkSize = 0;   // 分块大小（字节）
  std::string fileHash;         // 客户端提供的整文件 SHA-256，可为空
  std::string status;           // created | uploading | completed | aborted
  std::int64_t createdAt = 0;   // 毫秒
  std::int64_t updatedAt = 0;   // 毫秒
};

// 会话已收分块记录
struct UploadChunkRow {
  std::int64_t seq = 0;
  std::int64_t size = 0;
  std::string sha256;
  std::int64_t createdAt = 0;
};

// 分块上传会话的读写。所有操作直接作用于 Db 句柄；
// 临时分块文件（磁盘）由调用方（main）负责，本类只管元数据。
class UploadRepository {
 public:
  explicit UploadRepository(Db& db) : db_(db) {}

  // 新建会话，返回自增 upload_id
  bool create(const std::string& name, std::int64_t size, std::int64_t chunkSize,
              const std::string& fileHash, std::int64_t& id, std::string& err);

  bool findById(std::int64_t id, UploadSession& out, std::string& err);
  bool findChunk(std::int64_t id, std::int64_t seq, UploadChunkRow& out, bool& found,
                 std::string& err);

  // 复用：同 (file_hash,size,chunk_size) 且未结束（非 completed/aborted）的会话
  bool findResumable(const std::string& fileHash, std::int64_t size,
                     std::int64_t chunkSize, UploadSession& out, std::string& err);

  // 列出某会话全部分块（按 seq 升序）
  bool listChunks(std::int64_t id, std::vector<UploadChunkRow>& out, std::string& err);

  // 写入/覆盖分块（PRIMARY KEY 冲突即覆盖，幂等）
  bool putChunk(std::int64_t id, std::int64_t seq, std::int64_t size,
                const std::string& sha256, std::string& err);

  // 删除某会话的全部分块记录
  bool deleteChunks(std::int64_t id, std::string& err);

  // 删除某会话的单个分块记录（自愈用）
  bool deleteChunk(std::int64_t id, std::int64_t seq, std::string& err);

  // 更新状态与 updated_at
  bool setStatus(std::int64_t id, const std::string& status, std::string& err);

  // 会话目标目录（init 登记，complete 取用；不存在的会话返回空串）
  bool setDir(std::int64_t id, const std::string& dir, std::string& err);
  bool getDir(std::int64_t id, std::string& out, std::string& err);

  // 删除会话及其全部分块记录（取消 / GC 用）
  bool removeSession(std::int64_t id, std::string& err);

  // 列出闲置超时的未完成会话 id（updated_at 早于 now-thresholdMs）
  bool listStaleIds(std::int64_t thresholdMs, std::vector<std::int64_t>& out,
                    std::string& err);

 private:
  bool rowToSession(Stmt& stmt, UploadSession& out);

  Db& db_;
};

}  // namespace cv

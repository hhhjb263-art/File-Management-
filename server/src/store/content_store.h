#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace cv {

// 内容寻址存储：以 SHA-256 十六进制为键落盘，天然去重（秒传的基础）。
// 目录布局：<root>/<hash 前 2 位>/<hash>
class ContentStore {
 public:
  explicit ContentStore(std::string root);

  bool init(std::string& err);
  const std::string& root() const { return root_; }

  // 必须是 64 位十六进制，防止路径穿越。
  static bool validHash(const std::string& hex);

  std::string pathOf(const std::string& hex) const;
  bool exists(const std::string& hex) const;

  // 已存在则直接成功（去重）；否则先写临时文件再 rename，保证原子性。
  bool put(const std::string& hex, const std::string& data, std::string& err);
  bool get(const std::string& hex, std::string& out, std::string& err) const;
  bool drop(const std::string& hex, std::string& err);

  // 按 chunkSize 切块写入，返回分块哈希序列与整文件哈希
  // （整文件哈希 = 各分块哈希拼接后的 SHA-256）。
  bool putChunked(const std::string& data, std::size_t chunkSize,
                  std::vector<std::string>& chunkHashes,
                  std::vector<std::size_t>& chunkSizes, std::string& fileHash,
                  std::string& err);
  bool getChunked(const std::vector<std::string>& chunkHashes, std::string& out,
                  std::string& err) const;

 private:
  std::string root_;
};

}  // namespace cv

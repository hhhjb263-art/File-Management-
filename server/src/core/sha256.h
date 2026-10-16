#pragma once

#include <cstddef>
#include <string>

namespace cv {

// 零依赖 SHA-256（FIPS 180-4）。用于内容寻址：文件/分块的内容哈希即存储键。
class Sha256 {
 public:
  Sha256();

  void update(const void* data, std::size_t len);
  void update(const std::string& data);

  // 完成计算并返回 64 位小写十六进制；可重复调用，结果缓存。
  std::string hex();

  // 原始 32 字节摘要。**PBKDF2 的 HMAC 内部必须用原始字节**：
  // 以前只能 `hexToBytes(hex())` 转回来，21 万轮里每轮白做 4 次 hex↔bytes 转换。
  std::string bytes();

  static std::string of(const void* data, std::size_t len);
  static std::string of(const std::string& data);

 private:
  void reset();
  void compress(const unsigned char* block);
  void finalize();

  unsigned int state_[8];
  unsigned long long bitLen_;
  unsigned char buf_[64];
  std::size_t bufLen_;
  bool done_;
  std::string cached_;
};

}  // namespace cv

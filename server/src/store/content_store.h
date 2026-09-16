#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cv {

// 内容寻址存储：以 SHA-256 十六进制为键落盘，天然去重（秒传的基础）。
// 目录布局：<root>/<hash 前 2 位>/<hash>
//
// 静态数据加密（可选，需编译期启用 OpenSSL / CV_HAVE_OPENSSL）：
//   启用后 blob 文件落盘加密（AES-256-GCM，每 blob 独立随机 nonce）；
//   为与明文 blob 区分，落盘文件名追加 .enc 后缀：<hash>.enc（内容仍按 sha256 去重/秒传）。
//   blob 磁盘格式：magic(4 字节 "CVB1") + nonce(12) + tag(16) + ciphertext。
//   CVB1 仅作格式标记，不做内容嗅探——是否解密完全由文件名 .enc 后缀决定，
//   杜绝“伪装明文”绕过加密完整性校验。
//   向后兼容：加密开启时读取先找 <hash>.enc，找不到再回退同名明文 <hash>（支持从明文库平滑迁移）。
class ContentStore {
 public:
  explicit ContentStore(std::string root);

  bool init(std::string& err);
  const std::string& root() const { return root_; }

  // 必须是 64 位十六进制，防止路径穿越。
  static bool validHash(const std::string& hex);

  // 返回「首选」落盘路径：加密开启时为 <hash>.enc，否则为明文 <hash>。用于写入。
  std::string pathOf(const std::string& hex) const;
  // 返回「实际存在」的 blob 路径：加密时优先 <hash>.enc，否则回退同名明文 <hash>。用于读取/存在性/删除。
  std::string resolvePath(const std::string& hex) const;
  bool exists(const std::string& hex) const;

  // 启用静态加密：key 必须为 32 字节（来自 --data-key 文件）。
  // 返回 false（并填充 err）当：未编译 OpenSSL、key 长度非 32 字节。
  bool setDataKey(const std::vector<unsigned char>& key, std::string& err);
  bool encryptionEnabled() const { return encEnabled_; }

  // 已存在则直接成功（去重）；否则先写临时文件再 rename，保证原子性。
  bool put(const std::string& hex, const std::string& data, std::string& err);
  bool get(const std::string& hex, std::string& out, std::string& err) const;
  bool drop(const std::string& hex, std::string& err);

  // 按 chunkSize 切块写入，返回分块哈希序列与整文件哈希
  // （整文件哈希 = 原始字节的 SHA-256，与 file_node.content_hash 一致，用于秒传判重）。
  bool putChunked(const std::string& data, std::size_t chunkSize,
                  std::vector<std::string>& chunkHashes,
                  std::vector<std::size_t>& chunkSizes, std::string& fileHash,
                  std::string& err);
  bool getChunked(const std::vector<std::string>& chunkHashes, std::string& out,
                  std::string& err) const;

  // 仅提取 [offset, offset+length) 区间字节（按分块读取，内存占用只约一个分块大小）。
  // 用于下载端到端 Range/206，避免整文件入内存。
  bool readRange(const std::vector<std::string>& chunkHashes, std::int64_t offset,
                 std::int64_t length, std::string& out, std::string& err) const;

  // 按分块序列把内容拼接写入 targetAbs（先写 .tmp 再 rename）。
  // ⚠️ 内容寻址存储里只有「分块 blob」没有「整文件 blob」，所以镜像必须走本方法；
  // 经 get() 读取会自动解密（加密开启时），故镜像即明文拼接结果。
  // 传空序列即创建一个 0 字节文件。内存占用只约一个分块。
  bool materializeFromChunks(const std::vector<std::string>& chunkHashes,
                            const std::string& targetAbs, std::string& err);

  // 把已有文件「搬移」进内容库（同盘 rename，零拷贝；跨设备退回复制后删源）。
  // 用于分块上传 complete：避免 tmp 分块与 blob 同时占盘（峰值从 2× 降到 1×）。
  // 目标 blob 已存在（内容已去重）时直接返回 true，源文件由调用方清理。
  bool putFromFile(const std::string& hex, const std::string& srcPath, std::string& err);

 private:
  std::string root_;
  std::vector<unsigned char> key_;   // 32 字节静态加密密钥（仅加密开启时有效）
  bool encEnabled_ = false;          // 静态加密开关
};

}  // namespace cv

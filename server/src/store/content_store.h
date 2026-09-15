#pragma once

#include <cstddef>
#include <cstdint>
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

  // 把 blob 以硬链接（失败则复制）镜像到 targetAbs（文件树下载语义的物理载体）。
  // 父目录由调用方创建并完成越界校验；本方法只保证 blob 存在与落位。
  bool materialize(const std::string& hex, const std::string& targetAbs, std::string& err);

  // 按分块序列把内容拼接写入 targetAbs（先写 .tmp 再 rename）。
  // ⚠️ 内容寻址存储里只有「分块 blob」没有「整文件 blob」，所以镜像必须走本方法；
  // 传空序列即创建一个 0 字节文件。内存占用只约一个分块。
  bool materializeFromChunks(const std::vector<std::string>& chunkHashes,
                            const std::string& targetAbs, std::string& err);

  // 把已有文件「搬移」进内容库（同盘 rename，零拷贝；跨设备退回复制后删源）。
  // 用于分块上传 complete：避免 tmp 分块与 blob 同时占盘（峰值从 2× 降到 1×）。
  // 目标 blob 已存在（内容已去重）时直接返回 true，源文件由调用方清理。
  bool putFromFile(const std::string& hex, const std::string& srcPath, std::string& err);

  // 分块拼接镜像：把各分块 blob 依次流式写入 targetAbs（先写 .tmp 再原子 rename）。
  // 用于分块上传的文件——内容库里只有分块 blob，不存在整文件 blob，
  // 故不能用 materialize(整文件哈希)。内存峰值 ≈ 1 个分块。
  bool materializeChunked(const std::vector<std::string>& chunkHashes,
                          const std::string& targetAbs, std::string& err);

 private:
  std::string root_;
};

}  // namespace cv

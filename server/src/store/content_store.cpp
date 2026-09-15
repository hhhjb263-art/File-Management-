#include "store/content_store.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "core/sha256.h"

namespace cv {
namespace fs = std::filesystem;

namespace {

bool writeFileAtomic(const std::string& finalPath, const std::string& data,
                     std::string& err) {
  std::string tmp = finalPath + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      err = "cannot write " + tmp + ": " + std::strerror(errno);
      return false;
    }
    if (!data.empty()) out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out.good()) {
      err = "write failed: " + tmp;
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, finalPath, ec);
  if (ec) {
    fs::remove(tmp, ec);
    err = "rename failed: " + ec.message();
    return false;
  }
  return true;
}

bool readFileAll(const std::string& path, std::string& out, std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    err = "cannot open " + path + ": " + std::strerror(errno);
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

}  // namespace

ContentStore::ContentStore(std::string root) : root_(std::move(root)) {}

bool ContentStore::validHash(const std::string& hex) {
  if (hex.size() != 64) return false;
  for (char c : hex) {
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!ok) return false;
  }
  return true;
}

std::string ContentStore::pathOf(const std::string& hex) const {
  return root_ + "/" + hex.substr(0, 2) + "/" + hex;
}

bool ContentStore::init(std::string& err) {
  std::error_code ec;
  fs::create_directories(root_, ec);
  if (ec) {
    err = "create blob root failed: " + ec.message();
    return false;
  }
  return true;
}

bool ContentStore::exists(const std::string& hex) const {
  if (!validHash(hex)) return false;
  std::error_code ec;
  return fs::exists(pathOf(hex), ec);
}

bool ContentStore::put(const std::string& hex, const std::string& data, std::string& err) {
  if (!validHash(hex)) {
    err = "invalid content hash";
    return false;
  }
  if (exists(hex)) return true;  // 去重：内容相同直接成功

  std::string dir = root_ + "/" + hex.substr(0, 2);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    err = "create dir failed: " + ec.message();
    return false;
  }
  return writeFileAtomic(pathOf(hex), data, err);
}

bool ContentStore::get(const std::string& hex, std::string& out, std::string& err) const {
  if (!validHash(hex)) {
    err = "invalid content hash";
    return false;
  }
  return readFileAll(pathOf(hex), out, err);
}

bool ContentStore::drop(const std::string& hex, std::string& err) {
  if (!validHash(hex)) {
    err = "invalid content hash";
    return false;
  }
  std::error_code ec;
  bool removed = fs::remove(pathOf(hex), ec);
  if (!removed) {
    err = "remove failed: " + ec.message();
    return false;
  }
  return true;
}

bool ContentStore::putChunked(const std::string& data, std::size_t chunkSize,
                              std::vector<std::string>& chunkHashes,
                              std::vector<std::size_t>& chunkSizes,
                              std::string& fileHash, std::string& err) {
  if (chunkSize == 0) {
    err = "invalid chunk size";
    return false;
  }
  chunkHashes.clear();
  chunkSizes.clear();
  std::string concat;
  std::size_t offset = 0;
  if (data.empty()) {
    std::string h = Sha256::of("", 0);
    if (!put(h, "", err)) return false;
    chunkHashes.push_back(h);
    chunkSizes.push_back(0);
    concat += h;
  }
  while (offset < data.size()) {
    std::size_t n = std::min(chunkSize, data.size() - offset);
    std::string h = Sha256::of(data.data() + offset, n);
    if (!put(h, std::string(data.data() + offset, n), err)) return false;
    chunkHashes.push_back(h);
    chunkSizes.push_back(n);
    concat += h;
    offset += n;
  }
  fileHash = Sha256::of(concat);
  return true;
}

bool ContentStore::getChunked(const std::vector<std::string>& chunkHashes,
                              std::string& out, std::string& err) const {
  out.clear();
  for (const std::string& h : chunkHashes) {
    std::string part;
    if (!get(h, part, err)) return false;
    out += part;
  }
  return true;
}

bool ContentStore::readRange(const std::vector<std::string>& chunkHashes,
                             std::int64_t offset, std::int64_t length, std::string& out,
                             std::string& err) const {
  out.clear();
  if (offset < 0 || length <= 0) return true;  // 空区间：直接返回空
  std::int64_t pos = 0;
  std::int64_t wantEnd = offset + length;       // 半开区间终点
  for (const std::string& h : chunkHashes) {
    std::string part;
    if (!get(h, part, err)) return false;
    const std::int64_t psz = static_cast<std::int64_t>(part.size());
    // 计算本分块与 [offset, wantEnd) 的重叠区间
    const std::int64_t s = std::max<std::int64_t>(0, offset - pos);
    const std::int64_t e = std::min<std::int64_t>(psz, wantEnd - pos);
    if (s < e) out.append(part, static_cast<std::size_t>(s),
                          static_cast<std::size_t>(e - s));
    pos += psz;
    if (pos >= wantEnd) break;                  // 已满足请求区间，提前结束
  }
  return true;
}

bool ContentStore::materialize(const std::string& hex, const std::string& targetAbs,
                               std::string& err) {
  if (!validHash(hex)) {
    err = "invalid hash";
    return false;
  }
  std::error_code ec;
  fs::path src = fs::path(pathOf(hex));
  if (!fs::exists(src, ec)) {
    err = "blob missing: " + hex;
    return false;
  }
  fs::path dst = fs::path(targetAbs);
  // 覆盖旧镜像（重传同名文件时保持指向最新内容）
  fs::remove(dst, ec);
  // 优先硬链接：同一数据目录内零拷贝；跨设备等场景退回复制
  ec.clear();
  fs::create_hard_link(src, dst, ec);
  if (ec) {
    std::error_code cec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, cec);
    if (cec) {
      err = "materialize failed: link=" + ec.message() + " copy=" + cec.message();
      return false;
    }
  }
  return true;
}

bool ContentStore::materializeChunked(const std::vector<std::string>& chunkHashes,
                                      const std::string& targetAbs, std::string& err) {
  for (const std::string& h : chunkHashes) {
    if (!validHash(h)) {
      err = "invalid chunk hash";
      return false;
    }
    if (!exists(h)) {
      err = "blob missing: " + h;
      return false;
    }
  }
  std::error_code ec;
  fs::path dst = fs::path(targetAbs);
  fs::path tmp = dst;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      err = "cannot open tmp for write: " + tmp.string();
      return false;
    }
    for (const std::string& h : chunkHashes) {
      std::ifstream in(pathOf(h), std::ios::binary);
      if (!in.is_open()) {
        err = "cannot open blob: " + h;
        fs::remove(tmp, ec);
        return false;
      }
      out << in.rdbuf();   // 逐块流式拼接：峰值 ≈ 1 个分块，不整文件入内存
      if (!out.good()) {
        err = "write failed: " + tmp.string();
        fs::remove(tmp, ec);
        return false;
      }
    }
    out.flush();
    if (!out.good()) {
      err = "flush failed: " + tmp.string();
      fs::remove(tmp, ec);
      return false;
    }
  }
  fs::remove(dst, ec);
  fs::rename(tmp, dst, ec);
  if (ec) {
    fs::remove(tmp, ec);
    err = "rename failed: " + ec.message();
    return false;
  }
  return true;
}

bool ContentStore::materializeFromChunks(const std::vector<std::string>& chunkHashes,
                                        const std::string& targetAbs, std::string& err) {
  if (targetAbs.empty()) {
    err = "empty target path";
    return false;
  }
  const std::string tmp = targetAbs + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      err = "cannot open tmp for write: " + tmp;
      return false;
    }
    // 逐块读入写出：任何时刻内存里只有一个分块
    for (const std::string& h : chunkHashes) {
      if (!validHash(h)) {
        out.close();
        std::error_code rec;
        fs::remove(tmp, rec);
        err = "invalid chunk hash";
        return false;
      }
      std::ifstream in(pathOf(h), std::ios::binary);
      if (!in.is_open()) {
        out.close();
        std::error_code rec;
        fs::remove(tmp, rec);
        err = "chunk blob missing: " + h;
        return false;
      }
      out << in.rdbuf();
      if (!out.good()) {
        out.close();
        std::error_code rec;
        fs::remove(tmp, rec);
        err = "write failed: " + tmp;
        return false;
      }
    }
    out.flush();
    if (!out.good()) {
      out.close();
      std::error_code rec;
      fs::remove(tmp, rec);
      err = "flush failed: " + tmp;
      return false;
    }
  }
  std::error_code ec;
  fs::remove(targetAbs, ec);
  fs::rename(tmp, targetAbs, ec);
  if (ec) {
    std::error_code rec;
    fs::remove(tmp, rec);
    err = "rename failed: " + ec.message();
    return false;
  }
  return true;
}

bool ContentStore::putFromFile(const std::string& hex, const std::string& srcPath,
                               std::string& err) {
  if (!validHash(hex)) {
    err = "invalid hash";
    return false;
  }
  if (exists(hex)) {
    return true;   // 内容已去重命中：源文件由调用方清理
  }
  std::error_code ec;
  fs::path dst = fs::path(pathOf(hex));
  fs::create_directories(dst.parent_path(), ec);
  if (ec) {
    err = "create blob dir failed: " + ec.message();
    return false;
  }
  // 同盘 rename = 零拷贝搬移（tmp → blobs），避免 tmp 与 blob 双份占盘
  ec.clear();
  fs::rename(fs::path(srcPath), dst, ec);
  if (!ec) {
    return true;
  }
  // 跨设备等场景：复制后删源
  std::error_code cec;
  fs::copy_file(fs::path(srcPath), dst, fs::copy_options::overwrite_existing, cec);
  if (cec) {
    err = "move/copy failed: " + cec.message();
    return false;
  }
  fs::remove(fs::path(srcPath), cec);
  return true;
}

}  // namespace cv

#include "store/content_store.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef CV_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

#include "core/sha256.h"

namespace cv {
namespace fs = std::filesystem;

namespace {

// 加密 blob 磁盘格式：magic(4) + nonce(12) + tag(16) + ciphertext
constexpr char kBlobMagic[4] = {'C', 'V', 'B', '1'};
constexpr std::size_t kNonceLen = 12;
constexpr std::size_t kTagLen = 16;

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

#ifdef CV_HAVE_OPENSSL

// AES-256-GCM 加密：明文 → magic + nonce(12) + tag(16) + ciphertext。
// 每个 blob 使用独立随机 nonce；tag 用于解密时验证完整性与真实性。
bool evpEncrypt(const std::vector<unsigned char>& key, const std::string& plaintext,
                std::string& blob, std::string& err) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    err = "EVP_CIPHER_CTX_new failed";
    return false;
  }
  std::vector<unsigned char> nonce(kNonceLen, 0);
  if (RAND_bytes(nonce.data(), static_cast<int>(kNonceLen)) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    err = "RAND_bytes failed";
    return false;
  }
  if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceLen), nullptr) != 1 ||
      EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    err = "EVP init failed";
    return false;
  }
  std::string ct;
  ct.reserve(plaintext.size());
  int len = 0;
  if (!plaintext.empty()) {
    ct.resize(plaintext.size());
    if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(&ct[0]), &len,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1) {
      EVP_CIPHER_CTX_free(ctx);
      err = "EVP encrypt failed";
      return false;
    }
    ct.resize(static_cast<std::size_t>(len));
  }
  if (EVP_EncryptFinal_ex(ctx, nullptr, &len) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    err = "EVP final failed";
    return false;
  }
  std::vector<unsigned char> tag(kTagLen, 0);
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagLen), tag.data()) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    err = "EVP get tag failed";
    return false;
  }
  EVP_CIPHER_CTX_free(ctx);

  blob.clear();
  blob.reserve(4 + kNonceLen + kTagLen + ct.size());
  blob.append(kBlobMagic, 4);
  blob.append(reinterpret_cast<const char*>(nonce.data()), kNonceLen);
  blob.append(reinterpret_cast<const char*>(tag.data()), kTagLen);
  blob.append(ct);
  return true;
}

// AES-256-GCM 解密：magic + nonce(12) + tag(16) + ciphertext → 明文。
// tag 校验失败（数据损坏或密钥错误）返回 false。
bool evpDecrypt(const std::vector<unsigned char>& key, const std::string& blob,
                std::string& out, std::string& err) {
  if (blob.size() < 4 + kNonceLen + kTagLen) {
    err = "blob too short to be encrypted";
    return false;
  }
  const unsigned char* p = reinterpret_cast<const unsigned char*>(blob.data());
  if (std::memcmp(p, kBlobMagic, 4) != 0) {
    err = "bad blob magic";
    return false;
  }
  const unsigned char* nonce = p + 4;
  const unsigned char* tag = p + 4 + kNonceLen;
  const unsigned char* ct = p + 4 + kNonceLen + kTagLen;
  const std::size_t ctLen = blob.size() - (4 + kNonceLen + kTagLen);

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    err = "EVP_CIPHER_CTX_new failed";
    return false;
  }
  if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceLen), nullptr) != 1 ||
      EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    err = "EVP init failed";
    return false;
  }
  std::string pt;
  pt.resize(ctLen);
  int len = 0;
  if (ctLen > 0) {
    if (EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(&pt[0]), &len, ct,
                          static_cast<int>(ctLen)) != 1) {
      EVP_CIPHER_CTX_free(ctx);
      err = "EVP decrypt failed";
      return false;
    }
    pt.resize(static_cast<std::size_t>(len));
  }
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagLen),
                          const_cast<unsigned char*>(tag)) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    err = "EVP set tag failed";
    return false;
  }
  if (EVP_DecryptFinal_ex(ctx, nullptr, &len) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    err = "EVP tag verify failed (data corrupted or wrong key)";
    return false;
  }
  EVP_CIPHER_CTX_free(ctx);
  out = std::move(pt);
  return true;
}

#endif  // CV_HAVE_OPENSSL

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
  // 加密开启时落盘文件名追加 .enc 后缀，与明文 blob 物理区分（内容仍按 sha256 去重）。
  return root_ + "/" + hex.substr(0, 2) + "/" + hex + (encEnabled_ ? ".enc" : "");
}

std::string ContentStore::resolvePath(const std::string& hex) const {
  // 读取/存在性/删除时实际落盘路径：加密优先 <hash>.enc，否则回退同名明文 <hash>，
  // 以便从明文库平滑迁移（旧 blob 仍为明文，新 blob 为 .enc）。两者皆无则返回首选路径。
  std::error_code ec;
  const std::string enc = pathOf(hex);             // 加密时即 <hash>.enc，否则明文
  if (fs::exists(enc, ec)) return enc;
  const std::string plain = root_ + "/" + hex.substr(0, 2) + "/" + hex;
  if (fs::exists(plain, ec)) return plain;
  return enc;
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

bool ContentStore::setDataKey(const std::vector<unsigned char>& key, std::string& err) {
#ifdef CV_HAVE_OPENSSL
  if (key.size() != 32) {
    err = "data key must be 32 bytes (256 bits) for AES-256-GCM";
    return false;
  }
  key_ = key;
  encEnabled_ = true;
  return true;
#else
  (void)key;
  err = "static data encryption requires a TLS-capable build (CV_HAVE_OPENSSL); "
        "rebuild with CV_ENABLE_TLS=ON and OpenSSL installed";
  return false;
#endif
}

bool ContentStore::exists(const std::string& hex) const {
  if (!validHash(hex)) return false;
  std::error_code ec;
  return fs::exists(resolvePath(hex), ec);
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

#ifdef CV_HAVE_OPENSSL
  if (encEnabled_) {
    // 加密开启：内容用 AES-256-GCM 加密后落盘；落盘文件名追加 .enc 后缀（内容仍按 sha256 去重/秒传）。
    std::string blob;
    if (!evpEncrypt(key_, data, blob, err)) return false;
    return writeFileAtomic(pathOf(hex), blob, err);
  }
#endif
  return writeFileAtomic(pathOf(hex), data, err);
}

bool ContentStore::get(const std::string& hex, std::string& out, std::string& err) const {
  if (!validHash(hex)) {
    err = "invalid content hash";
    return false;
  }
  const std::string p = resolvePath(hex);
  std::string raw;
  if (!readFileAll(p, raw, err)) return false;
  // 是否解密完全由文件名 .enc 后缀决定（不做内容嗅探，避免伪造明文绕过加密完整性校验）。
  const bool isEncrypted = (p.size() >= 4 && p.compare(p.size() - 4, 4, ".enc") == 0);
#ifdef CV_HAVE_OPENSSL
  if (isEncrypted) {
    if (!evpDecrypt(key_, raw, out, err)) {
      err = "decrypt blob failed (" + hex + "): " + err;
      return false;
    }
    return true;
  }
#else
  // 未编译 OpenSSL 的二进制读不了密文：干净报错，绝不把密文当内容返回
  if (isEncrypted) {
    err = "blob is encrypted but this binary was built without OpenSSL support";
    return false;
  }
#endif
  out = raw;
  return true;
}

bool ContentStore::drop(const std::string& hex, std::string& err) {
  if (!validHash(hex)) {
    err = "invalid content hash";
    return false;
  }
  std::error_code ec;
  // 同时尝试 .enc（加密）与同名明文（迁移前遗留），两者皆删，避免漏删 .enc 造成孤立 blob。
  const std::string enc = root_ + "/" + hex.substr(0, 2) + "/" + hex + ".enc";
  const std::string plain = root_ + "/" + hex.substr(0, 2) + "/" + hex;
  const bool removedEnc = fs::remove(enc, ec);
  const bool removedPlain = fs::remove(plain, ec);
  if (!removedEnc && !removedPlain) {
    err = "blob not found: " + hex;
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
    // 逐块读入写出：任何时刻内存里只有一个分块。
    // 经 get() 读取会自动解密（加密开启时），故镜像即明文拼接结果。
    for (const std::string& h : chunkHashes) {
      if (!validHash(h)) {
        out.close();
        std::error_code rec;
        fs::remove(tmp, rec);
        err = "invalid chunk hash";
        return false;
      }
      std::string part;
      if (!get(h, part, err)) {
        out.close();
        std::error_code rec;
        fs::remove(tmp, rec);
        return false;
      }
      if (!part.empty()) {
        out.write(part.data(), static_cast<std::streamsize>(part.size()));
      }
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

#ifdef CV_HAVE_OPENSSL
  if (encEnabled_) {
    // 加密开启：零拷贝搬移失效，必须先读明文再加密落盘。源文件稍后清理。
    std::string data;
    if (!readFileAll(srcPath, data, err)) return false;
    std::string blob;
    if (!evpEncrypt(key_, data, blob, err)) return false;
    if (!writeFileAtomic(dst.string(), blob, err)) return false;
    std::error_code rec;
    fs::remove(fs::path(srcPath), rec);
    return true;
  }
#endif

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

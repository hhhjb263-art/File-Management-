#include "core/pbkdf2.h"

#include "core/sha256.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cv {
namespace pbkdf2 {
namespace {

constexpr int kSha256Block = 64;   // HMAC-SHA256 的分组大小（字节）
constexpr int kSha256Out = 32;     // SHA-256 输出长度（字节）

// 十六进制字符 → 数值（非法字符返回 -1）
int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 十六进制串 → 原始字节（长度应为偶数；末尾单字符忽略）
std::string hexToBytes(const std::string& hex) {
  std::string out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    int hi = hexVal(hex[i]);
    int lo = hexVal(hex[i + 1]);
    if (hi < 0 || lo < 0) break;  // 防御：salt 必为合法 hex，异常输入直接截断
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

std::string bytesToHex(const std::string& bytes) {
  static const char* h = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out.push_back(h[c >> 4]);
    out.push_back(h[c & 0xF]);
  }
  return out;
}

// 原始 32 字节 SHA-256（不经 hex）
std::string rawSha256(const std::string& data) {
  Sha256 s;
  s.update(data);
  return s.bytes();
}

// 预计算的 HMAC-SHA256 上下文：raw（字节）进、raw（32 字节）出。
//
// ⚠️ 为什么要这样写（性能，不改语义）：
//   PBKDF2 对**同一个密码**要做 21 万次 HMAC。ipad/opad 只由密码派生，
//   原先每一轮都重新算一遍，而且每轮 2 次哈希都走 `Sha256::of()`（返回 **hex 字符串**）
//   再用 `hexToBytes()` 转回原始字节 —— 一轮下来 4 次 hex↔bytes + 6 次以上字符串分配。
//   这里把 ipad/opad 提到构造时算一次，全程用原始 32 字节，**输出与原先逐字节相同**
//   （由 lead_pbkdf2_harness 的 6 条公开标准向量守住）。
struct HmacSha256 {
  std::string ipad;
  std::string opad;

  explicit HmacSha256(const std::string& key) {
    std::string k = key;
    // 密钥超一个分组：先压成 32 字节原始摘要（注意是**原始字节**，不是 hex）
    if (k.size() > kSha256Block) k = rawSha256(k);
    k.resize(kSha256Block, '\0');
    ipad.assign(kSha256Block, '\0');
    opad.assign(kSha256Block, '\0');
    for (int i = 0; i < kSha256Block; ++i) {
      const unsigned char c = static_cast<unsigned char>(k[i]);
      ipad[i] = static_cast<char>(c ^ 0x36);
      opad[i] = static_cast<char>(c ^ 0x5c);
    }
  }

  std::string operator()(const std::string& msg) const {
    std::string inner = ipad;
    inner += msg;
    std::string outer = opad;
    outer += rawSha256(inner);
    return rawSha256(outer);
  }
};

// 32 位大端整数追加到字符串（PBKDF2 的块序号 1..N）
void appendInt32Be(std::uint32_t v, std::string& out) {
  out.push_back(static_cast<char>((v >> 24) & 0xFF));
  out.push_back(static_cast<char>((v >> 16) & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
  out.push_back(static_cast<char>(v & 0xFF));
}

}  // namespace

std::string deriveHex(const std::string& password, const std::string& saltHex,
                      int iterations, int dkLenBytes) {
  if (iterations < 1 || dkLenBytes < 1) return std::string();
  std::string salt = hexToBytes(saltHex);

  const HmacSha256 hmac(password);   // ← 只算一次 ipad/opad
  std::string dk;
  dk.reserve(dkLenBytes);
  const int blocks = (dkLenBytes + kSha256Out - 1) / kSha256Out;
  for (int block = 1; block <= blocks; ++block) {
    // U_1 = HMAC(password, salt || INT_32_BE(block))
    std::string saltBlock = salt;
    appendInt32Be(static_cast<std::uint32_t>(block), saltBlock);
    std::string u = hmac(saltBlock);
    std::string t = u;  // T = U_1
    // T = U_1 ^ U_2 ^ ... ^ U_c
    for (int i = 2; i <= iterations; ++i) {
      u = hmac(u);  // U_i = HMAC(password, U_{i-1})
      for (int j = 0; j < kSha256Out; ++j) {
        t[j] = static_cast<char>(static_cast<unsigned char>(t[j]) ^
                                  static_cast<unsigned char>(u[j]));
      }
    }
    dk.append(t);
  }
  dk.resize(dkLenBytes);  // 末块可能需截断
  return bytesToHex(dk);
}

bool constantTimeEqualHex(const std::string& a, const std::string& b) {
  // 长度差直接以 size_t 参与（不截断），保证长度不同必使 diff 非零
  std::size_t diff = a.size() ^ b.size();
  const std::size_t n = a.size() < b.size() ? a.size() : b.size();
  for (std::size_t i = 0; i < n; ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  // 较长串多出的字节也参与 diff（长度差已使 diff 非零，这里仅保持恒定时长）
  const std::string& longer = a.size() < b.size() ? b : a;
  const std::size_t m = a.size() + b.size() - n;  // == max(len_a, len_b)
  for (std::size_t i = n; i < m; ++i) {
    diff |= static_cast<unsigned char>(longer[i]);
  }
  return diff == 0;
}

}  // namespace pbkdf2
}  // namespace cv

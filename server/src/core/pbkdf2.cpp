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

// HMAC-SHA256(key, msg)：key / msg 均为原始字节串。
// 密钥超 64 字节先用 SHA-256 压缩；不足 64 字节右侧补 0。
std::string hmacSha256(const std::string& key, const std::string& msg) {
  std::string k = key;
  // ⚠️ `Sha256::of()` 返回的是 **hex 字符串（64 字符）**，不是 32 字节原始摘要。
  // HMAC 内部必须用原始字节，否则 PBKDF2 会变成"hex 套 hex"：
  // 实测 4 条公开标准向量全部不过（输出是 hex(hex(HMAC)) 的形态）。
  if (k.size() > kSha256Block) k = hexToBytes(Sha256::of(k));  // 超长密钥 → 压缩为 32 字节原始摘要
  k.resize(kSha256Block, '\0');

  std::string ipad(kSha256Block, '\0');
  std::string opad(kSha256Block, '\0');
  for (int i = 0; i < kSha256Block; ++i) {
    unsigned char c = static_cast<unsigned char>(k[i]);
    ipad[i] = static_cast<char>(c ^ 0x36);
    opad[i] = static_cast<char>(c ^ 0x5c);
  }

  std::string inner = ipad + msg;
  // 内层摘要必须是**原始 32 字节**：HMAC 外层要哈希 opad || innerDigestBytes，
  // 若这里用 hex 串（64 字符），HMAC 就是错的（实测向量不过）。
  std::string innerHash = hexToBytes(Sha256::of(inner));  // SHA-256(ipad || msg) 的原始字节
  std::string outer = opad + innerHash;        // SHA-256(opad || innerHash)
  return hexToBytes(Sha256::of(outer));        // ← 转回原始 32 字节（见函数头注释）
}

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

  std::string dk;
  dk.reserve(dkLenBytes);
  const int blocks = (dkLenBytes + kSha256Out - 1) / kSha256Out;
  for (int block = 1; block <= blocks; ++block) {
    // U_1 = HMAC(password, salt || INT_32_BE(block))
    std::string saltBlock = salt;
    appendInt32Be(static_cast<std::uint32_t>(block), saltBlock);
    std::string u = hmacSha256(password, saltBlock);
    std::string t = u;  // T = U_1
    // T = U_1 ^ U_2 ^ ... ^ U_c
    for (int i = 2; i <= iterations; ++i) {
      u = hmacSha256(password, u);  // U_i = HMAC(password, U_{i-1})
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

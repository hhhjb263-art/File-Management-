#pragma once

#include <string>

namespace cv {
namespace pbkdf2 {

// PBKDF2-HMAC-SHA256 密钥派生。
// 基于项目自带的零依赖 cv::Sha256 实现，不依赖 OpenSSL（服务端 TLS 为可选编译，
// 引入 PKCS5_PBKDF2_HMAC 会在无 TLS 构建下编译/链接失败）。
//
// deriveHex: 用 saltHex（16 字节随机盐的十六进制）对 password 做 PBKDF2-HMAC-SHA256，
//            迭代 iterations 次，输出 dkLenBytes 字节派生密钥的小写十六进制。
//            默认迭代 210000（见 auth_routes / 仓储约定）。
std::string deriveHex(const std::string& password, const std::string& saltHex,
                      int iterations, int dkLenBytes);

// 恒定时间比较两个十六进制字符串（防时序侧信道）：长度不同必返回 false，且不提前 return，
// 比较时长与首个不同字节位置无关。用于登录时比对存储的 password_hash。
bool constantTimeEqualHex(const std::string& a, const std::string& b);

}  // namespace pbkdf2
}  // namespace cv

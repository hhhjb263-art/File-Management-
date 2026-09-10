#pragma once

#include <string>

namespace cv {
namespace util {

// 百分号解码（用于 URL 中的文件名参数）。
inline std::string urlDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      int hi = hex(s[i + 1]);
      int lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    if (s[i] == '+') {
      out.push_back(' ');
      continue;
    }
    out.push_back(s[i]);
  }
  return out;
}

// 保留 ASCII 安全字符，其余做百分号编码（用于 JSON 中的路径回显）。
inline std::string urlEncode(const std::string& s) {
  static const char* kUnreserved =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";
  std::string out;
  static const char* kHex = "0123456789ABCDEF";
  for (unsigned char c : s) {
    bool safe = false;
    for (const char* p = kUnreserved; *p; ++p) {
      if (*p == static_cast<char>(c)) {
        safe = true;
        break;
      }
    }
    if (safe) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[(c >> 4) & 0xf]);
      out.push_back(kHex[c & 0xf]);
    }
  }
  return out;
}

// 取文件名中的基础名，去掉可能的路径分隔符（防目录穿越的第一道闸）。
inline std::string baseName(const std::string& path) {
  std::size_t pos = path.find_last_of("/\\");
  std::string name = (pos == std::string::npos) ? path : path.substr(pos + 1);
  if (name.empty() || name == "." || name == "..") name = "unnamed";
  return name;
}

}  // namespace util
}  // namespace cv

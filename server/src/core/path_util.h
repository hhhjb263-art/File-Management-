#pragma once

// 相对路径规范化与校验（目录树边界防护的唯一入口）。
//
// 所有用户提供的"目录/路径"参数（上传目标目录、按路径下载等）必须先经
// sanitizeRelPath() 清洗，再用于 DB 查询或磁盘拼接。规则：
//   - 只接受 '/' 分隔的相对路径；拒绝反斜杠、盘符/冒号、绝对路径
//   - 拒绝 "." / ".." 路径段（路径穿越）
//   - 拒绝控制字符与 Windows 非法字符 < > : " | ? *
//   - 拒绝空路径段（如 a//b）、以 '.' 或空格结尾的段、Windows 保留设备名
//   - 限长：整路径 400 字节，单段 128 字节（中文名按 UTF-8 字节计）

#include <cstddef>
#include <string>

namespace cv {

enum class PathStatus {
  Ok,           // 合法（规范化完成）
  Empty,        // 空串（按约定视为"根目录"，由调用方决定是否接受）
  Absolute,     // 绝对路径 / 盘符 / 冒号（含反斜杠形式）
  Traversal,    // 含 "." / ".." 段
  IllegalChar,  // 非法字符 / 非法段（空段、保留名等）
  TooLong       // 超长
};

inline const char* pathStatusText(PathStatus s) {
  switch (s) {
    case PathStatus::Ok: return "ok";
    case PathStatus::Empty: return "empty path";
    case PathStatus::Absolute: return "absolute path / drive letter not allowed";
    case PathStatus::Traversal: return "'.' or '..' segments not allowed";
    case PathStatus::IllegalChar: return "illegal characters or path segment";
    case PathStatus::TooLong: return "path too long";
  }
  return "unknown";
}

namespace detail {

// Windows 保留设备名（含带扩展名形态，如 CON.txt），大小写不敏感
inline bool isReservedWinName(const std::string& seg) {
  std::string up;
  up.reserve(seg.size());
  for (char c : seg) {
    char u = c;
    if (u >= 'a' && u <= 'z') u = static_cast<char>(u - 'a' + 'A');
    up.push_back(u);
  }
  std::size_t dot = up.find('.');
  std::string base = (dot == std::string::npos) ? up : up.substr(0, dot);
  static const char* kNames[] = {"CON",    "PRN",  "AUX",  "NUL",
                                 "COM1",   "COM2", "COM3", "COM4", "COM5",
                                 "COM6",   "COM7", "COM8", "COM9",
                                 "LPT1",   "LPT2", "LPT3", "LPT4", "LPT5",
                                 "LPT6",   "LPT7", "LPT8", "LPT9"};
  for (const char* n : kNames) {
    if (base == n) return true;
  }
  return false;
}

}  // namespace detail

// 把用户输入清洗为规范化的相对路径（'/' 分隔，无首尾分隔符）。
// 返回 Ok 时 out 为规范化结果；其余状态时 out 被清空。
inline PathStatus sanitizeRelPath(const std::string& in, std::string& out) {
  out.clear();
  if (in.empty()) return PathStatus::Empty;
  if (in.size() > 400) return PathStatus::TooLong;
  if (in[0] == '/' || in[0] == '\\') return PathStatus::Absolute;
  if (in.find(':') != std::string::npos) return PathStatus::Absolute;
  if (in.find('\\') != std::string::npos) return PathStatus::IllegalChar;
  for (unsigned char c : in) {
    if (c < 0x20 || c == 0x7F) return PathStatus::IllegalChar;
    if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') {
      return PathStatus::IllegalChar;
    }
  }
  if (in.find("//") != std::string::npos) return PathStatus::IllegalChar;
  if (in.back() == '/') return PathStatus::IllegalChar;

  // 逐段校验
  std::size_t start = 0;
  while (start <= in.size()) {
    std::size_t slash = in.find('/', start);
    std::string seg = in.substr(start, slash == std::string::npos
                                          ? std::string::npos
                                          : slash - start);
    if (seg.empty() || seg == "." || seg == "..") return PathStatus::Traversal;
    if (seg.size() > 128) return PathStatus::TooLong;
    if (seg.back() == '.' || seg.back() == ' ') return PathStatus::IllegalChar;
    if (detail::isReservedWinName(seg)) return PathStatus::IllegalChar;
    out += seg;
    if (slash == std::string::npos) break;
    out += '/';
    start = slash + 1;
  }
  return PathStatus::Ok;
}

}  // namespace cv

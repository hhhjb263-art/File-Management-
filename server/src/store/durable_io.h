#pragma once

// store/durable_io.h —— 落盘屏障工具（断电安全的关键补丁）
//
// ── 为什么需要它 ──
// POSIX 里 `rename` 是**原子**的，但不是**持久**的；`ofstream::flush()` 只把数据推进
// 操作系统页缓存，不保证落到介质。于是存在这样一条致命时序：
//     写 blob → rename 成最终名 → DB COMMIT（synchronous=FULL，真落盘）→ 掉电
//   ⇒ 元数据说"内容在"，blob 的数据块却从未落盘（ext4 delayed allocation 下常见表现
//     是长度为 0 或全零）。又因为内容寻址 + 秒传会把"文件已存在"当成命中，
//     这种坏 blob 会被**永久固化**，永不自愈，且 ref_count 还在、GC 不会碰它。
//
// ── 正确顺序（本文件封装）──
//     写临时文件 → fsync(临时文件) → rename 到最终名 → fsync(父目录)
// 父目录的 fsync 保证"改名"这个目录项本身也持久；否则可能出现"数据在、名字没了"。
//
// ── 使用契约 ──
// 所有 blob / 分块 part / 镜像的写盘都必须走这里，并且必须在**提交引用它的 DB 事务之前**
// 完成 —— 只有这样才能保证"数据库说有的，磁盘上真的有"。

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>

namespace cv {
namespace durable {

namespace fs = std::filesystem;

// 可移植的"二进制打开"标志：Linux 上无此概念（O_BINARY 通常未定义 → 0），
// Windows/MinGW 上必须带它，否则文本模式会把数据里的 \n 改写成 \r\n，静默损坏 blob。
#ifndef O_BINARY
#define O_BINARY 0
#endif

// 对已打开的 fd 调 fsync。EINTR 重试（被信号打断不算失败）。
inline bool fsyncFd(int fd, const std::string& what, std::string& err) {
  if (fd < 0) {
    err = "fsync: bad fd for " + what;
    return false;
  }
  while (true) {
    if (::fsync(fd) == 0) return true;
    if (errno == EINTR) continue;
    err = "fsync failed (" + what + "): " + std::strerror(errno);
    return false;
  }
}

// 把「已存在的文件」内容刷到介质。
// ⚠️ 这一步最容易被漏掉：`rename` 只改名字，**不会**替你把早先写下的脏页刷下去。
//    所以 `putFromFile` 那种"纯 rename 搬移"的路径必须显式调用本函数。
inline bool flushFile(const std::string& path, std::string& err) {
  // 首选只读打开 —— POSIX 里 fsync 与打开方式无关，这是标准写法。
  int fd = ::open(path.c_str(), O_RDONLY | O_BINARY);
  if (fd < 0) {
    err = "open for fsync failed (" + path + "): " + std::strerror(errno);
    return false;
  }
  if (fsyncFd(fd, path, err)) {
    ::close(fd);
    return true;
  }

  // 退化路径：少数环境拒绝在**只读** fd 上 fsync。实测 MSVCRT 的 `_commit` 直接返回
  // EBADF(errno 9)，而 Linux 的 fsync 完全接受只读 fd —— 所以这一步在 Linux 上
  // **永不触发、零开销**，存在的意义是让本地 Windows harness 能真正跑起来验证本文件，
  // 顺带兜住个别 POSIX 系统对只读 fd 的 fsync 限制。
  const int savedErrno = errno;
  ::close(fd);
  fd = ::open(path.c_str(), O_RDWR | O_BINARY);
  if (fd < 0) {
    err = "fsync failed (errno=" + std::to_string(savedErrno) + ") and reopen for write " +
          "failed (" + path + "): " + std::strerror(errno);
    return false;
  }
  const bool ok = fsyncFd(fd, path, err);
  ::close(fd);
  return ok;
}

// 把父目录的目录项刷到介质。**尽力而为**：不支持目录 fsync 的文件系统会失败，
// 此时返回 false 仅作告警 —— 调用方**不应**据此判定写盘失败：
// 数据本身已经 flushFile 过了，丢的最多是"名字"这一条元数据。
inline bool flushDirOf(const std::string& path, std::string& err) {
  std::error_code ec;
  fs::path parent = fs::path(path).parent_path();
  if (parent.empty()) parent = ".";
  const int fd = ::open(parent.string().c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    err = "open dir for fsync failed (" + parent.string() + "): " + std::strerror(errno);
    return false;
  }
  const bool ok = fsyncFd(fd, parent.string(), err);
  ::close(fd);
  return ok;
}

// 原子 + 持久地写一个文件：临时文件 → fsync → rename → fsync 父目录。
// 父目录不存在时自动创建（历史上真机踩过 "cannot open tmp for write"）。
inline bool writeFileAtomic(const std::string& finalPath, const std::string& data,
                            std::string& err) {
  const fs::path fp(finalPath);
  if (!fp.parent_path().empty()) {
    std::error_code dec;
    fs::create_directories(fp.parent_path(), dec);  // 已存在 → 幂等空操作
  }

  const std::string tmp = finalPath + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
  if (fd < 0) {
    err = "cannot open tmp for write (" + tmp + "): " + std::strerror(errno);
    return false;
  }

  // 循环写：write() 允许短写（short write），必须处理
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      err = "write failed (" + tmp + "): " + std::strerror(errno);
      ::close(fd);
      std::error_code rec;
      fs::remove(tmp, rec);
      return false;
    }
    if (n == 0) {
      err = "write returned 0 (" + tmp + ")";
      ::close(fd);
      std::error_code rec;
      fs::remove(tmp, rec);
      return false;
    }
    off += static_cast<std::size_t>(n);
  }

  // ① 内容先落盘 —— 必须在 rename 之前，这是整个屏障的关键
  if (!fsyncFd(fd, tmp, err)) {
    ::close(fd);
    std::error_code rec;
    fs::remove(tmp, rec);
    return false;
  }
  ::close(fd);

  // ② 改名（原子）
  std::error_code rec2;
  fs::rename(tmp, finalPath, rec2);
  if (rec2) {
    std::error_code rec;
    fs::remove(tmp, rec);
    err = "rename failed: " + rec2.message();
    return false;
  }

  // ③ 目录项也落盘。失败只吞掉不报错：数据已持久，回滚反而会删掉已持久的内容。
  std::string dirErr;
  (void)flushDirOf(finalPath, dirErr);
  return true;
}

}  // namespace durable
}  // namespace cv

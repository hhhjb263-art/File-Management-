#pragma once

// Unix 毫秒 → ISO-8601 UTC 字符串（"2026-01-01T00:00:00Z"）。
//
// 为什么不用 gmtime/gmtime_r：
//   · `std::gmtime` 返回指向**共享静态缓冲**的指针 —— 服务端是多 worker 线程，会互相串数据；
//   · `gmtime_r` 是 POSIX 专有，Windows 侧不可用。
// 这里用 civil-from-days 自行换算，**无共享状态、无时区依赖、可离线单测**。
//
// 约定：ms <= 0 返回空串（与项目里 0 表示"未设置/永久"的语义一致）。

#include <cstdint>
#include <cstdio>
#include <string>

namespace cv {
namespace isotime {

inline std::string fromMillis(std::int64_t ms) {
  if (ms <= 0) return std::string();
  std::int64_t secs = ms / 1000;
  std::int64_t days = secs / 86400;
  std::int64_t rem  = secs % 86400;
  if (rem < 0) {
    rem += 86400;
    days -= 1;
  }
  const int hh = static_cast<int>(rem / 3600);
  const int mm = static_cast<int>((rem % 3600) / 60);
  const int ss = static_cast<int>(rem % 60);

  const std::int64_t z   = days + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned     doe = static_cast<unsigned>(z - era * 146097);
  const unsigned     yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y   = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned     doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned     mp  = (5 * doy + 2) / 153;
  const unsigned     d   = doy - (153 * mp + 2) / 5 + 1;
  const unsigned     m   = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t year = y + (m <= 2 ? 1 : 0);

  char buf[40];
  std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02d:%02d:%02dZ",
                static_cast<long long>(year), m, d, hh, mm, ss);
  return std::string(buf);
}

}  // namespace isotime
}  // namespace cv

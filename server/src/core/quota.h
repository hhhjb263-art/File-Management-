/****************************************************************************
 * core/quota.h —— 用户空间配额判定（纯逻辑，无 IO、无依赖）
 *
 * 阈值取自 PRD，不是我拍的：
 *   · FR-1-4（P1）管理员为每个用户设置配额（默认不限）
 *   · FR-7-6（**P0**）用量 ≥80% 提醒；**≥95% 阻止新上传并明确提示原因**
 *
 * ── 为什么单独抽成纯函数 ──
 * 配额判断是"整盘共享 vs 每人限额"里唯一容易写错、且写错代价很实在的一段
 * （判错方向要么让用户传不上去、要么让人把盘写满）。它没有 IO，正好适合表驱动测试：
 * 边界值（79/80/94/95/100/超一点/超很多）、正好装满、quota=0 不限、负数脏数据
 * 这些用一张表就能全覆盖，不用起服务端。
 ****************************************************************************/
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace cv {
namespace quota {

// PRD FR-7-6：≥80% 提醒；≥95% 阻止新上传
constexpr int kWarnPercent   = 80;
constexpr int kRejectPercent = 95;

enum class Verdict
{
    Ok,       // 放行
    Warn,     // 放行，但要提醒用户
    Reject    // 阻止
};

struct Decision
{
    Verdict      verdict  = Verdict::Ok;
    std::int64_t quota    = 0;   // 0 = 不限
    std::int64_t used     = 0;
    std::int64_t incoming = 0;
    int          percent  = 0;   // 当前用量占配额的百分比（**向上取整**）
    std::string  reason;         // Reject/Warn 时给用户看的中文原因
};

// 人类可读的字节数（仅用于提示文案，不参与判定）
inline std::string humanBytes(std::int64_t n)
{
    const char *unit[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    long double v      = static_cast<long double>(n);
    int         u      = 0;
    while (v >= 1024.0L && u < 5) {
        v /= 1024.0L;
        ++u;
    }
    char buf[64];
    if (u == 0)
        std::snprintf(buf, sizeof(buf), "%lld %s", static_cast<long long>(n), unit[u]);
    else
        std::snprintf(buf, sizeof(buf), "%.2Lf %s", v, unit[u]);
    return std::string(buf);
}

// 百分比（**向下取整**，只用于显示与提示文案）。
//
// ⚠️ 判定本身**不用它**，而是用下面的 exactPercentAtLeast() 做精确整数比较。
//    原因：PRD 写的是"≥95% 阻止"。若拿四舍五入/向上取整的结果去判，94.1% 会被算成 95%
//    从而被拒 —— 那是在拦本不该拦的上传，属于偏离验收标准。
//    （我第一版就是向上取整，写用例时才发现 941/1000 会被误拒。）
//    显示用向下取整则是保守且符合直觉的：说"已用 94%"时实际不超过 94.x%。
inline int percentOf(std::int64_t part, std::int64_t whole)
{
    if (whole <= 0 || part <= 0)
        return 0;
    if (part >= whole)
        return 100;
    return static_cast<int>((static_cast<long double>(part) * 100.0L) /
                            static_cast<long double>(whole));
}

// 精确判定：part 是否已达到 whole 的 pct%（等价于 part*100 >= pct*whole，用 long double 避免溢出）。
inline bool exactPercentAtLeast(std::int64_t part, std::int64_t whole, int pct)
{
    if (whole <= 0)
        return false;
    return static_cast<long double>(part) * 100.0L >=
           static_cast<long double>(pct) * static_cast<long double>(whole);
}

// used：该用户当前已用（口径 = `sumSizeOfOwner`，即 deleted=0 的逻辑大小）
// quota：0 或负数 = 不限
// incoming：本次要写进来的字节数（init 时是客户端声明的 size，complete 时是真实字节数）
inline Decision evaluate(std::int64_t used, std::int64_t quota, std::int64_t incoming)
{
    Decision d;
    d.quota    = quota;
    d.used     = used < 0 ? 0 : used;   // 脏数据兜底
    d.incoming = incoming < 0 ? 0 : incoming;
    if (quota <= 0)
        return d;   // 不限：一切放行，percent 保持 0

    d.percent = percentOf(d.used, quota);

    // ① 这次就装不下 —— 用减法比较，避免 used+incoming 溢出
    if (d.incoming > 0 && d.incoming > quota - d.used) {
        d.verdict = Verdict::Reject;
        d.reason  = "空间不足：已用 " + humanBytes(d.used) + "，本次 " + humanBytes(d.incoming) +
                   "，配额 " + humanBytes(quota);
        return d;
    }

    // ② PRD FR-7-6 的硬停线：用量 ≥95% 时**阻止任何**新上传（哪怕还剩几个字节）。
    //    这是文档明写的行为，不是我的取舍：留出的 5% 是给版本快照与元数据的余量。
    //    ⚠️ 别把它"顺手优化"成"还有空间就允许" —— 那会直接破坏这条 P0 验收项。
    if (exactPercentAtLeast(d.used, quota, kRejectPercent)) {
        d.verdict = Verdict::Reject;
        d.reason  = "已用空间已达配额的 " + std::to_string(d.percent) + "%（≥" +
                   std::to_string(kRejectPercent) + "% 时不允许再上传）。请先清理空间或联系管理员调整配额。";
        return d;
    }

    if (exactPercentAtLeast(d.used, quota, kWarnPercent)) {
        d.verdict = Verdict::Warn;
        d.reason  = "已用空间已达配额的 " + std::to_string(d.percent) + "%（" + humanBytes(d.used) +
                   " / " + humanBytes(quota) + "），接近上限";
    }
    return d;
}

}   // namespace quota
}   // namespace cv

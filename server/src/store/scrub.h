/****************************************************************************
 * store/scrub.h —— 主动巡检：发现"已经坏了的块"（header-only）
 *
 * ── 与既有机制的分工 ──
 *   · `store/durable_io.h`  保证**写下去的**内容不会半截（落盘屏障）
 *   · `store/blob_gc.h`     回收崩溃留下的**孤儿** blob（无人引用）
 *   · `store/scrub.h`（本文件）读回每一块并**重新算哈希**，发现**已经损坏**的内容
 * 前两者都防不了"写完之后烂掉"：磁盘静默损坏（bit rot）、坏道、误操作改文件、
 * 甚至多年前一次断电留下的坏块。没有巡检时，这类损坏只会在**用户点开文件那一刻**暴露 ——
 * 而那时往往已经没有好副本了。
 *
 * ── 为什么要由调用方给「本该有哪些块」，而不是枚举磁盘 ──
 * 最初想图省事：遍历 blobs/ 目录逐个读回算哈希与文件名比对。但 harness 立刻打脸 ——
 * **整块不见了**发现不了（文件都没了，枚举自然看不到它），而"元数据说有、磁盘上没有"
 * 恰恰是最严重的损坏（坏盘、误删、从残缺备份恢复）。故改为：元数据给"本该有什么"（权威），
 * 磁盘内容是被校验的对象。代价是本层需要调用方先给一份哈希清单。
 * 磁盘上"多出来"的孤儿 blob 不归本模块管，那是 blob_gc 的职责。
 * 内容寻址库的不变量就是 `<root>/<hash[0:2]>/<hash>`：文件名即内容的 SHA-256。
 * 所以巡检**完全不需要查数据库** —— 逐个 blob 读回、重算哈希、与文件名比对即可。
 * 好处有三：
 *   1. 分层干净：store 层不依赖 meta 层；
 *   2. 覆盖面更广：连"没被任何元数据引用"的 blob 也一并校验；
 *   3. 后台周期性巡检可以直接开线程跑：只做文件 IO（加密库的上下文是每次调用新建的），
 *      **不需要共享服务端的数据库句柄** —— 绕开了 Db 的线程归属问题。
 * 代价是：本层报出的是"哪些**分块**坏了"，映射到"哪些**文件**受影响"属于应用层的事
 * （需要查 file_chunk），由 app 层按需补。
 *
 * ── 加密库怎么办 ──
 * 一律走 `ContentStore::get()`：它按 `.enc` 后缀决定是否解密，返回**明文**。
 * 因此"对明文算哈希再和文件名比"对明文库与加密库同样成立；
 * 加密库还能顺带验出"密文被篡改"（GCM 校验失败 → 读失败，计入 readErrors）。
 *
 * ── 成本 ──
 * 一次全量巡检会把**整个内容库读一遍**。100GB 的库就是 100GB 读盘 ——
 * 所以它**默认关闭**，只在显式 `--scrub` 或配了周期时才跑。
 ****************************************************************************/
#pragma once

#include <string>
#include <vector>

#include "core/sha256.h"
#include "store/content_store.h"

namespace cv {
namespace scrub {

struct Stats
{
    std::int64_t checked = 0;      // 尝试校验过的 blob 数
    std::int64_t ok = 0;           // 内容与文件名一致
    std::int64_t missing = 0;      // 文件名合法，但文件已经不在磁盘上
    std::int64_t corrupt = 0;      // 读回来了，但哈希对不上 ⇒ 内容确实坏了
    std::int64_t readErrors = 0;   // 读/解密失败（加密库密文被篡改也会落这里）
    std::int64_t bytes = 0;        // 实际读过的字节数（用于估算耗时/带宽）
    std::vector<std::string> badHashes;   // 有问题的分块哈希（便于后续定位受影响文件）
};

// 全量巡检。stopOnFirstBad=true 时发现第一个坏块即返回（用于快速探活）。
// 返回 false 表示**遍历本身**失败（如根目录都读不了），而不是"发现了坏块"——
// 发现坏块属于正常结果，会准确反映在 Stats 里。这个区分很重要：
// 否则"巡检失败"会被误读成"数据没问题"。
inline bool scanHashes(ContentStore &store, const std::vector<std::string> &hashes,
                       bool stopOnFirstBad, Stats &st, std::string &err)
{
    for (const std::string &h : hashes) {
        ++st.checked;
        if (!store.exists(h)) {
            // 元数据说有、磁盘上没有 —— 整块丢失，最严重的一类
            ++st.missing;
            st.badHashes.push_back(h);
            if (stopOnFirstBad)
                return true;
            continue;
        }

        std::string data;
        std::string gerr;
        if (!store.get(h, data, gerr)) {
            // 读不出来（截断到无法解析 / 密文校验失败）同样算坏
            ++st.readErrors;
            st.badHashes.push_back(h);
            if (stopOnFirstBad)
                return true;
            continue;
        }
        st.bytes += static_cast<std::int64_t>(data.size());

        // 尺寸没变的静默损坏（bit rot）只能靠重新算哈希发现 —— 这正是本机制存在的意义
        if (Sha256::of(data) != h) {
            ++st.corrupt;
            st.badHashes.push_back(h);
            if (stopOnFirstBad)
                return true;
            continue;
        }
        ++st.ok;
    }
    return true;
}

}   // namespace scrub
}   // namespace cv

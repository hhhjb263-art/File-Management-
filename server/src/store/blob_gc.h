/****************************************************************************
 * store/blob_gc.h —— 启动期孤儿 blob 回收（header-only）
 *
 * ── 为什么需要 ──
 * `meta/chunk_gc.h` 只清理 chunk 表里 ref_count<=0 的**行**，从来不碰磁盘。
 * 而崩溃可能死在"分块已搬进 blobs/、chunk 行还没写"之间 —— 于是磁盘上留下
 * **永远无人引用、也永远无人回收**的 blob。落盘屏障（durable_io.h）保证的是
 * "写下的东西不会坏"，本模块解决的是"崩溃留下的残骸要有人扫"。
 *
 * ── 分层 ──
 * store/ 不依赖 meta/（服务端分层是 core → store/meta/net → app，反向依赖是禁止的），
 * 所以"这个 hash 是否仍被引用"由调用方以回调传入。app/main.cpp 里把它接到
 * FileRepository::chunkExists。
 *
 * ── 安全设计（这是会删数据的代码，务必按这几条读）──
 *  1. **只在开始监听之前跑**：此时本进程没有在途上传，不存在"blob 刚落地、chunk 行未写"的窗口。
 *  2. **宽限期（graceMillis）**：mtime 落在宽限期内的 blob 一律不删。本项目**没有单实例锁**，
 *     另一个进程可能正在上传 —— 这是对并发实例的兜底。grace<=0 表示不限（危险，慎用）。
 *  3. **判定失败一律保留**：查库出错时按"被引用"处理，宁可漏清也不误删。
 *  4. 命名不合法（非 64 位小写 hex / 分片目录不匹配）的文件直接跳过，绝不当作垃圾删。
 ****************************************************************************/
#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "store/content_store.h"

namespace cv {
namespace blobgc {

struct Stats
{
    std::int64_t scanned = 0;          // 扫描到的 blob 条目数
    std::int64_t removed = 0;          // 删除（dryRun 时为"将会删除"）数
    std::int64_t keptReferenced = 0;   // 仍被引用的，保留
    std::int64_t keptFresh = 0;        // 在宽限期内，保留
    std::int64_t keptOnError = 0;      // 判定失败，保守保留
    std::int64_t freedBytes = 0;       // 释放字节数（按明文 blob 尺寸计）
};

// isReferenced(hash, out, err)：该 hash 是否仍被元数据引用。
inline bool collectOrphans(ContentStore &store,
                           const std::function<bool(const std::string &, bool &, std::string &)>
                               &isReferenced,
                           std::int64_t graceMillis, std::int64_t nowMillis, bool dryRun,
                           Stats &st, std::string &err)
{
    std::vector<ContentStore::BlobInfo> blobs;
    if (!store.listBlobs(blobs, err))
        return false;

    std::set<std::string> seen;
    for (const ContentStore::BlobInfo &b : blobs) {
        ++st.scanned;
        // 明文迁移期可能出现 <hash> 与 <hash>.enc 并存：同一个 hash 只处理一次
        if (!seen.insert(b.hash).second)
            continue;

        bool        referenced = false;
        std::string qerr;
        if (!isReferenced(b.hash, referenced, qerr)) {
            ++st.keptOnError;   // 查不出来就当我删不起 —— 保守保留
            continue;
        }
        if (referenced) {
            ++st.keptReferenced;
            continue;
        }
        if (graceMillis > 0 && b.mtimeMs > 0 && (nowMillis - b.mtimeMs) < graceMillis) {
            ++st.keptFresh;
            continue;
        }

        if (dryRun) {
            ++st.removed;
            st.freedBytes += b.size;
            continue;
        }
        std::string derr;
        if (!store.drop(b.hash, derr))
            continue;   // 单个删除失败只跳过，不影响其余
        ++st.removed;
        st.freedBytes += b.size;
    }
    return true;
}

// ============================================================================
// 临时目录清扫（与孤儿 blob 同一类问题：崩溃留下的残骸没人收）
//
// ── 为什么会话 GC 不够 ──
// 启动期的会话 GC 是"遍历数据库里的会话，删掉过期会话的临时目录"。
// 那么**没有对应会话行的临时目录**它永远碰不到 —— 而崩溃恰好可能死在
// 「mkdir tmp/uploads/<id>」与「INSERT upload_session」之间，留下一个无主目录。
// 这种残骸既不会被会话 GC 清，也不在 blobRoot 里（blob_gc 不管 tmp），于是永久残留。
//
// ── 判定 ──
// 目录名即会话 id（见 app 层的 uploadTmpDir）。凡是不在 activeIds 里的目录都算无主，
// 但**仍受宽限期保护**：本项目没有单实例锁，另一个进程可能正在上传（它刚建好目录、
// 会话行还没写）。宽限期是这一小段窗口的兜底。
// ============================================================================
struct TmpStats
{
    std::int64_t scanned = 0;      // 扫描到的子目录数
    std::int64_t removed = 0;      // 删除（dryRun 时为"将会删除"）数
    std::int64_t keptActive = 0;   // 有对应会话，保留
    std::int64_t keptFresh = 0;    // 无主但在宽限期内，保留
    std::int64_t freedBytes = 0;   // 释放字节数
};

inline std::int64_t dirSizeBytes(const std::filesystem::path &p)
{
    std::int64_t             total = 0;
    std::error_code          ec;
    for (std::filesystem::recursive_directory_iterator it(
             p, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }
        const auto sz = it->file_size(ec);
        if (!ec) total += static_cast<std::int64_t>(sz);
        ec.clear();
    }
    return total;
}

inline bool collectOrphanTmpDirs(const std::string &uploadsDir,
                                 const std::set<std::int64_t> &activeIds,
                                 std::int64_t graceMillis, std::int64_t nowMillis, bool dryRun,
                                 TmpStats &st, std::string &err)
{
    std::error_code ec;
    if (!std::filesystem::exists(uploadsDir, ec))
        return true;   // 还没建过 ⇒ 无事可做，不是错误

    const auto toUnixMs = [](std::filesystem::file_time_type t) -> std::int64_t {
        const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count());
    };

    for (std::filesystem::directory_iterator it(
             uploadsDir, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        std::error_code iec;
        if (!it->is_directory(iec) || iec) { iec.clear(); continue; }

        ++st.scanned;
        const std::string name = it->path().filename().string();

        // 目录名必须是纯数字（会话 id）；不是的话说明是人工放的东西，一律不动
        bool numeric = !name.empty();
        for (char c : name)
            if (c < '0' || c > '9') numeric = false;
        if (!numeric)
            continue;

        std::int64_t id = 0;
        try {
            id = std::stoll(name);
        } catch (...) {
            continue;
        }
        if (activeIds.count(id) > 0) {
            ++st.keptActive;
            continue;
        }

        const auto mtimeMs = toUnixMs(it->last_write_time(iec));
        iec.clear();
        if (graceMillis > 0 && mtimeMs > 0 && (nowMillis - mtimeMs) < graceMillis) {
            ++st.keptFresh;
            continue;
        }

        const std::int64_t sz = dirSizeBytes(it->path());
        if (dryRun) {
            ++st.removed;
            st.freedBytes += sz;
            continue;
        }
        std::error_code rec;
        std::filesystem::remove_all(it->path(), rec);
        if (rec)
            continue;   // 单个删失败只跳过
        ++st.removed;
        st.freedBytes += sz;
    }
    return true;
}

}   // namespace blobgc
}   // namespace cv

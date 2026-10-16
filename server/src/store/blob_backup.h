/****************************************************************************
 * store/blob_backup.h —— 内容库副本：备份 + 从副本修复（header-only）
 *
 * ── 这是"磁盘损坏"那条清单项的正面回答 ──
 * 清单原文要求：多副本 / 纠删码 / 定期快照 / 备份到另一台机器。
 * 纠删码与自动多副本是另一个量级的工程（需要分片编码、机架感知、一致性协议）；
 * 本文件做的是**同一件事里最小可用的那一步**：把内容库**增量**镜像到第二个位置
 * （外挂盘、NAS、另一台机器挂载点），并在主库损坏时从副本修复。
 *
 * ── 为什么它必须和 scrub 配套存在 ──
 * `store/scrub.h` 只能"发现"哪个块坏了 —— 没有副本时，它的结论只能是一句"这个文件没救了"。
 * 有了副本，同一份坏块清单才变成**可执行的动作**：
 *     `--scrub` 找出坏块 → `--restore-blobs` 从副本写回 → 再 `--scrub` 复核
 * 所以这两个模块是同一件事的两半，单独交付任何一个都只算做了一半。
 *
 * ── 增量是白送的 ──
 * 内容寻址库的不变量：文件名 = 内容的 SHA-256。于是"这个块副本里有没有"退化成
 * "这个文件名在不在" —— **不需要比对、不需要记录上次备份时间**，
 * 重跑一次只会补上新增的块（去重语义天然带来的好处）。
 *
 * ── 为什么备份阶段要校验源 ──
 * 直接把坏块复制过去，等于把"唯一的一份坏数据"变成"两份坏数据"，
 * 而备份的存在意义恰恰是能救命 —— 所以**新复制的块必须先验证源哈希**再落盘。
 * 已存在（同名）的块跳过校验：它的正确性由定期 `--scrub` 负责，备份不重复扫全库。
 *
 * ── 为什么恢复阶段必须校验副本 ──
 * 恢复是"用副本覆盖主库"。若副本本身也是坏的，覆盖会**毁掉本来可能还能救的数据**
 * （例如主库只是读不出来、还能人工抢救）。所以副本必须先自证（重算哈希）才允许写回。
 ****************************************************************************/
#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/sha256.h"
#include "store/content_store.h"

namespace fs = std::filesystem;

namespace cv {
namespace blobbackup {

struct BackupStats
{
    std::int64_t scanned = 0;         // 主库里扫到的块数
    std::int64_t copied = 0;          // 新复制过去的
    std::int64_t skippedExisting = 0; // 副本里已有（增量跳过）
    std::int64_t skippedBad = 0;      // 源本身就是坏的 —— 拒绝复制（否则等于复制错误）
    std::int64_t bytesCopied = 0;
};

struct RestoreStats
{
    std::int64_t checked = 0;      // 检查过的块
    std::int64_t alreadyOk = 0;    // 主库里本来就好的
    std::int64_t restored = 0;     // 从副本修复回来的
    std::int64_t missingInReplica = 0;  // 主库坏了、副本也没有
    std::int64_t badReplica = 0;        // 副本有，但副本自己也坏了（**拒绝写回**）
    std::int64_t copyFailed = 0;        // 副本是好的，但写回主库失败（磁盘/权限问题）
    std::int64_t bytesRestored = 0;
};

namespace detail {

inline bool readAllBytes(const std::string &p, std::string &out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// 副本里的目标路径：<dstRoot>/<aa>/<hash>[.enc]（与主库同构，便于直接拷回去）
inline std::string replicaPathOf(const std::string &dstRoot, const std::string &hash,
                                 bool encrypted)
{
    return dstRoot + "/" + hash.substr(0, 2) + "/" + hash + (encrypted ? ".enc" : "");
}

// 校验主库里某个块的内容是否与其哈希一致。
//   明文块：直接对**落盘字节**算哈希（省一次解密）
//   加密块：落盘是密文，必须走 get() 解出明文再算（顺带验 GCM 完整性）
inline bool hashMismatch(ContentStore &store, const std::string &hash, bool encrypted)
{
    if (encrypted) {
        std::string plain, e;
        if (!store.get(hash, plain, e))
            return true;
        return Sha256::of(plain) != hash;
    }
    std::string raw;
    if (!readAllBytes(store.resolvePath(hash), raw))
        return true;
    return Sha256::of(raw) != hash;
}

}   // namespace detail

// 增量镜像内容库到 dstRoot。dryRun=true 时只统计不写。
inline bool mirrorBlobs(ContentStore &store, const std::string &dstRoot, bool dryRun,
                        BackupStats &st, std::string &err)
{
    std::vector<ContentStore::BlobInfo> blobs;
    if (!store.listBlobs(blobs, err))
        return false;

    const bool enc = store.encryptionEnabled();
    std::error_code ec;

    for (const ContentStore::BlobInfo &b : blobs) {
        ++st.scanned;
        const std::string dst = detail::replicaPathOf(dstRoot, b.hash, enc);

        if (fs::exists(dst, ec)) {
            ++st.skippedExisting;   // 增量：副本里已有就跳过（内容寻址 ⇒ 同名即同内容）
            continue;
        }

        // 新块才校验源：绝不把"唯一一份坏数据"变成两份坏数据
        if (detail::hashMismatch(store, b.hash, enc)) {
            ++st.skippedBad;
            continue;
        }
        if (dryRun) {
            ++st.copied;
            st.bytesCopied += b.size;
            continue;
        }

        fs::create_directories(fs::path(dst).parent_path(), ec);
        ec.clear();
        fs::copy_file(store.resolvePath(b.hash), dst, fs::copy_options::overwrite_existing, ec);
        if (ec)
            continue;   // 单个失败只跳过，不中断整次备份
        ++st.copied;
        st.bytesCopied += b.size;
    }
    return true;
}

// 从副本修复主库。hashes 是**应当存在**的块清单（权威来自元数据 —— 见 scrub.h 的同款说明：
// 靠枚举磁盘查不出"整块丢失"）。
inline bool restoreBlobs(ContentStore &store, const std::string &replicaRoot,
                         const std::vector<std::string> &hashes, bool dryRun, RestoreStats &st,
                         std::string &err)
{
    const bool enc = store.encryptionEnabled();
    std::error_code ec;

    for (const std::string &h : hashes) {
        ++st.checked;

        // 主库这份还好吗？（缺失 或 哈希不符 才需要修）
        const bool localOk = store.exists(h) && !detail::hashMismatch(store, h, enc);
        if (localOk) {
            ++st.alreadyOk;
            continue;
        }

        const std::string src = detail::replicaPathOf(replicaRoot, h, enc);
        if (!fs::exists(src, ec)) {
            ++st.missingInReplica;
            continue;
        }

        std::string raw;
        if (!detail::readAllBytes(src, raw)) {
            ++st.badReplica;
            continue;
        }

        // 明文库：可以对副本**先验后写**（落盘字节就是内容本身）——首选路径。
        if (!enc) {
            if (Sha256::of(raw) != h) {
                ++st.badReplica;
                continue;
            }
            if (dryRun) {
                ++st.restored;
                st.bytesRestored += static_cast<std::int64_t>(raw.size());
                continue;
            }
            const std::string dst = store.pathOf(h);
            fs::create_directories(fs::path(dst).parent_path(), ec);
            // ⚠️ 先删后拷，**不要**依赖 copy_file 的 overwrite_existing：
            //    实测在本工具链上它不生效，会直接报 "File exists" 而恢复静默失效。
            //    语义上这里本来就是"用副本替换掉那份已确认损坏的"，先删更直白。
            {
              std::error_code rec;
              fs::remove(dst, rec);
              rec.clear();
              fs::copy_file(src, dst, rec);
              if (rec) {
                ++st.copyFailed;   // 副本是好的，是**写回**失败 —— 别和"副本坏"混为一谈
                continue;
              }
            }
            ++st.restored;
            st.bytesRestored += static_cast<std::int64_t>(raw.size());
            continue;
        }

        // 加密库：落盘是密文，本层**无法在写回之前**自证（解密 key 在 ContentStore 内部，
        // 而 store.get() 只认主库自己的路径）。顺序只能退化为"先写回、再用 store.get 复核"。
        // 这个顺序之所以可接受：走到这里的前提是**主库这一份已经确认是坏的**，
        // 不存在"用坏副本毁掉好数据"的情形。
        // 复核不过就把写回的文件删掉 —— 明确暴露"这块没救"，绝不留下半吊子文件。
        if (dryRun) {
            ++st.restored;
            st.bytesRestored += static_cast<std::int64_t>(raw.size());
            continue;
        }
        {
            const std::string dst = store.pathOf(h);
            fs::create_directories(fs::path(dst).parent_path(), ec);
            {
              std::error_code rec;
              fs::remove(dst, rec);   // 同上：先删后拷
              rec.clear();
              fs::copy_file(src, dst, rec);
              if (rec) {
                ++st.copyFailed;
                continue;
              }
            }
            std::string plain, gerr;
            if (store.get(h, plain, gerr) && Sha256::of(plain) == h) {
                ++st.restored;
                st.bytesRestored += static_cast<std::int64_t>(raw.size());
            } else {
                fs::remove(dst, ec);
                ++st.badReplica;
            }
        }
    }
    return true;
}

}   // namespace blobbackup
}   // namespace cv

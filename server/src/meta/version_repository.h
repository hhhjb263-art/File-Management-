#pragma once

// meta/version_repository.h —— 文件历史版本快照（file_version / file_version_chunk）
//
// ── 语义（与 schema.h 顶部的说明一致，务必先读）──
//   file_version 只存**历史快照**；文件"当前内容"永远在 file_node 里（content_hash/
//   size/chunk_count + file_chunk）。因此：
//     · 新建文件不产生快照行；
//     · 每次内容被替换**之前**，把旧内容 captureCurrent() 成一条快照；
//     · 「当前版本号」= 历史最大 version_no + 1（无历史则为 1）。
//   这样 version_no 单调递增，回滚也只是"再追加一版"，不会出现版本号回退导致的歧义。
//
// ── 零拷贝 ──
//   快照与当前内容共享同一批 chunk 行，靠 chunk.ref_count 保命：谁还引用着，blob 就不会
//   被 GC 删掉。因此打版本几乎不额外占盘（只有新内容那部分才是真增量）。
//   ⚠️ 引用计数必须「按出现次数」增减 —— 同一文件内不同 seq 的分块内容可能完全相同
//      （重复数据块），用 `IN (SELECT ...)` 只会减一次，长期跑会漂出正偏移（泄漏）。
//      本文件一律用相关子查询 COUNT(*) 精确计数。
//
// ── 归属 ──
//   本仓储不自行鉴权，由路由层用 ownerVisible() 判定；但所有按 file_id 的查询都要求
//   调用方先确认归属，避免越权读写别人的版本。

#include <cstdint>
#include <string>
#include <vector>

#include "meta/db.h"

namespace cv {

struct VersionRow {
  std::int64_t id = 0;
  std::int64_t fileId = 0;
  std::int64_t versionNo = 1;
  std::string  contentHash;
  std::int64_t size = 0;
  std::int64_t chunkCount = 0;
  std::string  device;
  std::string  note;
  std::int64_t createdAt = 0;
};

class VersionRepository {
 public:
  explicit VersionRepository(Db& db) : db_(db) {}

  // 历史快照列表（version_no 降序；不含"当前内容"——当前内容由调用方从 FileRow 合成）
  bool listVersions(std::int64_t fileId, std::vector<VersionRow>& out, std::string& err);

  // 当前版本号 = 历史 MAX(version_no) + 1（无历史 → 1）
  bool currentVersionNo(std::int64_t fileId, std::int64_t& out, std::string& err);

  // 把 file_node 的当前内容快照成一条新历史版本（newVersionId 回传）。
  // ⚠️ **0 字节文件同样可以留档**（实测：harness E2）—— 实现只要求 file_node 行存在，
  //    对 chunk 清单为空没有任何拦截。所以**不要**照"内容为空就 return false"的思路去改：
  //    一旦拦住，空文件将永远无法留档 / 覆盖 / 回滚。仅"文件不存在"才返回 false。
  bool captureCurrent(std::int64_t fileId, const std::string& device, const std::string& note,
                      std::int64_t& newVersionId, std::string& err);

  bool findVersion(std::int64_t versionId, VersionRow& out, std::string& err);

  // 版本的分块序列（hashes 按 seq 升序）与对应大小（供回滚复用，走内容寻址零拷贝）
  bool versionChunks(std::int64_t versionId, std::vector<std::string>& hashes,
                     std::vector<std::size_t>& sizes, std::string& err);

  // 删除单个历史版本；orphans 回传引用归零的分块 hash（由调用方删 blob）
  bool deleteVersion(std::int64_t versionId, std::vector<std::string>& orphans, std::string& err);

  // 文件被**彻底删除**时清理其全部版本（不清理会在库里留下永远无人引用的快照行）。
  // ⚠️ 必须在 FileRepository::purgeFile() 之后调用：purgeFile 已清掉 file_chunk，
  //    此时只有版本侧还持有引用，本函数负责把这部分引用一并释放。
  bool purgeFileVersions(std::int64_t fileId, std::vector<std::string>& orphans,
                         std::string& err);

  // 保留最近 keep 条快照，更早的删除（keep <= 0 → 不做任何事）。orphans 同上。
  // 版本保留策略（PRD FR-4-2）：**条数与天数双限取先到者**。
  //   keep <= 0         → 不限条数（保留最新 keep 条）
  //   cutoffMillis <= 0 → 不限天数；否则 created_at < cutoff 即为过期
  //   ⚠️ 两者都 <= 0 时**什么都不做**（而不是清空）。
  // orphans 返回因本次删除而引用计数归零的分块哈希 —— 调用方负责 drop 对应 blob，
  // 否则版本行没了、磁盘却没释放（这正是"版本无限增长"之外的第二层浪费）。
  bool pruneVersions(std::int64_t fileId, std::int64_t keep, std::int64_t cutoffMillis,
                     std::vector<std::string>& orphans, std::string& err);

  bool countVersions(std::int64_t fileId, std::int64_t& out, std::string& err);

 private:
  // 递减「某版本集合」持有的分块引用计数，随后删净该集合的引用行与版本行。
  // whereClause 为限定条件（如 "version_id = ?" 或 "file_id = ?"，两种写法都已绑定）。
  bool releaseAndDelete(const char* sqlDecrement, const char* sqlDeleteLinks,
                        const char* sqlDeleteVersions, std::int64_t key,
                        std::vector<std::string>& orphans, std::string& err);

  Db& db_;
};

}  // namespace cv

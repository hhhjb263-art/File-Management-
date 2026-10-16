#pragma once

// meta/chunk_gc.h —— 分块行的「零引用回收」安全网（FileRepository 与 VersionRepository 共用）
//
// 背景：内容寻址库里，同一分块可能被多方引用 ——
//   · file_chunk          当前内容
//   · file_version_chunk  历史版本
// 每次引用 +1、解除引用 -1（chunk.ref_count）。只有当引用归零，blob 才能真正删除。
//
// 为什么做成共用函数：两个仓储都要做同一件事，而**判据必须完全一致**——
// 若版本侧漏判了 file_chunk（或反之），就会出现"另一方还在用的 blob 被删掉"的
// 静默数据损坏。把它收敛到一处，比在两处各写一遍可靠得多。
//
// 安全网（两道）：
//   1) ref_count <= 0
//   2) 且不存在任何 file_chunk / file_version_chunk 引用行
// 第 2 条是为了防"计数漂移"：即便计数因异常路径算错，只要还有人引用就不删。

#include <string>
#include <vector>

#include "meta/db.h"

namespace cv {

// 删除「引用计数归零且无任何引用」的分块行；被删行的 hash 通过 orphans 返回，
// 由调用方删除对应的 blob 文件（本函数只碰元数据，不碰磁盘）。
inline bool purgeZeroRefChunks(sqlite3* db, std::vector<std::string>& orphans,
                               std::string& err) {
  orphans.clear();
  if (!db) {
    err = "null db handle";
    return false;
  }

  {
    Stmt sel(db,
             "SELECT hash FROM chunk WHERE ref_count <= 0 "
             "AND hash NOT IN (SELECT chunk_hash FROM file_chunk) "
             "AND hash NOT IN (SELECT chunk_hash FROM file_version_chunk)",
             err);
    if (!sel.ok()) return false;
    while (true) {
      const int rc = sel.step(err);
      if (rc == SQLITE_DONE) break;
      if (rc != SQLITE_ROW) return false;
      orphans.push_back(sel.text(0));
    }
  }
  if (orphans.empty()) return true;

  Stmt del(db,
           "DELETE FROM chunk WHERE ref_count <= 0 "
           "AND hash NOT IN (SELECT chunk_hash FROM file_chunk) "
           "AND hash NOT IN (SELECT chunk_hash FROM file_version_chunk)",
           err);
  if (!del.ok()) return false;
  return del.step(err) == SQLITE_DONE;
}

}  // namespace cv

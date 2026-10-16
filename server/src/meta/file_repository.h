#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "meta/db.h"

namespace cv {

struct FileRow {
  std::int64_t id = 0;
  std::string name;
  std::int64_t size = 0;
  std::string contentHash;
  int chunkCount = 0;
  std::int64_t createdAt = 0;
  std::string dir;   // 所属目录（规范化相对路径，'' = 根目录）
  std::int64_t ownerId = 0;  // 归属用户；0 = 账号体系启用前历史数据（legacy 桶）
  std::int64_t deletedAt = 0; // 进回收站时间（毫秒）；0 = 未进站（或历史硬删除）
};

// 回收站里的一条目录条目（目录也进回收站，见 schema.h 的说明）
struct TrashDirRow {
  std::string path;
  std::int64_t deletedAt = 0;
};

// 文件元数据与分块清单的读写。所有操作都对 Db 句柄直接执行，
// 批量写入（insertFile）自带事务。
class FileRepository {
 public:
  explicit FileRepository(Db& db) : db_(db) {}

  // 秒传判定：同内容哈希是否已有文件
  bool findByContentHash(const std::string& hash, FileRow& out, std::string& err);
  bool findById(std::int64_t id, FileRow& out, std::string& err);
  // 同 findById，但**不过滤删除状态** —— 回收站里的文件也查得到。
  // 用途：彻底删除 / 清空回收站时需要拿到 name、dir 才能删掉磁盘镜像。
  bool findByIdAny(std::int64_t id, FileRow& out, std::string& err);
  bool listFiles(std::int64_t ownerId, std::vector<FileRow>& out, std::string& err);

  // 文件列表查询条件。
  //
  // 为什么要有过滤而不是"全量拉回客户端再筛"：本仓库原先在 SQL 里硬编码 `LIMIT 500`，
  // 而客户端是"拉全表 + 本地按 parent 过滤" —— 两者相乘的后果是：
  // **一个 owner 超过 500 个文件时，排在 500 名之外的目录会显示为空**。
  // 这不是性能问题，是正确性问题（用户的文件看得见、点不进去）。
  // 现在改为服务端过滤 + 分页，并由 total/hasMore 如实告知还有多少。
  struct FileListQuery {
    // ⚠️ 必须与 dirExact 分开置位：**根目录过滤时 dirExact 就是空串**，
    //    若拿"空串"当"不过滤"，根目录将永远列不出任何文件。
    bool         filterByDir = false;
    std::string  dirExact;
    // 前缀过滤（含子目录）：同步模块的 listUnderPath 用。
    bool         filterByUnder = false;
    std::string  underPrefix;
    std::int64_t limit = 0;    // 0 = 不限制（调用方应自行分页）
    std::int64_t offset = 0;
  };

  // outTotal 返回**不受 limit/offset 影响**的真实总数（hasMore 与界面显示都靠它）
  bool listFiles(std::int64_t ownerId, const FileListQuery& q, std::vector<FileRow>& out,
                 std::int64_t& outTotal, std::string& err);
  bool chunkHashesOf(std::int64_t fileId, std::vector<std::string>& out, std::string& err);
  // 取某文件的分块大小序列（owner 不限；仅用于秒传复用既有内容）
  bool chunkSizesOf(const std::vector<std::string>& hashes, std::vector<std::size_t>& out,
                    std::string& err);

  // 该内容分块是否仍被元数据引用（供启动期孤儿 blob 回收做判定）。
  // 按 blob 逐条查看似笨，但 chunk 行数可达千万级，一次性全读进内存会爆；
  // 这是启动期的运维动作，走主键索引的逐条查询完全够用。
  bool chunkExists(const std::string& hash, bool& out, std::string& err);

  // 巡检报告用：某个分块被哪些文件引用（返回 "目录/文件名" 标签，最多 maxLabels 条）。
  // 为什么需要它：巡检本身只能报"哪些**块**坏了"，而用户关心的是"哪些**文件**打不开了"。
  // 包含回收站里的文件 —— 它们的块同样被引用着，恢复后一样是坏的。
  // 巡检用：所有**仍被引用**的分块哈希（ref_count > 0）。
  // 这是"本该有什么"的权威清单 —— 巡检必须由它驱动，靠枚举磁盘查不出"整块丢失"。
  bool listReferencedChunkHashes(std::vector<std::string>& out, std::string& err);

  bool filesUsingChunk(const std::string& hash, int maxLabels,
                       std::vector<std::string>& out, std::string& err);

  // 按虚拟路径（目录 + 文件名）查找已记录文件（按路径下载用）。
  // ownerId 限定归属：ownerId == 0 仅匹配历史数据桶；传调用者真实 id 则只命中其名下文件。
  bool findByPath(std::int64_t ownerId, const std::string& dir, const std::string& name,
                  FileRow& out, std::string& err);

  // 写入文件记录 + 所属目录 + 分块清单 + 分块引用计数（同一事务）。
  // ownerId 为归属用户（>=1）；历史/legacy 数据写 0。
  bool insertFile(const std::string& name, const std::string& dir, std::int64_t ownerId,
                  std::int64_t size, const std::string& contentHash,
                  const std::vector<std::string>& chunkHashes,
                  const std::vector<std::size_t>& chunkSizes, std::int64_t& id,
                  std::string& err);

  // 目录登记：按 owner 隔离（ownerId, path 复合主键）。不存在则插入（created=true），
  // 已存在则幂等（created=false）。
  bool createDir(std::int64_t ownerId, const std::string& path, bool& created,
                 std::string& err);
  bool listDirs(std::int64_t ownerId, std::vector<std::string>& out, std::string& err);

  // 目录是否存在（未删除）。空串（根）恒为存在。
  bool dirExists(std::int64_t ownerId, const std::string& dir, bool& out, std::string& err);

  // 把文件移动到另一个目录。**纯元数据**：只改 file_dir.dir，不动 blob / 版本 / 标签 / 分块引用。
  // newDir 为规范化相对路径（'' = 根），必须已存在，且目标下不能有同名条目。
  bool moveFileToDir(std::int64_t ownerId, std::int64_t id, const std::string& newDir,
                     std::string& err);

  // 目录重命名 / 移动（同一份实现：只传不同的 to 即可）。
  // **纯元数据**，不遍历复制任何物理文件 —— 只改 dir_node 的 path 前缀与 file_dir 的 dir 前缀。
  // 拒绝：from 为空（根）/ to 落在 from 自身或其后代（否则整棵子树从树上脱落）/
  //       to 已存在 / 目标父目录不存在 / 同父下同名冲突。
  bool moveDir(std::int64_t ownerId, const std::string& from, const std::string& to,
               std::string& err);

  // 同 owner 同目录下是否已存在同名文件（excludeId > 0 时排除自身，用于重命名自检）
  bool nameExists(std::int64_t ownerId, const std::string& dir, const std::string& name,
                  std::int64_t excludeId, bool& exists, std::string& err);

  // 统计某 owner 的已用字节数（deleted=0 的文件 size 之和）；legacy(0) 也支持。
  bool sumSizeOfOwner(std::int64_t ownerId, std::int64_t& total, std::string& err);

  // 重命名（只改 file_node.name；物理镜像由调用方处理）
  bool renameFile(std::int64_t id, const std::string& newName, std::string& err);

  // 列出/删除引用计数归零的分块行（GC 用：回收历史残留）
  bool listOrphanChunkHashes(std::vector<std::string>& out, std::string& err);
  bool deleteChunkRows(const std::vector<std::string>& hashes, std::string& err);

  // 覆盖内容：保留 id 与名称，替换 content_hash/size 与分块清单（引用计数同步增减）；
  // 旧内容中引用计数归零的分块 hash 通过 orphanHashes 返回，由调用方删 blob。
  bool replaceContent(std::int64_t id, std::int64_t size, const std::string& contentHash,
                      const std::vector<std::string>& chunkHashes,
                      const std::vector<std::size_t>& chunkSizes,
                      std::vector<std::string>& orphanHashes, std::string& err);

  // 全部目录路径（已登记目录 dir_node + 文件所属目录 file_dir 及其所有祖先，
  // 含根 ''），用于文件树建树。祖先展开在 C++ 侧完成，不写递归 SQL。
  // 仅取归属于 ownerId 的目录（ownerId=-1 的 legacy 用 0 桶）。
  bool listDirsAll(std::int64_t ownerId, std::vector<std::string>& out, std::string& err);

  bool addChunkIfAbsent(const std::string& hash, std::int64_t size, std::string& err);

  // ======================= 回收站 =======================
  // 语义（与 schema.h 顶部说明一致）：
  //   trashFile   → deleted=1 + deleted_at=now，**保留** file_chunk/file_dir/ref_count（否则无法恢复）
  //   restoreFile → deleted=0 + deleted_at=0
  //   purgeFile   → 真正的物理清理（删 file_chunk、递减 ref_count、删 file_dir），不可恢复

  bool trashFile(std::int64_t id, std::int64_t nowMillis, std::string& err);
  // 恢复文件；outDir 回传原所属目录（供调用方判断目录是否还在 / 是否需要重建）
  bool restoreFile(std::int64_t id, std::string& outDir, std::string& err);
  // 彻底删除文件；orphanHashes 返回引用归零的分块（由调用方删 blob）
  bool purgeFile(std::int64_t id, std::vector<std::string>& orphanHashes, std::string& err);

  // 回收站列表：只返回「顶层条目」——文件的所属目录未被删除，且目录的父目录未被删除。
  // 这样删掉一个文件夹后，回收站里看到的是那个文件夹，而不是散开的 N 个文件。
  bool listTrash(std::int64_t ownerId, std::vector<FileRow>& files,
                 std::vector<TrashDirRow>& dirs, std::string& err);
  // 全部回收站文件 id（含被删除目录下的），供"清空回收站"批量清理
  bool listAllTrashedIds(std::int64_t ownerId, std::vector<std::int64_t>& out, std::string& err);
  // 到期（deleted_at 早于 deadline）的回收站文件 id —— 保留期自动清理用
  bool listExpiredTrashIds(std::int64_t ownerId, std::int64_t deadline,
                           std::vector<std::int64_t>& out, std::string& err);

  // ======================= 目录（含回收站状态） =======================
  // 目录是否处于「已删除」状态（不存在也返回 true —— 恢复时按"需要重建"处理）
  bool isDirDeleted(std::int64_t ownerId, const std::string& path, bool& deleted,
                    std::string& err);
  // 某目录及其全部子目录的 path（用于按前缀批量进站/恢复）
  bool listDirPathsUnder(std::int64_t ownerId, const std::string& path,
                         std::vector<std::string>& out, std::string& err);
  // 某目录（含全部子目录）下的全部文件 id
  bool listFileIdsUnderDir(std::int64_t ownerId, const std::string& path,
                           std::vector<std::int64_t>& out, std::string& err);
  // 某目录（含全部子目录）下**已进回收站**的文件行，且删除时间不早于 sinceMillis。
  // 三种用途共用：① 回收站列表里展示"这个文件夹占多少 / 含几个文件"；
  // ② 恢复文件夹时圈定"随它一起进站"的文件（早于它进站的单独删除项不被牵连）；
  // ③ 彻底删除文件夹时逐个清理。
  // sinceMillis <= 0 表示不限时间（该目录下全部已删除文件）。
  bool listTrashedFilesUnderDir(std::int64_t ownerId, const std::string& path,
                                std::int64_t sinceMillis, std::vector<FileRow>& out,
                                std::string& err);
  // 批量置目录删除状态
  bool setDirsTrashed(std::int64_t ownerId, const std::vector<std::string>& paths,
                      bool trashed, std::int64_t nowMillis, std::string& err);
  // 彻底删除目录行（purge 用）
  bool deleteDirRows(std::int64_t ownerId, const std::vector<std::string>& paths,
                     std::string& err);
  // 回收站里的顶层目录（父目录未删除）
  bool listTrashDirs(std::int64_t ownerId, std::vector<TrashDirRow>& out, std::string& err);
  // 某目录的直接子目录 path 列表（用于文件树排除已删除目录）
  bool listChildDirPaths(std::int64_t ownerId, const std::string& parent,
                         std::vector<std::string>& out, std::string& err);

 private:
  // 清理 ref_count <= 0 的分块行，返回其 hash（调用方删 blob）
  bool purgeZeroRefChunks(std::vector<std::string>& orphans, std::string& err);

  Db& db_;
};

std::int64_t nowMillis();

}  // namespace cv

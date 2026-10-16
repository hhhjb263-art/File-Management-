#pragma once

// meta/tag_repository.h —— 标签（tag）与文件-标签关联（file_tag）
//
// 语义要点：
//   · 标签**按 owner 隔离**：同名不同 owner 是两条记录（唯一索引是 owner_id+name）。
//   · 同名判定用 COLLATE NOCASE：`工作` / `工作` 大小写差异不算新标签。createTag 幂等。
//   · 删除标签必须级联清 file_tag —— 表没有外键，靠本文件保证，否则会留下悬空关联，
//     让"按标签查文件"返回已经不存在的 tag 命中行。
//   · setFileTags 是**全量替换**语义（先清空该文件的全部关联，再按 tagIds 重建）。
//     只接受属于 ownerId 的 tag，跨 owner 的 id 一律忽略（静默跳过，不报错）——
//     这样界面即使传了脏 id 也不会产生越权关联。
//   · 本仓储不自行鉴权；调用方须先确认文件归属。

#include <cstdint>
#include <string>
#include <vector>

#include "meta/db.h"

namespace cv {

struct TagRow {
  std::int64_t id = 0;
  std::int64_t ownerId = 0;
  std::string  name;
  std::string  color;
  std::int64_t createdAt = 0;
  std::int64_t fileCount = 0;  // 关联的**未删除**文件数（列表展示用）
};

class TagRepository {
 public:
  explicit TagRepository(Db& db) : db_(db) {}

  // 全部标签（按名称升序，NOCASE），带关联文件计数
  bool listTags(std::int64_t ownerId, std::vector<TagRow>& out, std::string& err);

  // 创建标签。同名（NOCASE）已存在 → created=false 且 out 为既有行（不视为错误）。
  bool createTag(std::int64_t ownerId, const std::string& name, const std::string& color,
                 TagRow& out, bool& created, std::string& err);

  bool findTag(std::int64_t ownerId, std::int64_t tagId, TagRow& out, std::string& err);

  // 改名 / 改色。name 非空且与同 owner 其它标签重名（NOCASE）→ 返回 false 且 err="name exists"
  bool renameTag(std::int64_t ownerId, std::int64_t tagId, const std::string& name,
                 const std::string& color, std::string& err);

  // 删除标签 + 级联清理 file_tag；标签不存在 / 不属于该 owner → false
  bool deleteTag(std::int64_t ownerId, std::int64_t tagId, std::string& err);

  // 某文件关联的标签（含计数）
  bool tagsOfFile(std::int64_t fileId, std::vector<TagRow>& out, std::string& err);

  // 全量替换某文件的标签集合；只接受属于 ownerId 的 tagId
  bool setFileTags(std::int64_t ownerId, std::int64_t fileId,
                   const std::vector<std::int64_t>& tagIds, std::string& err);

  // 命中任一指定标签的未删除文件 id（tagIds 为空 → 返回空集，**不是**全集）
  bool listFileIdsByTags(std::int64_t ownerId, const std::vector<std::int64_t>& tagIds,
                         std::vector<std::int64_t>& out, std::string& err);

 private:
  Db& db_;
};

}  // namespace cv

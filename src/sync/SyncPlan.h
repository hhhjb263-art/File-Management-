/****************************************************************************
 * sync/SyncPlan.h —— 同步对账决策（FR-3-2 方向 / FR-3-5 冲突 / FR-3-6 删除同步）
 *
 * 这是同步引擎的**大脑**，也是整个 F3 里唯一会静默毁掉用户数据的地方，
 * 所以刻意做成：**纯函数、无 IO、无 Qt 事件循环、只依赖 QtCore** —— 便于逐条 harness 验证。
 *
 * ── 三方比较 ──
 * 判定"哪一侧改了"不能直接比较本地与云端（两端时间戳/时区/精度都不同），必须引入第三方基准：
 *     lastSynced（上次同步成功时两端一致的状态，由 LocalIndex 持久化）
 *   · 本地 ≠ lastSynced  ⇒ 本地改了
 *   · 云端 ≠ lastSynced  ⇒ 云端改了
 *   · 只有一侧改 → 单向传播；两端都改 → 冲突
 *
 * ── 绝不静默覆盖（FR-3-5）──
 * 冲突时"较新者为主文件，较旧者另存为 `文件名(冲突-设备名-时间).ext`"，两份都留：
 *   · ConflictKeepLocal（本地较新）：主文件 = 本地版本（上传覆盖云端）；
 *     旧副本 = **云端那一份** ⇒ 引擎需先把它下载到 conflictPath 再覆盖云端。
 *   · ConflictKeepRemote（云端较新）：主文件 = 云端版本（下载覆盖本地）；
 *     旧副本 = **本地那一份** ⇒ 引擎只需先把它改名/复制成 conflictPath（本地操作，零网络）。
 *
 * ── 索引推进（重要，防"假同步成功"）──
 * 本模块**不**返回"预期的下次索引状态"。索引必须由引擎在**每个动作真正成功之后**，
 * 按**实际观察到的**结果逐条推进；动作失败则不得推进该路径 —— 否则下一次对账会以为
 * "已经同步过了"，把用户的改动当成无差异而静默丢弃。
 ****************************************************************************/
#pragma once

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVector>

#include "core/Types.h"   // SyncMode

namespace cv {

// 一端（本地或云端）在某一时刻的目录快照。key = relPath（'/' 分隔、相对同步根）
struct SyncSnapshot
{
    struct Item
    {
        bool      isDir = false;
        qint64    size  = 0;
        QDateTime mtime;   // UTC；本地为文件 mtime，云端为服务端 created_at
        QString   hash;    // 内容 SHA-256（目录为空；未知时留空）
        QString   rev;     // 云端版本号（本地侧留空；用于识别云端内容变化）
        QString   id;      // 云端文件/目录 id（本地侧留空）—— 执行阶段下载/删除都要用它，
                           // 决策本身不看这个字段，故它不参与 sameContentAs 的比较

        // 内容是否等价。目录只比"是不是目录"；文件优先比 hash，hash 缺失时退化为 size+mtime。
        bool sameContentAs(const Item &o) const;
    };

    QHash<QString, Item> items;

    const Item *find(const QString &relPath) const;
    bool        has(const QString &relPath) const { return items.contains(relPath); }
    bool        isEmpty() const { return items.isEmpty(); }
};

struct SyncAction
{
    enum Kind
    {
        UploadNew,          // 本地新增 → 上传
        UploadUpdate,       // 本地修改 → 覆盖上传
        DownloadNew,        // 云端新增 → 下载
        DownloadUpdate,     // 云端修改 → 覆盖下载
        DeleteRemote,       // 本地删了且云端未变 → 删云端（进回收站）
        DeleteLocal,        // 云端删了且本地未变 → 删本地
        ConflictKeepLocal,  // 两端都改且本地较新：上传本地为主 + 云端旧内容另存为 conflictPath
        ConflictKeepRemote, // 两端都改且云端较新：下载云端为主 + 本地旧内容另存为 conflictPath
        MakeLocalDir,       // 建本地目录
        MakeRemoteDir,      // 建云端目录
        Skip,               // 不动（含"被方向策略裁剪"与"两端已一致"）
    };

    Kind    kind = Skip;
    QString relPath;
    QString conflictPath;   // 仅冲突动作用：较旧那一份应保存的相对路径
    QString reason;         // 人读原因（进同步日志，FR-3-9）
};

struct SyncCounts
{
    int up = 0;
    int down = 0;
    int delLocal = 0;
    int delRemote = 0;
    int conflicts = 0;
    int dirs = 0;
    int skipped = 0;
};

struct SyncPlanInput
{
    SyncMode  mode = SyncMode::TwoWay;
    bool      deleteSyncEnabled = true;  // FR-3-6：关掉即为"仅解除同步、不删另一端"
    QString   deviceName;                // 冲突副本命名用
    QDateTime now;                       // 冲突副本命名用（注入以便测试可复现）
    SyncSnapshot lastSynced;
    SyncSnapshot local;
    SyncSnapshot remote;
    // 选择性同步（FR-3-7）：列出的**云端**目录不落地本地（条目相对 remotePath）。
    QStringList excludeDirs;
};

struct SyncPlanResult
{
    QVector<SyncAction> actions;   // 顺序：先目录后文件（保证建目录早于写文件）
    SyncCounts          counts;
};

class SyncPlan
{
public:
    static SyncPlanResult build(const SyncPlanInput &in);

    // 选择性同步判定：relPath 是否落在某个被排除的云端目录之下（含其自身）。
    // 条目会做归一化（去首尾空白与斜杠）；**必须按 '目录/' 前缀匹配**，
    // 否则排除 "docs" 会把 "docs2"、"docs.txt" 一并误伤。
    static bool isExcluded(const QStringList &excludeDirs, const QString &relPath);

    // `报告.docx` + `PC-01` + 2026-10-15T19:22 → `报告(冲突-PC-01-20261015-192200).docx`
    // 设备名中的非法路径字符会被替换为 '_'（Windows 上 : * ? " < > | 都是非法的）。
    static QString conflictName(const QString &name, const QString &device, const QDateTime &when);
};

} // namespace cv

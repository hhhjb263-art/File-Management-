/****************************************************************************
 * data/TransferModel.h —— 传输队列数据模型（QML ListView 数据源）
 *
 * 冻结契约：docs/整合实现契约.md §4 —— 角色名必须严格一致：
 *     taskId  fileName  kind  done  total  progress  state  message
 * 另附辅助角色 progressRatio（0.0..1.0）、stateText（中文展示）。
 *
 * 设计要点：
 *   1. 增量更新：新增用 beginInsertRows / endInsertRows，删除用
 *      beginRemoveRows / endRemoveRows，整表清空才用 beginResetModel；
 *   2. 高频进度更新走 dataChanged 并**只指定进度相关角色**，绝不整表刷新；
 *   3. 只在主线程读写（网络 / IO 结果经信号回主线程后再改模型），不引入 QMutex。
 *
 * 历史分组（加法式，不改任何既有角色名 / 语义）：
 *   当任务进入**终端态**（Completed / Failed / Canceled）时，该行从活动队列
 *   `m_tasks` 移出，并头插到 `m_history`（最新在前，上限 200，超出丢弃最旧）。
 *   历史数据不经角色暴露，而是由 `uploadHistoryAt()` / `downloadHistoryAt()`
 *   以 QVariantMap 返回（键名与角色名同义 + sizeText / displayPath）。
 *   约定：同一任务**只存在于 `m_tasks` 与 `m_history` 之一**。
 ****************************************************************************/
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QString>
#include <QVariantMap>

#include "Types.h"

namespace cv {

// 传输队列模型。
class TransferModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // 历史分组（加法式）：统一 NOTIFY historyChanged。
    Q_PROPERTY(int historyCount         READ historyCount         NOTIFY historyChanged)
    Q_PROPERTY(int uploadHistoryCount   READ uploadHistoryCount   NOTIFY historyChanged)
    Q_PROPERTY(int downloadHistoryCount READ downloadHistoryCount NOTIFY historyChanged)

public:
    // 角色枚举：前 8 个对应契约冻结角色名，其余为辅助角色。
    enum Roles {
        TaskIdRole = Qt::UserRole + 1, // "taskId"
        FileNameRole,                  // "fileName"
        KindRole,                      // "kind"（"upload" / "download"）
        DoneRole,                      // "done"（已传输字节）
        TotalRole,                     // "total"（总字节）
        ProgressRole,                  // "progress"（百分比 0..100）
        StateRole,                     // "state"（queued/hashing/running/paused/completed/failed/canceled）
        MessageRole,                   // "message"（错误 / 提示文案）
        ProgressRatioRole,             // "progressRatio"（0.0..1.0，供 ProgressBar.value）
        StateTextRole                  // "stateText"（中文展示）
    };
    Q_ENUM(Roles)

    explicit TransferModel(QObject *parent = nullptr);

    // ---- QAbstractListModel ----
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // ---- 增 / 改 / 删 ----
    // 增：任务不存在时追加一行（beginInsertRows）；已存在则退化为更新。
    void addTask(const TransferTask &task);
    // 改：按 id 覆盖式更新（存在 → 全角色 dataChanged；不存在 → 等同新增）。
    void upsertTask(const TransferTask &task);
    // 改（高频）：仅更新进度字段（dataChanged 指定 done/total/progress/progressRatio）。
    void updateProgress(const QString &taskId, qint64 done, qint64 total);
    // 改：更新状态与消息（dataChanged 指定 state/stateText/message）。
    void updateState(const QString &taskId, TransferState state, const QString &message);
    // 删：按 id 移除一行（beginRemoveRows）。
    void removeTask(const QString &taskId);
    // 整表替换（beginResetModel，去重且丢弃空 id 行）。
    void setTasks(const QList<TransferTask> &tasks);
    // 清空整表。
    Q_INVOKABLE void clear();

    // 供 QML 读取某行快照（越界返回空 map）。
    Q_INVOKABLE QVariantMap taskAt(int row) const;
    // 按 id 查行号（不存在返回 -1）。
    Q_INVOKABLE int indexOf(const QString &taskId) const;

    // 行数（供 QML `model.count` 绑定）。
    int count() const;

    // ---- 历史分组（加法式，均 NOTIFY historyChanged）----
    // 历史总数。
    int historyCount() const;
    // 历史中「已上传」（kind == TransferKind::Upload）的条数。
    int uploadHistoryCount() const;
    // 历史中「已下载」（kind == TransferKind::Download）的条数。
    int downloadHistoryCount() const;
    // 第 i 条「已上传」历史（0 = 最近一次；越界返回空 map）。
    Q_INVOKABLE QVariantMap uploadHistoryAt(int i) const;
    // 第 i 条「已下载」历史（0 = 最近一次；越界返回空 map）。
    Q_INVOKABLE QVariantMap downloadHistoryAt(int i) const;
    // 清空全部历史（仅清历史，不动活动队列）。
    Q_INVOKABLE void clearHistory();

signals:
    void countChanged();
    void historyChanged();

private:
    // TransferKind -> "upload" / "download"（机器可读 token）。
    static QString kindToken(TransferKind kind);
    // TransferState -> 英文 token（供 QML 逻辑判断）。
    static QString stateToken(TransferState state);
    // TransferState -> 中文展示（供界面显示）。
    static QString stateLabel(TransferState state);
    // 是否终端态（Completed / Failed / Canceled）—— 决定是否移入历史。
    static bool isTerminalState(TransferState state);

    // 线性查行号：传输任务数量级很小，避免维护索引哈希带来的移位复杂度。
    int rowOf(const QString &taskId) const;
    // 线性查历史索引（不存在返回 -1）——用于维护「同一 id 不同时存在于两处」。
    int historyIndexOf(const QString &taskId) const;
    // 发出整行（全角色）dataChanged。
    void emitRowChanged(int row);

    // 按 kind 取第 i 条历史（越界返回空 map）。
    QVariantMap historyAtOfKind(TransferKind kind, int i) const;
    // 把一条历史任务转成 QVariantMap（键名与角色同义 + sizeText / displayPath）。
    QVariantMap historyMapFor(const TransferTask &task) const;

    // 历史上限（超出丢弃最旧，即尾部）。
    static constexpr int kHistoryLimit = 200;

    QList<TransferTask> m_tasks;
    QList<TransferTask> m_history; // 头插：索引 0 = 最新
};

} // namespace cv

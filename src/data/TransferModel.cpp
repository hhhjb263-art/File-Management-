/****************************************************************************
 * data/TransferModel.cpp —— 传输队列数据模型实现
 ****************************************************************************/
#include "TransferModel.h"

#include <QModelIndex>
#include <QSet>

namespace cv {

// ============================ 映射辅助 ============================

QString TransferModel::kindToken(TransferKind kind)
{
    return kind == TransferKind::Download ? QStringLiteral("download")
                                          : QStringLiteral("upload");
}

QString TransferModel::stateToken(TransferState state)
{
    // 机器可读 token，QML 用它做逻辑分支（颜色 / 可否取消）。
    switch (state) {
    case TransferState::Queued:    return QStringLiteral("queued");
    case TransferState::Hashing:   return QStringLiteral("hashing");
    case TransferState::Running:   return QStringLiteral("running");
    case TransferState::Paused:    return QStringLiteral("paused");
    case TransferState::Completed: return QStringLiteral("completed");
    case TransferState::Failed:    return QStringLiteral("failed");
    case TransferState::Canceled:  return QStringLiteral("canceled");
    }
    return QStringLiteral("queued");
}

QString TransferModel::stateLabel(TransferState state)
{
    // 中文展示文案。
    switch (state) {
    case TransferState::Queued:    return QStringLiteral("排队中");
    case TransferState::Hashing:   return QStringLiteral("校验中");
    case TransferState::Running:   return QStringLiteral("传输中");
    case TransferState::Paused:    return QStringLiteral("已暂停");
    case TransferState::Completed: return QStringLiteral("已完成");
    case TransferState::Failed:    return QStringLiteral("失败");
    case TransferState::Canceled:  return QStringLiteral("已取消");
    }
    return QStringLiteral("排队中");
}

// ============================ 构造 / 查询 ============================

TransferModel::TransferModel(QObject *parent) : QAbstractListModel(parent) {}

int TransferModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return int(m_tasks.size());
}

QVariant TransferModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }
    const int row = index.row();
    if (row < 0 || row >= int(m_tasks.size())) {
        return QVariant();
    }
    const TransferTask &t = m_tasks.at(row);
    switch (role) {
    case TaskIdRole:        return t.id;
    case FileNameRole:      return t.fileName;
    case KindRole:          return kindToken(t.kind);
    case DoneRole:          return t.done;
    case TotalRole:         return t.total;
    case ProgressRole:      return t.percent(); // 百分比 0..100
    case StateRole:         return stateToken(t.state);
    case MessageRole:       return t.error;
    case ProgressRatioRole: return t.total > 0 ? double(t.done) / double(t.total) : 0.0;
    case StateTextRole:     return stateLabel(t.state);
    case Qt::DisplayRole:   return t.fileName; // 兜底
    default:                return QVariant();
    }
}

QHash<int, QByteArray> TransferModel::roleNames() const
{
    // 键名严格对齐契约 §4：taskId fileName kind done total progress state message。
    QHash<int, QByteArray> roles;
    roles[TaskIdRole]        = QByteArrayLiteral("taskId");
    roles[FileNameRole]      = QByteArrayLiteral("fileName");
    roles[KindRole]          = QByteArrayLiteral("kind");
    roles[DoneRole]          = QByteArrayLiteral("done");
    roles[TotalRole]         = QByteArrayLiteral("total");
    roles[ProgressRole]      = QByteArrayLiteral("progress");
    roles[StateRole]         = QByteArrayLiteral("state");
    roles[MessageRole]       = QByteArrayLiteral("message");
    roles[ProgressRatioRole] = QByteArrayLiteral("progressRatio");
    roles[StateTextRole]     = QByteArrayLiteral("stateText");
    return roles;
}

int TransferModel::indexOf(const QString &taskId) const
{
    return rowOf(taskId);
}

int TransferModel::count() const
{
    return int(m_tasks.size());
}

QVariantMap TransferModel::taskAt(int row) const
{
    QVariantMap map;
    if (row < 0 || row >= int(m_tasks.size())) {
        return map;
    }
    const TransferTask &t = m_tasks.at(row);
    map.insert(QStringLiteral("taskId"), t.id);
    map.insert(QStringLiteral("fileName"), t.fileName);
    map.insert(QStringLiteral("kind"), kindToken(t.kind));
    map.insert(QStringLiteral("done"), t.done);
    map.insert(QStringLiteral("total"), t.total);
    map.insert(QStringLiteral("progress"), t.percent());
    map.insert(QStringLiteral("progressRatio"),
               t.total > 0 ? double(t.done) / double(t.total) : 0.0);
    map.insert(QStringLiteral("state"), stateToken(t.state));
    map.insert(QStringLiteral("stateText"), stateLabel(t.state));
    map.insert(QStringLiteral("message"), t.error);
    map.insert(QStringLiteral("localPath"), t.localPath);
    map.insert(QStringLiteral("remotePath"), t.remotePath);
    return map;
}

// ============================ 增 / 改 / 删 ============================

void TransferModel::addTask(const TransferTask &task)
{
    // 「增」语义：不存在则插入；已存在则退化为更新（避免重复行）。
    upsertTask(task);
}

void TransferModel::upsertTask(const TransferTask &task)
{
    if (task.id.isEmpty()) {
        return; // 无 id 的任务无法被定位更新 / 删除，直接忽略
    }
    const int existing = rowOf(task.id);
    if (existing >= 0) {
        m_tasks[existing] = task;
        emitRowChanged(existing); // 全角色 dataChanged（单行，非整表刷新）
        return;
    }
    const int row = int(m_tasks.size());
    beginInsertRows(QModelIndex(), row, row);
    m_tasks.append(task);
    endInsertRows();
    emit countChanged();
}

void TransferModel::updateProgress(const QString &taskId, qint64 done, qint64 total)
{
    const int row = rowOf(taskId);
    if (row < 0) {
        return;
    }
    TransferTask &t = m_tasks[row];
    t.done = done;
    if (total >= 0) {
        t.total = total; // 负值表示「不改动 total」
    }
    // 只刷新进度相关角色，绝不整表刷新。
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx,
                     QList<int>{DoneRole, TotalRole, ProgressRole, ProgressRatioRole});
}

void TransferModel::updateState(const QString &taskId, TransferState state, const QString &message)
{
    const int row = rowOf(taskId);
    if (row < 0) {
        return;
    }
    TransferTask &t = m_tasks[row];
    t.state = state;
    t.error = message;
    // 只刷新状态相关角色。
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, QList<int>{StateRole, StateTextRole, MessageRole});
}

void TransferModel::removeTask(const QString &taskId)
{
    const int row = rowOf(taskId);
    if (row < 0) {
        return;
    }
    beginRemoveRows(QModelIndex(), row, row);
    m_tasks.removeAt(row);
    endRemoveRows();
    emit countChanged();
}

void TransferModel::setTasks(const QList<TransferTask> &tasks)
{
    beginResetModel();
    m_tasks.clear();
    m_tasks.reserve(tasks.size());
    QSet<QString> seen;
    for (const TransferTask &t : tasks) {
        if (t.id.isEmpty() || seen.contains(t.id)) {
            continue; // 丢弃空 id 与重复 id，保证 id 唯一可定位
        }
        seen.insert(t.id);
        m_tasks.append(t);
    }
    endResetModel();
    emit countChanged();
}

void TransferModel::clear()
{
    if (m_tasks.isEmpty()) {
        return;
    }
    beginResetModel();
    m_tasks.clear();
    endResetModel();
    emit countChanged();
}

// ============================ 内部 ============================

int TransferModel::rowOf(const QString &taskId) const
{
    if (taskId.isEmpty()) {
        return -1;
    }
    const int n = int(m_tasks.size());
    for (int i = 0; i < n; ++i) {
        if (m_tasks.at(i).id == taskId) {
            return i;
        }
    }
    return -1;
}

void TransferModel::emitRowChanged(int row)
{
    if (row < 0 || row >= int(m_tasks.size())) {
        return;
    }
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx); // 无 roles 参数 == 全角色
}

} // namespace cv

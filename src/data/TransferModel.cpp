/****************************************************************************
 * data/TransferModel.cpp —— 传输队列数据模型实现
 ****************************************************************************/
#include "TransferModel.h"

#include "Util.h" // cv::Util::humanSize（历史 sizeText 复用全项目统一格式化）

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

bool TransferModel::isTerminalState(TransferState state)
{
    // 终端态 —— 任务已结束，应移出活动队列并归入历史分组。
    return state == TransferState::Completed
        || state == TransferState::Failed
        || state == TransferState::Canceled;
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

    // 不变量（见 TransferModel.h）：同一任务只存在于 m_tasks 与 m_history 之一。
    // 若同 id 已在历史中（如终端态之后又 addTask 同 id），先从历史移除，避免两处并存。
    const int histIdx = historyIndexOf(task.id);
    if (histIdx >= 0) {
        m_history.removeAt(histIdx);
        emit historyChanged();
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
        // 已不在活动队列（未建行 / 已移入历史）→ 静默忽略，不误插入新行。
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
        // 已不在活动队列（可能已移入历史）→ 静默忽略，绝不误插入新行。
        // TransferController 在任务结束后仍可能收到迟到信号，这里必须容错。
        return;
    }

    // 先就地写入新状态 / 消息，确保随后移入历史时携带的是「更新后」的完整快照。
    TransferTask &t = m_tasks[row];
    t.state = state;
    t.error = message;

    if (isTerminalState(state)) {
        // 终端态：从活动队列移出 → 头插到历史（最新在前），保证活动队列不被已完成任务堆积。
        const TransferTask snapshot = t; // 值拷贝：removeAt 后引用失效，须先取快照
        beginRemoveRows(QModelIndex(), row, row);
        m_tasks.removeAt(row);
        endRemoveRows();
        emit countChanged();

        m_history.prepend(snapshot);
        while (m_history.size() > kHistoryLimit) {
            m_history.removeLast(); // 超上限丢弃最旧（尾部）
        }
        emit historyChanged();
        // 行已不在 m_tasks，不再对该行发 dataChanged。
        return;
    }

    // 非终端态（排队中 / 校验中 / 传输中 / 已暂停）：只刷新状态相关角色。
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, QList<int>{StateRole, StateTextRole, MessageRole});
}

void TransferModel::removeTask(const QString &taskId)
{
    const int row = rowOf(taskId);
    if (row < 0) {
        // 已不在活动队列 → 静默忽略（迟到 / 重复删除均安全）。
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

// ============================ 历史分组 ============================

int TransferModel::historyCount() const
{
    return int(m_history.size());
}

int TransferModel::uploadHistoryCount() const
{
    int n = 0;
    for (const TransferTask &t : m_history) {
        if (t.kind == TransferKind::Upload) {
            ++n;
        }
    }
    return n;
}

int TransferModel::downloadHistoryCount() const
{
    int n = 0;
    for (const TransferTask &t : m_history) {
        if (t.kind == TransferKind::Download) {
            ++n;
        }
    }
    return n;
}

QVariantMap TransferModel::uploadHistoryAt(int i) const
{
    return historyAtOfKind(TransferKind::Upload, i);
}

QVariantMap TransferModel::downloadHistoryAt(int i) const
{
    return historyAtOfKind(TransferKind::Download, i);
}

void TransferModel::clearHistory()
{
    if (m_history.isEmpty()) {
        return;
    }
    m_history.clear();
    emit historyChanged();
}

QVariantMap TransferModel::historyAtOfKind(TransferKind kind, int i) const
{
    if (i < 0) {
        return QVariantMap(); // 越界（负索引）
    }
    // 历史为头插（索引 0 = 最新），按 kind 过滤后第 i 条即调用方所要。
    int seen = 0;
    for (const TransferTask &t : m_history) {
        if (t.kind != kind) {
            continue;
        }
        if (seen == i) {
            return historyMapFor(t);
        }
        ++seen;
    }
    return QVariantMap(); // 越界
}

QVariantMap TransferModel::historyMapFor(const TransferTask &task) const
{
    QVariantMap map;
    // 键名与角色名同义、同类型（QML 可与活动队列共用同一套绑定 / 判色逻辑）。
    map.insert(QStringLiteral("taskId"), task.id);
    map.insert(QStringLiteral("fileName"), task.fileName);
    map.insert(QStringLiteral("kind"), kindToken(task.kind));
    map.insert(QStringLiteral("done"), task.done);
    map.insert(QStringLiteral("total"), task.total);
    map.insert(QStringLiteral("progress"), task.percent());
    map.insert(QStringLiteral("progressRatio"),
               task.total > 0 ? double(task.done) / double(task.total) : 0.0);
    map.insert(QStringLiteral("state"), stateToken(task.state));
    map.insert(QStringLiteral("stateText"), stateLabel(task.state));
    map.insert(QStringLiteral("message"), task.error);
    // 历史附加字段：
    //   sizeText    —— 已完成字节的人类可读串（复用 Util::humanSize，保证全项目文案一致）
    //   displayPath —— 上传 = 远端目标目录（remotePath）；下载 = 本地保存路径（localPath）
    map.insert(QStringLiteral("sizeText"), Util::humanSize(task.done));
    map.insert(QStringLiteral("displayPath"),
               task.kind == TransferKind::Download ? task.localPath : task.remotePath);
    return map;
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

int TransferModel::historyIndexOf(const QString &taskId) const
{
    if (taskId.isEmpty()) {
        return -1;
    }
    const int n = int(m_history.size());
    for (int i = 0; i < n; ++i) {
        if (m_history.at(i).id == taskId) {
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

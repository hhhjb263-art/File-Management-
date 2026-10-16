/****************************************************************************
 * data/SyncPairModel.cpp —— 同步对数据模型（真实实现）
 *
 * 职责很窄：把 SyncController 维护的同步对列表**呈现**给 QML。
 * 本模型不读写 Settings、不碰网络、不做校验 —— 那些都在 SyncController 里，
 * 这样"数据"与"策略"分开，模型本身是纯粹的只读视图。
 ****************************************************************************/
#include "SyncPairModel.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

namespace cv {

SyncPairModel::SyncPairModel(QObject *parent) : QAbstractListModel(parent) {}

QString SyncPairModel::statusText(int status)
{
    switch (SyncStatus(status)) {
    case SyncStatus::Idle:     return QStringLiteral("已同步");
    case SyncStatus::Watching: return QStringLiteral("监听中");
    case SyncStatus::Syncing:  return QStringLiteral("同步中");
    case SyncStatus::Conflict: return QStringLiteral("有冲突");
    case SyncStatus::Error:    return QStringLiteral("出错");
    case SyncStatus::Paused:   return QStringLiteral("已暂停");
    case SyncStatus::Disabled: return QStringLiteral("未启用");
    }
    return QStringLiteral("未知");
}

QString SyncPairModel::modeText(int mode)
{
    switch (SyncMode(mode)) {
    case SyncMode::TwoWay:       return QStringLiteral("双向同步");
    case SyncMode::UploadOnly:   return QStringLiteral("仅上传");
    case SyncMode::DownloadOnly: return QStringLiteral("仅下载");
    }
    return QStringLiteral("双向同步");
}

void SyncPairModel::setPairs(const QVector<SyncPair> &pairs)
{
    beginResetModel();
    m_pairs = pairs;
    endResetModel();
    emit countChanged();
}

void SyncPairModel::updatePair(const SyncPair &pair)
{
    for (int i = 0; i < m_pairs.size(); ++i) {
        if (m_pairs[i].id != pair.id)
            continue;
        m_pairs[i] = pair;
        const QModelIndex idx = index(i, 0);
        emit dataChanged(idx, idx);
        return;
    }
    // 找不到就追加：控制器新增同步对后即便忘了 reset 也不会丢展示
    beginInsertRows(QModelIndex(), m_pairs.size(), m_pairs.size());
    m_pairs.append(pair);
    endInsertRows();
    emit countChanged();
}

void SyncPairModel::removePair(const QString &pairId)
{
    for (int i = 0; i < m_pairs.size(); ++i) {
        if (m_pairs[i].id != pairId)
            continue;
        beginRemoveRows(QModelIndex(), i, i);
        m_pairs.remove(i);
        endRemoveRows();
        emit countChanged();
        return;
    }
}

const SyncPair *SyncPairModel::pairAt(int row) const
{
    if (row < 0 || row >= m_pairs.size())
        return nullptr;
    return &m_pairs[row];
}

int SyncPairModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_pairs.size();
}

int SyncPairModel::count() const
{
    return m_pairs.size();
}

QVariant SyncPairModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_pairs.size())
        return QVariant();
    const SyncPair &p = m_pairs[index.row()];

    switch (role) {
    case IdRole:            return p.id;
    case LocalPathRole:     return p.localPath;
    case RemotePathRole:    return p.remotePath;
    case ModeRole:          return int(p.mode);
    case EnabledRole:       return p.enabled;
    case StatusRole:        return int(p.status);
    case LastErrorRole:     return p.lastError;
    case LastSyncRole:      return p.lastSync;
    case PendingCountRole:  return p.pendingCount;
    case ConflictCountRole: return p.conflictCount;

    case StatusTextRole:    return statusText(int(p.status));
    case ModeTextRole:      return modeText(int(p.mode));
    case ExcludeTextRole:   return p.excludeDirs.join(QLatin1Char('\n'));
    case ExcludeCountRole:  return p.excludeDirs.size();
    case LastSyncTextRole:
        return p.lastSync.isValid()
                   ? p.lastSync.toLocalTime().toString(QStringLiteral("MM-dd HH:mm:ss"))
                   : QStringLiteral("从未同步");
    case CanSyncRole:
        // 本地目录被删/改名是最常见的"同步静默失效"原因 —— 界面上必须能看出来，
        // 否则用户会以为什么都没发生
        return p.enabled && QFileInfo(QDir(p.localPath).absolutePath()).isDir();
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> SyncPairModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[IdRole]            = QByteArrayLiteral("id");
    roles[LocalPathRole]     = QByteArrayLiteral("localPath");
    roles[RemotePathRole]    = QByteArrayLiteral("remotePath");
    roles[ModeRole]          = QByteArrayLiteral("mode");
    roles[EnabledRole]       = QByteArrayLiteral("enabled");
    roles[StatusRole]        = QByteArrayLiteral("status");
    roles[LastErrorRole]     = QByteArrayLiteral("lastError");
    roles[LastSyncRole]      = QByteArrayLiteral("lastSync");
    roles[PendingCountRole]  = QByteArrayLiteral("pendingCount");
    roles[ConflictCountRole] = QByteArrayLiteral("conflictCount");
    roles[StatusTextRole]    = QByteArrayLiteral("statusText");
    roles[ModeTextRole]      = QByteArrayLiteral("modeText");
    roles[LastSyncTextRole]  = QByteArrayLiteral("lastSyncText");
    roles[CanSyncRole]       = QByteArrayLiteral("canSync");
    roles[ExcludeTextRole]   = QByteArrayLiteral("excludeText");
    roles[ExcludeCountRole]  = QByteArrayLiteral("excludeCount");
    return roles;
}

} // namespace cv

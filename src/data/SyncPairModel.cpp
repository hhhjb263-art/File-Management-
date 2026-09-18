/****************************************************************************
 * data/SyncPairModel.cpp —— 同步对数据模型（骨架实现）
 * 服务端不支持同步 → rowCount() 恒为 0；仅暴露约定角色名。
 ****************************************************************************/
#include "SyncPairModel.h"

namespace cv {

SyncPairModel::SyncPairModel(QObject *parent) : QAbstractListModel(parent) {}

int SyncPairModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 0; // 骨架：无数据行
}

QVariant SyncPairModel::data(const QModelIndex &index, int role) const
{
    Q_UNUSED(index);
    Q_UNUSED(role);
    return QVariant(); // 骨架：无数据
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
    return roles;
}

int SyncPairModel::count() const
{
    return 0;
}

} // namespace cv

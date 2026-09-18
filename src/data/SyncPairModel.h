/****************************************************************************
 * data/SyncPairModel.h —— 同步对数据模型（骨架）
 *
 * 服务端不支持同步（契约 §1 Unsupported 清单），本模型保留骨架：
 *   rowCount() == 0，roleNames() 暴露约定角色名，UI 入口置灰。
 * 角色名与 core/Types.h 的 SyncPair 字段一一对应，便于后续启用时零迁移。
 ****************************************************************************/
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>

#include "Types.h"

namespace cv {

class SyncPairModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1, // "id"
        LocalPathRole,             // "localPath"
        RemotePathRole,            // "remotePath"
        ModeRole,                  // "mode"
        EnabledRole,               // "enabled"
        StatusRole,                // "status"
        LastErrorRole,             // "lastError"
        LastSyncRole,              // "lastSync"
        PendingCountRole,          // "pendingCount"
        ConflictCountRole          // "conflictCount"
    };
    Q_ENUM(Roles)

    explicit SyncPairModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;

signals:
    void countChanged();
};

} // namespace cv

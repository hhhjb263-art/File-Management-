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
#include <QVector>

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
        ConflictCountRole,         // "conflictCount"
        // ↓ 本次实现新增的可展示角色（加法式，不影响既有使用方）
        StatusTextRole,            // "statusText" 中文状态
        ModeTextRole,              // "modeText"   中文方向
        LastSyncTextRole,          // "lastSyncText" 人类可读时间，空则"从未同步"
        CanSyncRole,               // "canSync"    本地目录仍存在且已启用
        ExcludeTextRole,           // "excludeText" 选择性同步的排除目录（每行一个，供编辑框回填）
        ExcludeCountRole           // "excludeCount" 排除目录个数（供徽标显示）
    };
    Q_ENUM(Roles)

    explicit SyncPairModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;

    // ---- 写入接口（由 SyncController 调用；模型本身不碰 Settings/网络）----
    void setPairs(const QVector<SyncPair> &pairs);
    void updatePair(const SyncPair &pair);   // 按 id 就地更新；找不到则忽略
    void removePair(const QString &pairId);
    const SyncPair *pairAt(int row) const;

    // 角色值 → 中文（QML 里也能用 Sync.statusText() 拿到同样的文案，两处口径一致）
    static QString statusText(int status);
    static QString modeText(int mode);

signals:
    void countChanged();

private:
    QVector<SyncPair> m_pairs;
};

} // namespace cv

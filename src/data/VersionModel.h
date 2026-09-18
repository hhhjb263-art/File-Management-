/****************************************************************************
 * data/VersionModel.h —— 历史版本数据模型（骨架）
 *
 * 服务端不支持版本管理（契约 §1 Unsupported 清单），本模型保留骨架：
 *   rowCount() == 0，roleNames() 暴露约定角色名，UI 入口置灰。
 * 角色名与 core/Types.h 的 FileVersion 字段一一对应，便于后续启用时零迁移。
 ****************************************************************************/
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>

#include "Types.h"

namespace cv {

class VersionModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1, // "id"
        FileIdRole,                // "fileId"
        VersionRole,               // "version"
        SizeRole,                  // "size"
        CreatedRole,               // "created"
        DeviceRole,                // "device"
        NoteRole,                  // "note"
        CurrentRole                // "current"
    };
    Q_ENUM(Roles)

    explicit VersionModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;

signals:
    void countChanged();
};

} // namespace cv

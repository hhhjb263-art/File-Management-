/****************************************************************************
 * data/VersionModel.h —— 历史版本数据模型（QML ListView 数据源）
 *
 * 角色名与 core/Types.h 的 FileVersion 字段一一对应（骨架期定的，启用后零迁移），
 * 另追加两个纯展示角色（sizeText / time），免去 QML 侧重复格式化。
 * 数据由 VersionController 拉取后整批灌入（setVersions），模型自身不做 IO。
 ****************************************************************************/
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QVariantMap>
#include <QVector>

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
        VersionRole,               // "version"（第几版）
        SizeRole,                  // "size"（原始字节）
        CreatedRole,               // "created"（原始毫秒）
        DeviceRole,                // "device"
        NoteRole,                  // "note"
        CurrentRole,               // "current"
        // ---- 追加的展示角色（不破坏既有契约）----
        SizeTextRole,              // "sizeText"（人类可读，如 "1.23 MB"）
        TimeRole                   // "time"（"yyyy-MM-dd HH:mm:ss"）
    };
    Q_ENUM(Roles)

    explicit VersionModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;

    void setVersions(const QVector<FileVersion> &versions);
    Q_INVOKABLE QVariantMap at(int row) const;
    Q_INVOKABLE void clear();

signals:
    void countChanged();

private:
    QVector<FileVersion> m_versions;
};

} // namespace cv

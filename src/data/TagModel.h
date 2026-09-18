/****************************************************************************
 * data/TagModel.h —— 标签数据模型（骨架）
 *
 * 服务端不支持标签（契约 §1 Unsupported 清单），本模型保留骨架：
 *   rowCount() == 0，roleNames() 暴露约定角色名，UI 入口置灰。
 * 角色名与 core/Types.h 的 Tag 字段一一对应，便于后续启用时零迁移。
 ****************************************************************************/
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>

#include "Types.h"

namespace cv {

class TagModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1, // "id"
        NameRole,                  // "name"
        ColorRole,                 // "color"
        CountRole                  // "count"
    };
    Q_ENUM(Roles)

    explicit TagModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;

signals:
    void countChanged();
};

} // namespace cv

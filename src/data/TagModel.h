/****************************************************************************
 * data/TagModel.h —— 标签列表模型（QML ListView 数据源）
 *
 * 角色名与 core/Types.h 的 Tag 字段一一对应（骨架期就这么定的，启用后零迁移）。
 * 数据由 TagController 拉取后整批灌入（setTags），模型自身不做任何网络 / IO，
 * 只在主线程读写 —— 与 FileListModel 同一套约定。
 ****************************************************************************/
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QVariantMap>
#include <QVector>

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
        CountRole                  // "count"（关联的未删除文件数）
    };
    Q_ENUM(Roles)

    explicit TagModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;

    // 整批替换（beginResetModel / endResetModel）
    void setTags(const QVector<Tag> &tags);

    // 供右键菜单 / 详情读取某行（越界返回空 map）。键：id/name/color/count。
    Q_INVOKABLE QVariantMap at(int row) const;

    Q_INVOKABLE void clear();

signals:
    void countChanged();

private:
    QVector<Tag> m_tags;
};

} // namespace cv

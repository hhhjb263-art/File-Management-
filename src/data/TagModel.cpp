/****************************************************************************
 * data/TagModel.cpp —— 标签数据模型（骨架实现）
 * 服务端不支持标签 → rowCount() 恒为 0；仅暴露约定角色名。
 ****************************************************************************/
#include "TagModel.h"

namespace cv {

TagModel::TagModel(QObject *parent) : QAbstractListModel(parent) {}

int TagModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 0; // 骨架：无数据行
}

QVariant TagModel::data(const QModelIndex &index, int role) const
{
    Q_UNUSED(index);
    Q_UNUSED(role);
    return QVariant(); // 骨架：无数据
}

QHash<int, QByteArray> TagModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[IdRole]    = QByteArrayLiteral("id");
    roles[NameRole]  = QByteArrayLiteral("name");
    roles[ColorRole] = QByteArrayLiteral("color");
    roles[CountRole] = QByteArrayLiteral("count");
    return roles;
}

int TagModel::count() const
{
    return 0;
}

} // namespace cv

/****************************************************************************
 * data/VersionModel.cpp —— 历史版本数据模型（骨架实现）
 * 服务端不支持版本管理 → rowCount() 恒为 0；仅暴露约定角色名。
 ****************************************************************************/
#include "VersionModel.h"

namespace cv {

VersionModel::VersionModel(QObject *parent) : QAbstractListModel(parent) {}

int VersionModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 0; // 骨架：无数据行
}

QVariant VersionModel::data(const QModelIndex &index, int role) const
{
    Q_UNUSED(index);
    Q_UNUSED(role);
    return QVariant(); // 骨架：无数据
}

QHash<int, QByteArray> VersionModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[IdRole]      = QByteArrayLiteral("id");
    roles[FileIdRole]  = QByteArrayLiteral("fileId");
    roles[VersionRole] = QByteArrayLiteral("version");
    roles[SizeRole]    = QByteArrayLiteral("size");
    roles[CreatedRole] = QByteArrayLiteral("created");
    roles[DeviceRole]  = QByteArrayLiteral("device");
    roles[NoteRole]    = QByteArrayLiteral("note");
    roles[CurrentRole] = QByteArrayLiteral("current");
    return roles;
}

int VersionModel::count() const
{
    return 0;
}

} // namespace cv

/****************************************************************************
 * data/ShareLinkModel.cpp —— 分享链接数据模型（骨架实现）
 * 服务端不支持分享 → rowCount() 恒为 0；仅暴露约定角色名。
 ****************************************************************************/
#include "ShareLinkModel.h"

namespace cv {

ShareLinkModel::ShareLinkModel(QObject *parent) : QAbstractListModel(parent) {}

int ShareLinkModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 0; // 骨架：无数据行
}

QVariant ShareLinkModel::data(const QModelIndex &index, int role) const
{
    Q_UNUSED(index);
    Q_UNUSED(role);
    return QVariant(); // 骨架：无数据
}

QHash<int, QByteArray> ShareLinkModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[IdRole]           = QByteArrayLiteral("id");
    roles[FileIdRole]       = QByteArrayLiteral("fileId");
    roles[FileNameRole]     = QByteArrayLiteral("fileName");
    roles[IsDirRole]        = QByteArrayLiteral("isDir");
    roles[UrlRole]          = QByteArrayLiteral("url");
    roles[CodeRole]         = QByteArrayLiteral("code");
    roles[CreatedRole]      = QByteArrayLiteral("created");
    roles[ExpireRole]       = QByteArrayLiteral("expire");
    roles[DownloadsRole]    = QByteArrayLiteral("downloads");
    roles[MaxDownloadsRole] = QByteArrayLiteral("maxDownloads");
    roles[RevokedRole]      = QByteArrayLiteral("revoked");
    return roles;
}

int ShareLinkModel::count() const
{
    return 0;
}

} // namespace cv

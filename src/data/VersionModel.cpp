/****************************************************************************
 * data/VersionModel.cpp —— 历史版本数据模型实现
 ****************************************************************************/
#include "VersionModel.h"

#include <QDateTime>

#include "Util.h" // cv::Util::humanSize

namespace cv {

VersionModel::VersionModel(QObject *parent) : QAbstractListModel(parent) {}

int VersionModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_versions.size();
}

QVariant VersionModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_versions.size())
        return QVariant();

    const FileVersion &v = m_versions.at(index.row());
    switch (role) {
    case IdRole:      return v.id;
    case FileIdRole:  return v.fileId;
    case VersionRole: return v.version;
    case SizeRole:    return v.size;
    case CreatedRole: return v.created.toMSecsSinceEpoch();
    case DeviceRole:  return v.device;
    case NoteRole:    return v.note;
    case CurrentRole: return v.current;
    case SizeTextRole:
        return Util::humanSize(v.size);
    case TimeRole:
        return v.created.isValid()
                   ? v.created.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                   : QStringLiteral("-");
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> VersionModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[IdRole]       = QByteArrayLiteral("id");
    roles[FileIdRole]   = QByteArrayLiteral("fileId");
    roles[VersionRole]  = QByteArrayLiteral("version");
    roles[SizeRole]     = QByteArrayLiteral("size");
    roles[CreatedRole]  = QByteArrayLiteral("created");
    roles[DeviceRole]   = QByteArrayLiteral("device");
    roles[NoteRole]     = QByteArrayLiteral("note");
    roles[CurrentRole]  = QByteArrayLiteral("current");
    roles[SizeTextRole] = QByteArrayLiteral("sizeText");
    roles[TimeRole]     = QByteArrayLiteral("time");
    return roles;
}

int VersionModel::count() const
{
    return m_versions.size();
}

void VersionModel::setVersions(const QVector<FileVersion> &versions)
{
    beginResetModel();
    m_versions = versions;
    endResetModel();
    emit countChanged();
}

QVariantMap VersionModel::at(int row) const
{
    QVariantMap m;
    if (row < 0 || row >= m_versions.size())
        return m;
    const FileVersion &v = m_versions.at(row);
    m.insert(QStringLiteral("id"), v.id);
    m.insert(QStringLiteral("fileId"), v.fileId);
    m.insert(QStringLiteral("version"), v.version);
    m.insert(QStringLiteral("size"), v.size);
    m.insert(QStringLiteral("sizeText"), Util::humanSize(v.size));
    m.insert(QStringLiteral("time"),
             v.created.isValid()
                 ? v.created.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                 : QStringLiteral("-"));
    m.insert(QStringLiteral("device"), v.device);
    m.insert(QStringLiteral("note"), v.note);
    m.insert(QStringLiteral("current"), v.current);
    return m;
}

void VersionModel::clear()
{
    if (m_versions.isEmpty())
        return;
    beginResetModel();
    m_versions.clear();
    endResetModel();
    emit countChanged();
}

} // namespace cv

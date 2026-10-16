/****************************************************************************
 * data/TagModel.cpp —— 标签列表模型实现
 ****************************************************************************/
#include "TagModel.h"

namespace cv {

TagModel::TagModel(QObject *parent) : QAbstractListModel(parent) {}

int TagModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_tags.size();
}

QVariant TagModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tags.size())
        return QVariant();

    const Tag &t = m_tags.at(index.row());
    switch (role) {
    case IdRole:    return t.id;
    case NameRole:  return t.name;
    case ColorRole: return t.color;
    case CountRole: return t.count;
    default:        return QVariant();
    }
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
    return m_tags.size();
}

void TagModel::setTags(const QVector<Tag> &tags)
{
    beginResetModel();
    m_tags = tags;
    endResetModel();
    emit countChanged();
}

QVariantMap TagModel::at(int row) const
{
    QVariantMap m;
    if (row < 0 || row >= m_tags.size())
        return m;
    const Tag &t = m_tags.at(row);
    m.insert(QStringLiteral("id"), t.id);
    m.insert(QStringLiteral("name"), t.name);
    m.insert(QStringLiteral("color"), t.color);
    m.insert(QStringLiteral("count"), t.count);
    return m;
}

void TagModel::clear()
{
    if (m_tags.isEmpty())
        return;
    beginResetModel();
    m_tags.clear();
    endResetModel();
    emit countChanged();
}

} // namespace cv

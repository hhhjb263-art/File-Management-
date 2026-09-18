/****************************************************************************
 * data/FileListModel.cpp —— 文件列表数据模型实现
 ****************************************************************************/
#include "FileListModel.h"

#include <QDateTime>

#include <algorithm>

#include "Util.h" // cv::Util::humanSize（core 层纯格式化函数）

namespace cv {

// ============================ FileRow ============================

QString FileRow::sizeText() const
{
    // 复用 core/Util.h 的 humanSize；负数（异常值）由它统一返回 "-"。
    return Util::humanSize(size);
}

QString FileRow::timeText() const
{
    // 与 client/src/MainWindow.cpp::formatTime 完全一致的展示规则：
    //   0 / 缺失 / 非法 → "-"；服务端 created_at 可能是毫秒，>1e11 归一化为秒。
    qint64 epoch = createdAt;
    if (epoch <= 0) {
        return QStringLiteral("-");
    }
    if (epoch > 100000000000LL) {
        epoch /= 1000;
    }
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(epoch).toLocalTime();
    if (!dt.isValid()) {
        return QStringLiteral("-");
    }
    return dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

// ============================ FileListModel ============================

FileListModel::FileListModel(QObject *parent) : QAbstractListModel(parent) {}

int FileListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0; // 列表模型没有子层级
    }
    return int(m_items.size());
}

QVariant FileListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }
    const int row = index.row();
    if (row < 0 || row >= int(m_items.size())) {
        return QVariant();
    }
    const Item &item = m_items.at(row);
    switch (role) {
    case IdRole:            return item.row.id;
    case NameRole:          return item.row.name;
    case SizeRole:          return item.sizeText;        // 展示字符串 "1.23 MB"
    case SizeBytesRole:     return item.row.size;        // 原始字节
    case TimeRole:          return item.timeText;        // 展示字符串
    case MtimeRole:         return item.row.createdAt;   // 原始时间
    case StatusRole:        return item.row.status;
    case DirRole:           return item.row.dir;
    case HashRole:          return item.row.hash;
    case Qt::DisplayRole:   return item.row.name;        // 兜底：未指定角色时显示名称
    default:                return QVariant();
    }
}

QHash<int, QByteArray> FileListModel::roleNames() const
{
    // 键名严格对齐契约 §4：name size time status id dir（其余为附加角色）。
    QHash<int, QByteArray> roles;
    roles[IdRole]        = QByteArrayLiteral("id");
    roles[NameRole]      = QByteArrayLiteral("name");
    roles[SizeRole]      = QByteArrayLiteral("size");
    roles[SizeBytesRole] = QByteArrayLiteral("sizeBytes");
    roles[TimeRole]      = QByteArrayLiteral("time");
    roles[MtimeRole]     = QByteArrayLiteral("mtime");
    roles[StatusRole]    = QByteArrayLiteral("status");
    roles[DirRole]       = QByteArrayLiteral("dir");
    roles[HashRole]      = QByteArrayLiteral("hash");
    return roles;
}

void FileListModel::setRows(const QList<FileRow> &rows)
{
    beginResetModel();
    m_items.clear();
    m_items.reserve(rows.size());
    for (const FileRow &row : rows) {
        Item item;
        item.row      = row;
        item.sizeText = row.sizeText(); // 预计算展示串，避免 QML 反复格式化
        item.timeText = row.timeText();
        m_items.append(item);
    }
    sortItems(); // 整批替换后按当前排序键重排
    endResetModel();
    emit countChanged();
}

void FileListModel::sortBy(int key, bool asc)
{
    m_sortKey = key;
    m_sortAsc = asc;
    // 排序会改变所有行的位置，用 reset 通知最稳妥（行数不变，无需 countChanged）。
    beginResetModel();
    sortItems();
    endResetModel();
}

QVariantMap FileListModel::rowAt(int row) const
{
    QVariantMap map;
    if (row < 0 || row >= int(m_items.size())) {
        return map;
    }
    const Item &item = m_items.at(row);
    map.insert(QStringLiteral("id"), item.row.id);
    map.insert(QStringLiteral("name"), item.row.name);
    map.insert(QStringLiteral("dir"), item.row.dir);
    map.insert(QStringLiteral("size"), item.sizeText);
    map.insert(QStringLiteral("sizeBytes"), item.row.size);
    map.insert(QStringLiteral("time"), item.timeText);
    map.insert(QStringLiteral("mtime"), item.row.createdAt);
    map.insert(QStringLiteral("status"), item.row.status);
    map.insert(QStringLiteral("hash"), item.row.hash);
    return map;
}

void FileListModel::clear()
{
    if (m_items.isEmpty()) {
        return;
    }
    beginResetModel();
    m_items.clear();
    endResetModel();
    emit countChanged();
}

int FileListModel::count() const
{
    return int(m_items.size());
}

// ============================ 内部 ============================

void FileListModel::sortItems()
{
    const int  key = m_sortKey;
    const bool asc = m_sortAsc;
    // 与 client/src/MainWindow.cpp::applySort 同构：stable_sort + 同名按 dir 兜底。
    std::stable_sort(m_items.begin(), m_items.end(), [key, asc](const Item &a, const Item &b) {
        int cmp = 0;
        if (key == 1) {
            cmp = (a.row.size < b.row.size) ? -1 : (a.row.size > b.row.size ? 1 : 0);
        } else if (key == 2) {
            cmp = (a.row.createdAt < b.row.createdAt)
                      ? -1
                      : (a.row.createdAt > b.row.createdAt ? 1 : 0);
        } else {
            cmp = QString::compare(a.row.name, b.row.name, Qt::CaseInsensitive);
            if (cmp == 0) {
                cmp = QString::compare(a.row.dir, b.row.dir, Qt::CaseInsensitive);
            }
        }
        return asc ? (cmp < 0) : (cmp > 0);
    });
}

} // namespace cv

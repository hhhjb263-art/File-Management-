/****************************************************************************
 * controllers/TrashController.cpp —— 回收站控制器实现
 ****************************************************************************/
#include "TrashController.h"

#include "core/Async.h"
#include "core/Util.h" // cv::Util::humanSize
#include "data/FileListModel.h"
#include "net/Backend.h"
#include "net/HttpBackend.h"

namespace cv {

TrashController::TrashController(Backend *backend, FileListModel *model, QObject *parent)
    : QObject(parent), m_backend(backend), m_model(model)
{
    if (m_model)
        m_model->clear();
}

QString TrashController::usedText() const
{
    return Util::humanSize(m_usedBytes);
}

QString TrashController::emptyText() const
{
    // 口径提醒（勿简化成"仍然占空间"）：服务端 sumSizeOfOwner 带 `WHERE deleted = 0`，
    // 所以进站后「已用空间」当场扣除，但磁盘上的 blob 一个没少。
    // 文案只说"磁盘空间"这一件确定的事，不与设置页的用量数字打架。
    return QStringLiteral("回收站是空的。删除的文件会先来到这里，可以随时恢复到原位置；"
                          "彻底删除或清空后才会真正回收磁盘空间。");
}

bool TrashController::hasDirEntries() const
{
    for (const FileItem &f : m_items) {
        if (f.isDir)
            return true;
    }
    return false;
}

void TrashController::setBusy(bool on)
{
    if (m_busy == on)
        return;
    m_busy = on;
    emit busyChanged();
}

QString TrashController::friendlyError(const QString &raw)
{
    if (raw.isEmpty())
        return QStringLiteral("操作失败");
    if (HttpBackend::isUnsupported(raw))
        return raw; // 已含中文说明
    if (raw.contains(QStringLiteral("[network]")))
        return QStringLiteral("网络不可达，请检查服务端地址与连接");
    return raw;
}

void TrashController::applyItems(const QVector<FileItem> &items)
{
    m_items = items;
    m_usedBytes = 0;
    for (const FileItem &f : items)
        m_usedBytes += f.size;

    if (m_model) {
        QList<FileRow> rows;
        rows.reserve(items.size());
        for (const FileItem &f : items) {
            FileRow row;
            row.id   = f.id;
            row.name = f.name;
            // dir 列展示"原位置"：根目录显示为空串，QML 侧渲染成 "/"
            row.dir  = (f.parentId.isEmpty() || f.parentId == kRootId || f.parentId == kRootPath)
                           ? QString()
                           : f.parentId;
            row.size = f.size;
            row.hash = f.hash;
            row.status = f.isDir ? QStringLiteral("文件夹") : QStringLiteral("文件");
            // 复用 time 列显示"删除时间"：FileRow::timeText 会把毫秒归一化
            row.createdAt = f.trashedAt.isValid() ? f.trashedAt.toMSecsSinceEpoch() : 0;
            rows.append(row);
        }
        m_model->setRows(rows);
    }
    emit trashChanged();
}

void TrashController::refresh()
{
    if (!m_backend)
        return;
    setBusy(true);
    const int seq = ++m_seq;
    Async::run(
        this, [this] { return m_backend->listTrash(); },
        [this, seq](Result<QVector<FileItem>> r) {
            if (seq != m_seq)
                return; // 乱序回包：丢弃
            setBusy(false);
            if (!r.ok) {
                emit errorOccurred(friendlyError(r.error));
                emit refreshFinished(false);
                return;
            }
            applyItems(r.value);
            emit refreshFinished(true);
        });
}

void TrashController::restore(const QString &id)
{
    if (!m_backend || id.isEmpty())
        return;
    setBusy(true);
    Async::run(
        this, [this, id] { return m_backend->restore(id); },
        [this, id](Ok r) {
            setBusy(false);
            if (!r.ok) {
                emit errorOccurred(friendlyError(r.error));
                return;
            }
            emit statusMessage(QStringLiteral("已恢复到原位置"), true);
            emit trashMutated();
            refresh();
        });
}

void TrashController::purge(const QString &id)
{
    if (!m_backend || id.isEmpty())
        return;
    setBusy(true);
    Async::run(
        this, [this, id] { return m_backend->purge(id); },
        [this, id](Ok r) {
            setBusy(false);
            if (!r.ok) {
                emit errorOccurred(friendlyError(r.error));
                return;
            }
            emit statusMessage(QStringLiteral("已彻底删除，空间已释放"), true);
            emit trashMutated();
            refresh();
        });
}

void TrashController::emptyTrash()
{
    if (!m_backend)
        return;
    setBusy(true);
    Async::run(
        this, [this] { return m_backend->emptyTrash(); },
        [this](Ok r) {
            setBusy(false);
            if (!r.ok) {
                emit errorOccurred(friendlyError(r.error));
                return;
            }
            emit statusMessage(QStringLiteral("回收站已清空，空间已释放"), true);
            emit trashMutated();
            refresh();
        });
}

QVariantMap TrashController::at(int i) const
{
    QVariantMap m;
    if (i < 0 || i >= m_items.size())
        return m;
    const FileItem &f = m_items.at(i);
    m.insert(QStringLiteral("id"), f.id);
    m.insert(QStringLiteral("name"), f.name);
    m.insert(QStringLiteral("isDir"), f.isDir);
    m.insert(QStringLiteral("size"), f.size);
    m.insert(QStringLiteral("sizeText"), Util::humanSize(f.size));
    m.insert(QStringLiteral("parentId"), f.parentId);
    m.insert(QStringLiteral("deletedAt"),
             f.trashedAt.isValid() ? f.trashedAt.toMSecsSinceEpoch() : qint64(0));
    return m;
}

} // namespace cv

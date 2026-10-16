/****************************************************************************
 * controllers/TagController.cpp —— 标签控制器实现
 ****************************************************************************/
#include "TagController.h"

#include "core/Async.h"
#include "core/Util.h" // cv::Util::humanSize
#include "data/TagModel.h"
#include "net/Backend.h"
#include "net/HttpBackend.h"

namespace cv {

TagController::TagController(Backend *backend, TagModel *model, QObject *parent)
    : QObject(parent), m_backend(backend), m_model(model)
{
    if (m_model)
        m_model->clear();
}

QString TagController::emptyText() const
{
    return QStringLiteral("还没有标签。标签是给文件打的横向分类 —— 一个文件可以有多个，"
                          "比文件夹更灵活，且不改变文件原有的位置。");
}

void TagController::setBusy(bool on)
{
    if (m_busy == on)
        return;
    m_busy = on;
    emit busyChanged();
}

QString TagController::friendlyError(const QString &raw)
{
    if (raw.isEmpty())
        return QStringLiteral("操作失败");
    if (HttpBackend::isUnsupported(raw))
        return raw;
    if (raw.contains(QStringLiteral("[network]")))
        return QStringLiteral("网络不可达，请检查服务端地址与连接");
    return raw;
}

void TagController::refresh()
{
    if (!m_backend)
        return;
    setBusy(true);
    const int seq = ++m_seq;
    Async::run(
        this, [this] { return m_backend->tags(); },
        [this, seq](Result<QVector<Tag>> r) {
            if (seq != m_seq)
                return; // 乱序丢弃
            setBusy(false);
            if (!r.ok) {
                emit errorOccurred(friendlyError(r.error));
                return;
            }
            m_tags = r.value;
            if (m_model)
                m_model->setTags(m_tags);
            emit tagsChanged();
        });
}

void TagController::create(const QString &name, const QString &color)
{
    const QString n = name.trimmed();
    if (n.isEmpty()) {
        emit errorOccurred(QStringLiteral("标签名不能为空"));
        return;
    }
    if (!m_backend)
        return;

    setBusy(true);
    Async::run(
        this, [this, n, color] { return m_backend->createTag(n, color); },
        [this, n](Result<Tag> r) {
            setBusy(false);
            if (!r.ok) {
                emit errorOccurred(friendlyError(r.error));
                return;
            }
            emit statusMessage(QStringLiteral("标签「%1」已就绪").arg(r.value.name), true);
            emit tagCreated(r.value.name);
            refresh(); // 重新拉一次：拿到准确的关联计数与服务端归一化后的颜色
        });
}

void TagController::remove(const QString &tagId)
{
    if (!m_backend || tagId.isEmpty())
        return;
    setBusy(true);
    Async::run(
        this, [this, tagId] { return m_backend->deleteTag(tagId); },
        [this, tagId](Ok r) {
            setBusy(false);
            if (!r.ok) {
                emit errorOccurred(friendlyError(r.error));
                return;
            }
            // 正在按这个标签筛选的话，筛选条件已经失效了，顺手清掉
            if (m_filterTagId == tagId) {
                m_filterTagId.clear();
                m_matches.clear();
                emit filterChanged();
                emit matchesChanged();
            }
            emit statusMessage(QStringLiteral("标签已删除"), true);
            refresh();
        });
}

void TagController::assign(const QString &fileId, const QStringList &tagIds)
{
    if (!m_backend || fileId.isEmpty())
        return;
    setBusy(true);
    Async::run(
        this, [this, fileId, tagIds] { return m_backend->setTags(fileId, tagIds); },
        [this, fileId](Ok r) {
            setBusy(false);
            if (!r.ok) {
                emit errorOccurred(friendlyError(r.error));
                return;
            }
            emit statusMessage(QStringLiteral("标签已更新"), true);
            refresh(); // 关联数变了，主列表的计数要跟着动
            // 文件列表也要跟着刷：打了标签之后那一行的标签列/筛选结果都可能变。
            // 以前只 refresh() 了标签本身，所以用户得手动刷新才看得到变化。
            emit fileTagsAssigned(fileId);
        });
}

void TagController::loadFileTags(const QString &fileId)
{
    if (!m_backend || fileId.isEmpty()) {
        emit fileTagsLoaded(fileId, QStringList());
        return;
    }
    // 不用 m_seq：结果本身带 fileId，调用方按 fileId 自行判定是否还是当前目标
    Async::run(
        this, [this, fileId] { return m_backend->tagsOfFile(fileId); },
        [this, fileId](Result<QVector<Tag>> r) {
            QStringList ids;
            if (r.ok) {
                for (const Tag &t : r.value)
                    ids << t.id;
            }
            emit fileTagsLoaded(fileId, ids);
        });
}

void TagController::applyFilter(const QString &tagId)
{
    if (m_filterTagId == tagId)
        return;
    m_filterTagId = tagId;
    emit filterChanged();

    if (tagId.isEmpty()) {
        m_matches.clear();
        emit matchesChanged();
        return;
    }
    if (!m_backend)
        return;

    setBusy(true);
    const int seq = ++m_seq;
    Async::run(
        this,
        [this, tagId] { return m_backend->search(QString(), QStringList{tagId}, -1); },
        [this, seq](Result<QVector<FileItem>> r) {
            if (seq != m_seq)
                return;
            setBusy(false);
            if (!r.ok) {
                emit errorOccurred(friendlyError(r.error));
                return;
            }
            m_matches = r.value;
            emit matchesChanged();
        });
}

void TagController::search(const QString &keyword)
{
    if (!m_backend)
        return;
    setBusy(true);
    const int seq = ++m_seq;
    const QString kw = keyword.trimmed();
    Async::run(
        this,
        [this, kw] {
            return m_backend->search(kw, QStringList{},
                                     -1); // type=-1 全部类型
        },
        [this, seq](Result<QVector<FileItem>> r) {
            if (seq != m_seq)
                return;
            setBusy(false);
            if (!r.ok) {
                emit errorOccurred(friendlyError(r.error));
                return;
            }
            m_matches = r.value;
            emit matchesChanged();
        });
}

QVariantList TagController::matchedFiles() const
{
    QVariantList out;
    out.reserve(m_matches.size());
    for (const FileItem &f : m_matches) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), f.id);
        m.insert(QStringLiteral("name"), f.name);
        m.insert(QStringLiteral("path"), f.path);
        m.insert(QStringLiteral("size"), f.size);
        m.insert(QStringLiteral("sizeText"), Util::humanSize(f.size));
        m.insert(QStringLiteral("isDir"), f.isDir);
        out.append(m);
    }
    return out;
}

} // namespace cv

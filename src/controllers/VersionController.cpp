/****************************************************************************
 * controllers/VersionController.cpp —— 文件历史版本控制器实现
 ****************************************************************************/
#include "VersionController.h"

#include "core/Async.h"
#include "data/VersionModel.h"
#include "net/Backend.h"
#include "net/HttpBackend.h"

namespace cv {

VersionController::VersionController(Backend *backend, VersionModel *model, QObject *parent)
    : QObject(parent), m_backend(backend), m_model(model)
{
    if (m_model)
        m_model->clear();
}

QString VersionController::emptyText() const
{
    if (m_fileId.isEmpty())
        return QStringLiteral("先在「文件」页选中一个文件，再回到这里查看它的历史版本。");
    return QStringLiteral("这个文件还没有历史版本。每次覆盖上传新内容时，"
                          "旧内容都会自动存成一条版本，随时可以回滚。");
}

void VersionController::setBusy(bool on)
{
    if (m_busy == on)
        return;
    m_busy = on;
    emit busyChanged();
}

QString VersionController::friendlyError(const QString &raw)
{
    if (raw.isEmpty())
        return QStringLiteral("操作失败");
    if (HttpBackend::isUnsupported(raw))
        return raw;
    if (raw.contains(QStringLiteral("[network]")))
        return QStringLiteral("网络不可达，请检查服务端地址与连接");
    return raw;
}

void VersionController::applyVersions(const QVector<FileVersion> &versions)
{
    m_versions = versions;
    m_currentVersion = 1;
    for (const FileVersion &v : versions) {
        if (v.current) {
            m_currentVersion = v.version;
            break;
        }
    }
    if (m_model)
        m_model->setVersions(m_versions);
    emit versionsChanged();
}

void VersionController::open(const QString &fileId, const QString &fileName)
{
    m_fileId   = fileId;
    m_fileName = fileName;
    emit targetChanged();
    refresh();
}

void VersionController::close()
{
    m_fileId.clear();
    m_fileName.clear();
    m_versions.clear();
    m_currentVersion = 1;
    if (m_model)
        m_model->clear();
    emit targetChanged();
    emit versionsChanged();
}

void VersionController::refresh()
{
    if (!m_backend || m_fileId.isEmpty())
        return;
    setBusy(true);
    const int seq = ++m_seq;
    const QString fid = m_fileId;
    Async::run(
        this, [this, fid] { return m_backend->versions(fid); },
        [this, seq, fid](Result<QVector<FileVersion>> r) {
            if (seq != m_seq || fid != m_fileId)
                return; // 乱序 / 已切换目标：丢弃
            setBusy(false);
            if (!r.ok) {
                emit errorOccurred(friendlyError(r.error));
                return;
            }
            applyVersions(r.value);
        });
}

void VersionController::snapshot()
{
    if (!m_backend || m_fileId.isEmpty())
        return;
    setBusy(true);
    const QString fid = m_fileId;
    Async::run(
        this,
        [this, fid] { return m_backend->snapshotVersion(fid, QStringLiteral("手动快照")); },
        [this, fid](Ok r) {
            if (fid != m_fileId)
                return;
            setBusy(false);
            if (!r.ok) {
                emit errorOccurred(friendlyError(r.error));
                return;
            }
            emit statusMessage(QStringLiteral("已把当前内容存为一个历史版本"), true);
            refresh();
        });
}

void VersionController::restore(const QString &versionId)
{
    if (!m_backend || m_fileId.isEmpty() || versionId.isEmpty())
        return;
    setBusy(true);
    const QString fid = m_fileId;
    Async::run(
        this, [this, fid, versionId] { return m_backend->restoreVersion(fid, versionId); },
        [this, fid](Ok r) {
            if (fid != m_fileId)
                return;
            setBusy(false);
            if (!r.ok) {
                emit errorOccurred(friendlyError(r.error));
                return;
            }
            emit statusMessage(QStringLiteral("已回滚到所选版本（回滚前的内容已自动备份）"), true);
            emit versionRestored(fid);
            refresh();
        });
}

void VersionController::removeVersion(const QString &versionId)
{
    if (!m_backend || m_fileId.isEmpty() || versionId.isEmpty())
        return;
    setBusy(true);
    const QString fid = m_fileId;
    Async::run(
        this, [this, fid, versionId] { return m_backend->deleteVersion(fid, versionId); },
        [this, fid](Ok r) {
            if (fid != m_fileId)
                return;
            setBusy(false);
            if (!r.ok) {
                emit errorOccurred(friendlyError(r.error));
                return;
            }
            emit statusMessage(QStringLiteral("历史版本已删除"), true);
            refresh();
        });
}

QVariantMap VersionController::at(int i) const
{
    QVariantMap m;
    if (i < 0 || i >= m_versions.size())
        return m;
    const FileVersion &v = m_versions.at(i);
    m.insert(QStringLiteral("id"), v.id);
    m.insert(QStringLiteral("version"), v.version);
    m.insert(QStringLiteral("size"), v.size);
    m.insert(QStringLiteral("device"), v.device);
    m.insert(QStringLiteral("note"), v.note);
    m.insert(QStringLiteral("current"), v.current);
    return m;
}

} // namespace cv

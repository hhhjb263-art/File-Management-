/****************************************************************************
 * sync/FileWatcher.cpp —— 递归监听 + 防抖实现
 ****************************************************************************/
#include "FileWatcher.h"

#include "core/Logging.h"
#include "sync/SyncRules.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>

namespace cv {

FileWatcher::FileWatcher(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
    , m_debounce(new QTimer(this))
{
    m_debounce->setSingleShot(true);
    connect(m_debounce, &QTimer::timeout, this, [this]() {
        if (m_pending) {
            m_pending = false;
            emit pendingChanged(false);
        }
        emit settled();
    });

    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this,
            &FileWatcher::onDirectoryChanged);
}

void FileWatcher::setDebounceMs(int ms)
{
    m_debounceMs = qMax(0, ms);
}

int FileWatcher::debounceMs() const
{
    return m_debounceMs;
}

bool FileWatcher::isPending() const
{
    return m_pending;
}

QStringList FileWatcher::ignorePatternsFor(const QString &rootAbs) const
{
    // 每个同步根有自己的 .cvignore，故按根现取
    return SyncRules::effectivePatterns(rootAbs);
}

void FileWatcher::watch(const QString &rootAbs)
{
    if (rootAbs.isEmpty())
        return;
    const QString abs = QDir(rootAbs).absolutePath();
    if (m_roots.contains(abs))
        return;

    QDir d(abs);
    if (!d.exists()) {
        qWarning() << "同步监听：目录不存在，跳过" << abs;
        return;
    }
    m_roots << abs;
    resyncRegistration();
}

void FileWatcher::unwatchAll()
{
    if (!m_registeredDirs.isEmpty())
        m_watcher->removePaths(m_registeredDirs);
    m_registeredDirs.clear();
    m_roots.clear();
    m_debounce->stop();
    if (m_pending) {
        m_pending = false;
        emit pendingChanged(false);
    }
}

void FileWatcher::registerTree(const QString &absDir, QStringList &alive)
{
    const QStringList patterns = ignorePatternsFor(absDir);

    QDirIterator it(absDir, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    // 根目录自己也要登记
    QDirIterator rootIt(absDir, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);

    QStringList dirs;
    dirs << QDir(absDir).absolutePath();
    while (it.hasNext()) {
        const QString p = it.next();
        // 忽略目录不登记（.git/ 之类会把事件刷爆，且我们本来也不同步它）
        const QString rel = QDir(absDir).relativeFilePath(p);
        const QString name = QFileInfo(p).fileName();
        if (SyncRules::isIgnoredPath(patterns, rel, true))
            continue;
        // 祖先里若有被忽略的，也一并跳过（isIgnoredPath 只看自己那一段）
        bool ancestorIgnored = false;
        QString acc;
        const QStringList segs = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (int i = 0; i + 1 < segs.size(); ++i) {
            acc = acc.isEmpty() ? segs[i] : (acc + QLatin1Char('/') + segs[i]);
            if (SyncRules::isIgnoredPath(patterns, acc, true)) {
                ancestorIgnored = true;
                break;
            }
        }
        if (ancestorIgnored)
            continue;
        dirs << p;
        (void)name;
    }
    (void)rootIt;

    QStringList fresh;
    for (const QString &p : dirs) {
        alive << p;
        if (!m_registeredDirs.contains(p))
            fresh << p;
    }
    // 新目录补登记
    for (const QString &p : fresh) {
        if (m_watcher->addPath(p))
            m_registeredDirs << p;
    }
}

void FileWatcher::resyncRegistration()
{
    QStringList alive;
    for (const QString &root : m_roots)
        registerTree(root, alive);

    // 摘掉已经不存在（或被忽略）的目录，避免句柄泄漏
    QStringList drop;
    for (const QString &p : m_registeredDirs) {
        if (!alive.contains(p))
            drop << p;
    }
    if (!drop.isEmpty()) {
        m_watcher->removePaths(drop);
        for (const QString &p : drop)
            m_registeredDirs.removeAll(p);
    }
}

void FileWatcher::onDirectoryChanged(const QString &path)
{
    Q_UNUSED(path);
    // 目录结构可能变了（新建/删除子目录）→ 先把登记表同步一次，
    // 否则新目录里的后续改动不会被监听到（这是"漏事件"的经典成因）。
    resyncRegistration();

    if (!m_pending) {
        m_pending = true;
        emit pendingChanged(true);
    }
    // 每次变更都重置 → 连续写入只会合并成一次 settled()
    m_debounce->start(m_debounceMs);
}

void FileWatcher::flush()
{
    if (m_debounce->isActive())
        m_debounce->stop();
    if (m_pending) {
        m_pending = false;
        emit pendingChanged(false);
    }
    emit settled();
}

} // namespace cv

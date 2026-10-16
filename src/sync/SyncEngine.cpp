/****************************************************************************
 * sync/SyncEngine.cpp —— 同步引擎实现
 *
 * 流程：扫本地(pool) → 拉云端(主线程) → SyncPlan 决策 → 逐动作执行(主线程，动作间让出事件循环)
 *       每个动作成功后按**实际观察到的**结果推进 LocalIndex（见 SyncPlan.h 的说明）。
 ****************************************************************************/
#include "SyncEngine.h"

#include "core/AppPaths.h"
#include "core/Crypto.h"
#include "core/Logging.h"
#include "net/Backend.h"
#include "sync/FileWatcher.h"
#include "sync/LocalIndex.h"
#include "sync/SyncRules.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThreadPool>
#include <QTimer>

namespace cv {
namespace {

QString dirNameOf(const QString &relPath)
{
    const int slash = relPath.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : relPath.left(slash);
}

QString baseNameOf(const QString &relPath)
{
    const int slash = relPath.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? relPath : relPath.mid(slash + 1);
}

// 云端根路径规范化：统一成"无尾斜杠、有前导斜杠"（"" 或 "/" → 根）
QString normRemoteRoot(const QString &p)
{
    QString s = p.trimmed();
    while (s.endsWith(QLatin1Char('/')))
        s.chop(1);
    if (!s.isEmpty() && !s.startsWith(QLatin1Char('/')))
        s.prepend(QLatin1Char('/'));
    return s;
}

// relPath 在同步根下的绝对路径
QString absOf(const QString &rootAbs, const QString &relPath)
{
    return relPath.isEmpty() ? rootAbs : (rootAbs + QLatin1Char('/') + relPath);
}

QString hashFile(const QString &absPath, QString &err)
{
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) {
        err = QStringLiteral("无法读取 %1：%2").arg(absPath, f.errorString());
        return QString();
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) {
        err = QStringLiteral("读取内容失败：%1").arg(absPath);
        return QString();
    }
    return QString::fromLatin1(h.result().toHex());
}

// 递归扫描（**剪枝**：忽略目录整棵不下探）—— 用 QDir 手写而不是 QDirIterator，
// 因为后者无法在遍历中跳过整个子树，会把 .git/ 里成千上万个文件全哈希一遍。
void walkLocal(const QString &rootAbs, const QString &relDir, const QStringList &patterns,
               const QHash<QString, LocalIndex::Entry> &prev, SyncSnapshot &out,
               QStringList &errors)
{
    QDir d(absOf(rootAbs, relDir));
    const QFileInfoList entries =
        d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden |
                            QDir::System);

    for (const QFileInfo &fi : entries) {
        const QString name = fi.fileName();
        const QString rel  = relDir.isEmpty() ? name : (relDir + QLatin1Char('/') + name);

        // ⚠️ 跳过符号链接：目录符号链接会导致无限递归（A→B→A）
        if (fi.isSymLink())
            continue;

        const bool isDir = fi.isDir();
        if (SyncRules::isIgnoredPath(patterns, rel, isDir))
            continue;

        SyncSnapshot::Item it;
        it.isDir = isDir;
        if (isDir) {
            out.items.insert(rel, it);
            walkLocal(rootAbs, rel, patterns, prev, out, errors);
            continue;
        }

        it.size  = fi.size();
        it.mtime = fi.lastModified().toUTC();

        // 未变则复用上次的哈希：对只读大文件能省下整文件读取（同步的常见热路径）
        auto p = prev.constFind(rel);
        if (p != prev.constEnd() && !p->hash.isEmpty() && p->size == it.size &&
            p->mtime == it.mtime) {
            it.hash = p->hash;
        } else {
            QString e;
            it.hash = hashFile(fi.absoluteFilePath(), e);
            if (it.hash.isEmpty())
                errors << (rel + QStringLiteral("（") + e + QStringLiteral("）"));
        }
        out.items.insert(rel, it);
    }
}

// 把目录整棵搬走（本地删除的"保险箱"用）。同卷 rename 最快；跨卷退回复制后删源。
bool moveTree(const QString &src, const QString &dst, QString &err)
{
    if (QFile::rename(src, dst))
        return true;

    QFileInfo si(src);
    if (si.isDir()) {
        if (!QDir().mkpath(dst)) {
            err = QStringLiteral("无法创建目录 %1").arg(dst);
            return false;
        }
        QDir srcDir(src);
        const QFileInfoList kids =
            srcDir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
        for (const QFileInfo &k : kids) {
            const QString to = dst + QLatin1Char('/') + k.fileName();
            if (k.isDir()) {
                if (!moveTree(k.absoluteFilePath(), to, err))
                    return false;
            } else if (!QFile::copy(k.absoluteFilePath(), to)) {
                err = QStringLiteral("复制失败：%1").arg(k.absoluteFilePath());
                return false;
            } else {
                QFile::remove(k.absoluteFilePath());
            }
        }
        if (!QDir(src).removeRecursively()) {
            err = QStringLiteral("清理源目录失败：%1").arg(src);
            return false;
        }
        return true;
    }

    if (!QFile::copy(src, dst)) {
        err = QStringLiteral("复制失败：%1").arg(src);
        return false;
    }
    if (!QFile::remove(src)) {
        err = QStringLiteral("删除源文件失败：%1").arg(src);
        return false;
    }
    return true;
}

// 把后端返回的云端路径换算成「相对同步根」的路径；不属于本根则返回 false。
//
// 🔴 这里必须**双向归一化**，不能直接比前缀（实测踩过，后果很严重）：
//    两种 Backend 的前导斜杠约定不同 —— HttpBackend 返回 "/照片/2026/a.jpg"，
//    MockBackend（本地引擎）返回 "synctest/a.txt"（无前导斜杠）。
//    若按 pair.remotePath（"/synctest"）去 startsWith，远端条目会被**整片判成"不属于本同步对"**，
//    于是远端快照为空、下一轮对账把每个本地文件都当成"云端已删除"而删掉本地文件。
bool relativizeRemote(const QString &itemPath, const QString &remoteRoot, QString &outRel)
{
    const auto norm = [](const QString &s) {
        QString t = s.trimmed();
        while (t.startsWith(QLatin1Char('/')))
            t.remove(0, 1);
        while (t.endsWith(QLatin1Char('/')))
            t.chop(1);
        return t;
    };

    const QString p    = norm(itemPath);
    const QString root = norm(remoteRoot);
    if (p.isEmpty())
        return false;
    if (root.isEmpty()) {           // 同步根就是云端根：整条路径都是相对路径
        outRel = p;
        return true;
    }
    if (p == root)                  // 根本身不入表
        return false;
    if (!p.startsWith(root + QLatin1Char('/')))
        return false;
    outRel = p.mid(root.size() + 1);
    return !outRel.isEmpty();
}

} // namespace

// ===========================================================================

SyncEngine::SyncEngine(Backend *backend, LocalIndex *index, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_index(index)
    , m_watcher(new FileWatcher(this))
    , m_fallback(new QTimer(this))
{
    m_fallback->setSingleShot(false);
    connect(m_fallback, &QTimer::timeout, this, [this]() {
        if (m_running)
            reconcileNow();
    });
    // 事件驱动：防抖满了就对一次账（本次实现对**所有**启用对重对一次；
    // 个人网盘同步对通常 1~3 个，重新对账成本极低，换来的是"绝不会因为漏判
    // 某个根而漏同步"——宁可多算一次，也不漏）
    connect(m_watcher, &FileWatcher::settled, this, [this]() {
        if (m_running)
            reconcileNow();
    });
}

SyncEngine::~SyncEngine() = default;

QString SyncEngine::localTrashRoot()
{
    return AppPaths::rootDir() + QStringLiteral("/sync-trash");
}

QString SyncEngine::actionText(SyncAction::Kind kind)
{
    switch (kind) {
    case SyncAction::UploadNew:          return QStringLiteral("上传新增");
    case SyncAction::UploadUpdate:       return QStringLiteral("上传修改");
    case SyncAction::DownloadNew:        return QStringLiteral("下载新增");
    case SyncAction::DownloadUpdate:     return QStringLiteral("下载修改");
    case SyncAction::DeleteRemote:       return QStringLiteral("删除云端");
    case SyncAction::DeleteLocal:        return QStringLiteral("删除本地");
    case SyncAction::ConflictKeepLocal:  return QStringLiteral("冲突·保留本地");
    case SyncAction::ConflictKeepRemote: return QStringLiteral("冲突·保留云端");
    case SyncAction::MakeLocalDir:       return QStringLiteral("建本地目录");
    case SyncAction::MakeRemoteDir:      return QStringLiteral("建云端目录");
    case SyncAction::Skip:               return QStringLiteral("跳过");
    }
    return QStringLiteral("?");
}

QString SyncEngine::countsText(const SyncCounts &c)
{
    return QStringLiteral("上传 %1 · 下载 %2 · 删云 %3 · 删本地 %4 · 冲突 %5 · 目录 %6 · 跳过 %7")
        .arg(c.up).arg(c.down).arg(c.delRemote).arg(c.delLocal)
        .arg(c.conflicts).arg(c.dirs).arg(c.skipped);
}

void SyncEngine::setPairs(const QVector<SyncPair> &pairs)
{
    m_pairs = pairs;
}

void SyncEngine::setDeviceName(const QString &name)
{
    m_deviceName = name;
}

void SyncEngine::setScanIntervalMin(int minutes)
{
    m_scanIntervalMin = qMax(1, minutes);
    if (m_fallback->isActive())
        m_fallback->start(m_scanIntervalMin * 60 * 1000);
}

void SyncEngine::setDebounceMs(int ms)
{
    m_watcher->setDebounceMs(ms);
}

SyncPair *SyncEngine::pairById(const QString &pairId)
{
    for (SyncPair &p : m_pairs) {
        if (p.id == pairId)
            return &p;
    }
    return nullptr;
}

void SyncEngine::start()
{
    if (m_running)
        return;
    m_running = true;

    m_watcher->unwatchAll();
    for (const SyncPair &p : m_pairs) {
        if (p.enabled && !p.localPath.isEmpty())
            m_watcher->watch(p.localPath);
    }

    // 兜底对账（FR-3-1 的"每 10 分钟全量对账"）
    m_fallback->start(m_scanIntervalMin * 60 * 1000);

    // 启动即做一次全量对账
    reconcileNow();
}

void SyncEngine::stop()
{
    m_running = false;
    m_fallback->stop();
    m_watcher->unwatchAll();
    m_queue.clear();
    m_busy = false;
    m_run = Run();
}

void SyncEngine::reconcileNow(const QString &pairId)
{
    if (pairId.isEmpty()) {
        for (const SyncPair &p : m_pairs) {
            if (p.enabled && !p.localPath.isEmpty())
                enqueue(p.id, QStringLiteral("定时/事件"));
        }
    } else {
        enqueue(pairId, QStringLiteral("手动"));
    }
}

void SyncEngine::enqueue(const QString &pairId, const QString &reason)
{
    if (!m_busy) {
        m_queue.enqueue(pairId);
        startNext();
        return;
    }
    if (!m_queue.contains(pairId))
        m_queue.enqueue(pairId);
    (void)reason;
}

void SyncEngine::startNext()
{
    if (m_queue.isEmpty()) {
        m_busy = false;
        emit allFinished();
        return;
    }
    const QString pairId = m_queue.dequeue();
    SyncPair *p = pairById(pairId);
    if (!p || !p->enabled || p->localPath.isEmpty()) {
        startNext();
        return;
    }

    m_busy = true;
    m_run = Run();
    m_run.pairId = pairId;
    m_run.pair   = *p;

    emit pairStatus(pairId, int(SyncStatus::Syncing), QStringLiteral("正在对账…"));
    scanLocalPhase();
}

void SyncEngine::scanLocalPhase()
{
    const SyncPair pair    = m_run.pair;
    const QString pairId   = m_run.pairId;
    const QStringList pats = SyncRules::effectivePatterns(pair.localPath);

    // 上次索引里的状态：既作为"三方比较"的基准，也用来复用未变文件的哈希
    QHash<QString, LocalIndex::Entry> prev;
    if (m_index)
        prev = m_index->entries(pairId);

    const QString rootAbs = QDir(pair.localPath).absolutePath();
    QPointer<SyncEngine> self(this);

    // 扫描 + 哈希放线程池：纯文件 IO，不碰网络，慢也不该卡界面
    QThreadPool::globalInstance()->start([self, rootAbs, pats, prev, pairId]() {
        SyncSnapshot snap;
        QStringList  errors;
        if (QDir(rootAbs).exists())
            walkLocal(rootAbs, QString(), pats, prev, snap, errors);

        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, snap, errors, pairId]() {
                // ⚠️ 这个回调在成员函数里创建，必须显式捕获 this 才能访问 m_run / 调成员函数
                //    （外层是线程池 lambda，不捕获 this；内层是独立 lambda，捕获列表要重写）
                if (!self)
                    return;
                SyncEngine *eng = self.data();
                if (eng->m_run.pairId != pairId)
                    return;
                eng->m_run.local = snap;
                for (const QString &e : errors)
                    eng->m_run.log << QStringLiteral("[扫描告警] ") + e;
                if (!errors.isEmpty()) {
                    // 扫描期读不到的文件不阻断整次对账（很可能是别的进程正在写）
                    qWarning() << "同步扫描有告警" << errors.size() << "条";
                }
                eng->fetchRemotePhase();
            },
            Qt::QueuedConnection);
    });
}

void SyncEngine::fetchRemotePhase()
{
    const SyncPair pair = m_run.pair;

    // 云端树一次取回（listUnderPath 含子目录）
    //
    // 🔴 两种 Backend 的路径约定不同，必须**两种写法都试**：
    //    · HttpBackend  云端路径带前导斜杠："/照片/2026/a.jpg"
    //    · MockBackend  不带：                     "synctest/a.txt"
    //    而 MockBackend::listUnderPath 是拿传入路径做 **startsWith 前缀匹配**的，
    //    传 "/synctest" 会一条都匹配不到（实测：远端快照为空 → 下一轮对账把每个本地文件
    //    都当成"云端已删除"从而**删掉本地文件**）。这里按 id 去重合并，谁也不依赖对方改约定。
    QVector<FileItem> remoteItems;
    {
        const auto collect = [&remoteItems](const Result<QVector<FileItem>> &rr) {
            if (!rr.ok)
                return;
            for (const FileItem &f : rr.value) {
                bool dup = false;
                for (const FileItem &x : remoteItems) {
                    if (x.id == f.id) { dup = true; break; }
                }
                if (!dup)
                    remoteItems.append(f);
            }
        };
        const QString slashRoot = normRemoteRoot(pair.remotePath);   // "/synctest"
        QString noSlash = slashRoot;
        while (noSlash.startsWith(QLatin1Char('/')))
            noSlash.remove(0, 1);                                     // "synctest"

        Result<QVector<FileItem>> r1 = m_backend->listUnderPath(slashRoot);
        if (!r1.ok && remoteItems.isEmpty()) {
            // 第一种就失败：报错更可信，直接返回，避免把"网络错误"误判成"云端为空"
            finishCurrent(QStringLiteral("读取云端目录失败：") + r1.error);
            return;
        }
        collect(r1);
        if (noSlash != slashRoot) {
            Result<QVector<FileItem>> r2 = m_backend->listUnderPath(noSlash);
            collect(r2);
        }
    }

    const QString root = normRemoteRoot(pair.remotePath);
    SyncSnapshot snap;
    for (const FileItem &f : remoteItems) {
        QString p;
        if (!relativizeRemote(f.path, root, p))
            continue;   // 根自身 / 不属于本同步对 / 路径异常

        SyncSnapshot::Item it;
        it.isDir = f.isDir;
        it.size  = f.size;
        it.mtime = f.modified;
        it.hash  = f.hash;
        it.rev   = f.rev;
        it.id    = f.id;
        snap.items.insert(p, it);
    }
    m_run.remote = snap;

    // 上次同步成功时两端一致的状态
    SyncSnapshot base;
    if (m_index) {
        const QHash<QString, LocalIndex::Entry> es = m_index->entries(m_run.pairId);
        for (auto it = es.constBegin(); it != es.constEnd(); ++it) {
            SyncSnapshot::Item b;
            b.isDir = it->isDir;
            b.size  = it->size;
            b.mtime = it->mtime;
            b.hash  = it->hash;
            b.rev   = it->rev;
            base.items.insert(it.key(), b);
        }
    }
    m_run.lastSynced = base;

    planPhase();
}

void SyncEngine::planPhase()
{
    SyncPlanInput in;
    in.mode              = m_run.pair.mode;
    in.deleteSyncEnabled = true;      // FR-3-6 的"仅解除同步"开关（当前默认开启传播删除）
    in.deviceName        = m_deviceName;
    in.now               = QDateTime::currentDateTimeUtc();
    in.lastSynced        = m_run.lastSynced;
    in.local            = m_run.local;
    in.remote           = m_run.remote;
    in.excludeDirs      = m_run.pair.excludeDirs;   // FR-3-7 选择性同步

    m_run.plan        = SyncPlan::build(in);
    m_run.actionIndex = 0;

    m_run.log << QStringLiteral("对账完成：%1（本地 %2 项 / 云端 %3 项）")
                     .arg(countsText(m_run.plan.counts))
                     .arg(m_run.local.items.size())
                     .arg(m_run.remote.items.size());

    emit pairProgress(m_run.pairId, QStringLiteral("开始执行"), 0, m_run.plan.actions.size());
    executePhase();
}

void SyncEngine::executePhase()
{
    if (m_run.actionIndex >= m_run.plan.actions.size()) {
        finishCurrent(QString());
        return;
    }

    const SyncAction a = m_run.plan.actions[m_run.actionIndex];
    QString err;
    const bool ok = [&]() -> bool {
        // ---------------- 目录 ----------------
        if (a.kind == SyncAction::MakeLocalDir) {
            const QString abs = absOf(QDir(m_run.pair.localPath).absolutePath(), a.relPath);
            if (!QDir().mkpath(abs)) {
                err = QStringLiteral("创建本地目录失败：") + a.relPath;
                return false;
            }
            if (m_index) {
                LocalIndex::Entry e;
                e.pairId = m_run.pairId; e.relPath = a.relPath; e.isDir = true;
                m_index->put(e);
            }
            return true;
        }
        if (a.kind == SyncAction::MakeRemoteDir) {
            const QString dir = normRemoteRoot(m_run.pair.remotePath) + QLatin1Char('/') + a.relPath;
            Result<FileItem> r = m_backend->ensureFolderPath(dir);
            if (!r.ok) { err = QStringLiteral("创建云端目录失败：") + r.error; return false; }
            if (m_index) {
                LocalIndex::Entry e;
                e.pairId = m_run.pairId; e.relPath = a.relPath; e.isDir = true;
                m_index->put(e);
            }
            return true;
        }

        // ---------------- 删除 -----------------
        if (a.kind == SyncAction::DeleteRemote) {
            const SyncSnapshot::Item *rem = m_run.remote.find(a.relPath);
            if (!rem) { err = QStringLiteral("云端条目已不存在"); return false; }
            // 进云端回收站（可恢复），不是物理删除
            Ok o = m_backend->moveToTrash(rem->id);
            if (!o.ok) { err = QStringLiteral("删除云端失败：") + o.error; return false; }
            if (m_index) m_index->remove(m_run.pairId, a.relPath);
            return true;
        }
        if (a.kind == SyncAction::DeleteLocal) {
            const QString rootAbs = QDir(m_run.pair.localPath).absolutePath();
            const QString src     = absOf(rootAbs, a.relPath);
            if (!QFileInfo::exists(src)) {   // 已经不在了 → 视为完成
                if (m_index) m_index->remove(m_run.pairId, a.relPath);
                return true;
            }
            // ⚠️ 本地删除**不直接抹掉**：先搬进 AppData 下的同步保险箱。
            //    搬移失败就放弃删除（报错），绝不在没能保住副本的情况下硬删。
            const QString dstRoot = localTrashRoot() + QLatin1Char('/') + m_run.pairId +
                                    QLatin1Char('/') +
                                    QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
            const QString dst = absOf(dstRoot, a.relPath);
            QDir().mkpath(dirNameOf(dst));
            if (!moveTree(src, dst, err)) {
                err = QStringLiteral("本地删除已放弃（副本未能保住）：") + err;
                return false;
            }
            if (m_index) m_index->remove(m_run.pairId, a.relPath);
            return true;
        }

        // ---------------- 上传 ----------------
        if (a.kind == SyncAction::UploadNew || a.kind == SyncAction::UploadUpdate ||
            a.kind == SyncAction::ConflictKeepLocal) {
            const QString rootAbs = QDir(m_run.pair.localPath).absolutePath();
            const QString src     = absOf(rootAbs, a.relPath);
            QFileInfo fi(src);
            if (!fi.exists() || !fi.isFile()) {
                err = QStringLiteral("本地文件不存在：") + a.relPath;
                return false;
            }

            // 冲突（本地为主）：先把**云端旧内容**下载到冲突副本，再覆盖云端 —— 两份都不丢
            if (a.kind == SyncAction::ConflictKeepLocal && !a.conflictPath.isEmpty()) {
                const SyncSnapshot::Item *oldRem = m_run.remote.find(a.relPath);
                if (oldRem && !oldRem->id.isEmpty()) {
                    QString derr;
                    if (!downloadTo(a.relPath, a.conflictPath, derr)) {
                        err = QStringLiteral("保存云端旧内容为冲突副本失败：") + derr;
                        return false;
                    }
                }
            }

            QString herr;
            const QString hash = hashFile(src, herr);
            if (hash.isEmpty()) { err = herr; return false; }

            const QString remoteDir = normRemoteRoot(m_run.pair.remotePath) +
                                      (dirNameOf(a.relPath).isEmpty()
                                           ? QString()
                                           : (QLatin1Char('/') + dirNameOf(a.relPath)));
            Result<FileItem> dirRes = m_backend->ensureFolderPath(remoteDir);
            if (!dirRes.ok) { err = QStringLiteral("准备云端目录失败：") + dirRes.error; return false; }

            m_backend->setUploadOverwrite(true);
            Result<UploadTicket> t = m_backend->beginUpload(dirRes.value.id,
                                                            baseNameOf(a.relPath),
                                                            fi.size(), hash);
            if (!t.ok) {
                m_backend->setUploadOverwrite(false);
                err = QStringLiteral("发起上传失败：") + t.error;
                return false;
            }

            // 🔴 秒传分支必须单独处理（实测踩过）：内容已在库中时，服务端/本地引擎都返回
            //    "已完成"，**uploadId 是空的** —— 若照常去 putChunk 就会拿到
            //    "上传会话不存在" 并把一次本该成功的上传判成失败。
            //    语义上 instant 表示"内容已在云端就位"，无需再传任何分块。
            if (t.value.instant || t.value.uploadId.isEmpty()) {
                m_backend->setUploadOverwrite(false);
                if (m_index) {
                    LocalIndex::Entry e;
                    e.pairId = m_run.pairId; e.relPath = a.relPath;
                    e.size = fi.size(); e.mtime = fi.lastModified().toUTC();
                    e.hash = hash;   // rev 留空：Ticket 不带 rev，判等靠 hash 即可
                    m_index->put(e);
                }
                m_run.log << QStringLiteral("[%1] %2 — 秒传命中（内容已在云端，未传分块）")
                                 .arg(actionText(a.kind), a.relPath);
                return true;
            }

            QFile in(src);
            if (!in.open(QIODevice::ReadOnly)) {
                m_backend->cancelUpload(t.value.uploadId);
                m_backend->setUploadOverwrite(false);
                err = QStringLiteral("打开本地文件失败：") + in.errorString();
                return false;
            }
            const qint64 cs = m_backend->chunkSize();
            if (cs <= 0) {
                in.close();
                m_backend->cancelUpload(t.value.uploadId);
                m_backend->setUploadOverwrite(false);
                err = QStringLiteral("分块大小为 0，无法上传");
                return false;
            }
            // 分块大小必须与 init 时报给服务端的**完全一致**：HttpBackend::putChunk 会校验
            // `offset % chunkSize == 0` 并用 seq = offset/chunkSize 定位分块，
            // 不一致会直接被判"offset 未按分块大小对齐"。这里与 putChunk 取同一来源。
            const auto uploadAllChunks = [&](QString &serr) -> bool {
                qint64 off = 0;
                while (off < fi.size()) {
                    if (!in.seek(off)) {
                        serr = QStringLiteral("定位本地文件失败 offset=%1").arg(off);
                        return false;
                    }
                    const QByteArray buf = in.read(qMin(cs, fi.size() - off));
                    if (buf.isEmpty())
                        break;
                    Ok po = m_backend->putChunk(t.value.uploadId, off, buf);
                    if (!po.ok) {
                        serr = QStringLiteral("上传分块失败：") + po.error;
                        return false;
                    }
                    off += buf.size();
                }
                return true;
            };

            // 🔴 409(缺块) / 422(坏块) 的恢复（与真实服务端错误码的对接点）：
            //    HttpBackend::finishUpload 会把这两种情况以错误串回传，并保留服务端的
            //    `[missing-chunks]` / `[invalid-chunks]` 标记。正确反应**不是**放弃 ——
            //    那样要等下一轮全量对账才重来（可能几分钟后）。这里把分块重传一遍再 complete，
            //    只重试一次上限，避免坏网络下无限循环。
            const auto recoverableComplete = [](const QString &e) {
                return e.contains(QStringLiteral("missing-chunks")) ||
                       e.contains(QStringLiteral("invalid-chunks"));
            };

            Result<FileItem> fin;
            for (int attempt = 0;; ++attempt) {
                QString serr;
                if (!uploadAllChunks(serr)) {
                    in.close();
                    m_backend->cancelUpload(t.value.uploadId);
                    m_backend->setUploadOverwrite(false);
                    err = serr;
                    return false;
                }
                fin = m_backend->finishUpload(t.value.uploadId);
                if (fin.ok)
                    break;
                if (attempt == 0 && recoverableComplete(fin.error)) {
                    m_run.log << QStringLiteral("[%1] %2 — 服务端报告分块缺失/损坏，重传后重试完成")
                                     .arg(actionText(a.kind), a.relPath);
                    continue;   // 会话仍在，重传分块再 complete
                }
                in.close();
                m_backend->setUploadOverwrite(false);
                err = QStringLiteral("完成上传失败：") + fin.error;
                return false;
            }
            in.close();
            m_backend->setUploadOverwrite(false);

            if (m_index) {
                LocalIndex::Entry e;
                e.pairId = m_run.pairId; e.relPath = a.relPath;
                e.size = fi.size(); e.mtime = fi.lastModified().toUTC();
                e.hash = hash; e.rev = fin.value.rev;
                m_index->put(e);
            }
            return true;
        }

        // ---------------- 下载 ----------------
        if (a.kind == SyncAction::DownloadNew || a.kind == SyncAction::DownloadUpdate ||
            a.kind == SyncAction::ConflictKeepRemote) {
            // 冲突（云端为主）：先把**本地旧内容**另存为冲突副本（本地改名，零网络）
            if (a.kind == SyncAction::ConflictKeepRemote && !a.conflictPath.isEmpty()) {
                const QString rootAbs = QDir(m_run.pair.localPath).absolutePath();
                const QString src     = absOf(rootAbs, a.relPath);
                if (QFileInfo::exists(src)) {
                    const QString dst = absOf(rootAbs, a.conflictPath);
                    QDir().mkpath(dirNameOf(dst));
                    if (!moveTree(src, dst, err)) {
                        err = QStringLiteral("本地旧内容另存为冲突副本失败：") + err;
                        return false;
                    }
                }
            }
            QString derr;
            if (!downloadTo(a.relPath, a.relPath, derr)) { err = derr; return false; }
            return true;
        }

        // ---------------- 跳过 ----------------
        // 只有"两端确认一致"（或两端都无）才推进索引；被方向策略裁剪的跳过**不**推进，
        // 否则会把"本地改了但我们故意没同步"记成已同步，之后再也发现不了这次改动。
        if (a.kind == SyncAction::Skip) {
            const SyncSnapshot::Item *l = m_run.local.find(a.relPath);
            const SyncSnapshot::Item *r = m_run.remote.find(a.relPath);
            if (l && r && l->sameContentAs(*r)) {
                if (m_index) recordFromObserved(a.relPath, *l, *r);
            } else if (!l && !r) {
                if (m_index) m_index->remove(m_run.pairId, a.relPath);
            }
            return true;
        }
        return true;
    }();

    m_run.log << QStringLiteral("[%1] %2 %3%4")
                     .arg(actionText(a.kind), a.relPath,
                          a.reason.isEmpty() ? QString()
                                             : (QStringLiteral(" — ") + a.reason),
                          ok ? QString() : (QStringLiteral("  ✗ ") + err));

    if (!ok) {
        // 单个动作失败不放弃整次对账：其余动作继续，但**索引绝不推进**该路径
        qWarning() << "同步动作失败" << a.relPath << err;
        m_run.failed = true;
        m_run.error  = err;
    }

    ++m_run.actionIndex;
    emit pairProgress(m_run.pairId, QStringLiteral("%1 %2").arg(actionText(a.kind), a.relPath),
                      m_run.actionIndex, m_run.plan.actions.size());

    // 让出一个事件循环轮次：界面能在动作之间重绘（Backend 无异步上传/下载，
    // 单个动作内部仍会阻塞，这里至少保证动作之间不"连坐"）
    QTimer::singleShot(0, this, &SyncEngine::executePhase);
}

void SyncEngine::recordFromObserved(const QString &relPath, const SyncSnapshot::Item &local,
                                    const SyncSnapshot::Item &remote)
{
    if (!m_index)
        return;
    LocalIndex::Entry e;
    e.pairId  = m_run.pairId;
    e.relPath = relPath;
    e.isDir   = local.isDir;
    e.size    = local.size;
    e.mtime   = local.mtime;
    // 哈希优先取本地（客户端自己算的），缺失时退回云端 content_hash
    e.hash = !local.hash.isEmpty() ? local.hash : remote.hash;
    // rev 是云端权威信号，仅云端有
    e.rev = remote.rev;
    m_index->put(e);
}

bool SyncEngine::downloadTo(const QString &remoteRel, const QString &localRel, QString &err)
{
    const SyncSnapshot::Item *rem = m_run.remote.find(remoteRel);
    if (!rem || rem->id.isEmpty()) {
        err = QStringLiteral("云端条目不存在或缺少 id：") + remoteRel;
        return false;
    }

    const QString rootAbs = QDir(m_run.pair.localPath).absolutePath();
    const QString dst     = absOf(rootAbs, localRel);
    QDir().mkpath(dirNameOf(dst));

    // 先写 .tmp，全部成功后再改名 —— 中断/失败都不会留下"看起来完整"的半截文件
    const QString tmp = dst + QStringLiteral(".cvpart");
    QFile out(tmp);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        err = QStringLiteral("无法写入 %1：%2").arg(tmp, out.errorString());
        return false;
    }

    const qint64 cs = qMax<qint64>(1, m_backend->chunkSize());
    qint64 off = 0;
    while (off < rem->size) {
        const qint64 len = qMin(cs, rem->size - off);
        Result<QByteArray> r = m_backend->getRange(rem->id, off, len);
        if (!r.ok) {
            out.close();
            QFile::remove(tmp);
            err = QStringLiteral("下载分块失败：") + r.error;
            return false;
        }
        if (out.write(r.value) != r.value.size()) {
            out.close();
            QFile::remove(tmp);
            err = QStringLiteral("写本地文件失败：") + tmp;
            return false;
        }
        off += r.value.size();
        if (r.value.isEmpty())
            break;   // 防死循环：服务端返回空块即终止
    }
    out.flush();
    out.close();

    if (QFileInfo::exists(dst) && !QFile::remove(dst)) {
        QFile::remove(tmp);
        err = QStringLiteral("覆盖本地文件失败（可能被占用）：") + dst;
        return false;
    }
    if (!QFile::rename(tmp, dst)) {
        QFile::remove(tmp);
        err = QStringLiteral("改名落盘失败：") + dst;
        return false;
    }

    if (m_index) {
        LocalIndex::Entry e;
        e.pairId  = m_run.pairId;
        e.relPath = localRel;
        e.isDir   = false;
        e.size    = rem->size;
        e.mtime   = QFileInfo(dst).lastModified().toUTC();
        e.hash    = rem->hash;
        e.rev     = rem->rev;
        m_index->put(e);
    }
    return true;
}

void SyncEngine::finishCurrent(const QString &error)
{
    for (const QString &line : m_run.log)
        emit logAppended(QStringLiteral("[%1] %2")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
                                  line));

    SyncPair after = m_run.pair;
    after.lastSync = QDateTime::currentDateTimeUtc();
    after.pendingCount = m_run.plan.counts.up + m_run.plan.counts.down +
                         m_run.plan.counts.delLocal + m_run.plan.counts.delRemote;
    after.conflictCount = m_run.plan.counts.conflicts;
    after.lastError     = error.isEmpty() ? m_run.error : error;

    if (!after.lastError.isEmpty())
        after.status = SyncStatus::Error;
    else if (after.conflictCount > 0)
        after.status = SyncStatus::Conflict;
    else
        after.status = m_running ? SyncStatus::Watching : SyncStatus::Idle;

    emit pairStatus(m_run.pairId, int(after.status), after.lastError);
    emit pairSettled(after);

    m_busy = false;
    m_run  = Run();
    QTimer::singleShot(0, this, &SyncEngine::startNext);
}

} // namespace cv

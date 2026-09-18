/****************************************************************************
 * transfer/TransferManager.cpp —— 传输队列实现
 *
 * 逻辑移植自已验证的 client/src/MainWindow.cpp：
 *   · 分块上传 beginChunkedUploadFor / sendInit / pumpChunks / handleChunkReply /
 *     sendComplete / finishChunkSession
 *   · 断点清单 loadManifest / saveManifest / clearManifest
 *   · 下载 sendDownloadRange / pumpDownload / drainDownloadReady / maybeStartWrite /
 *     checkDownloadDone / appendDownloadSegmentAsync / finalizeDownload
 *   · 同名冲突 askOverwrite（这里改为「发信号 → 等 resolveConflict → 回调」）
 *
 * 与母体的差异（lead 已在契约 §8.4 / §8.5 裁定）：
 *   1) 只依赖 net/Backend.h 抽象（阻塞式同步接口），所有网络调用也一并放到线程池；
 *   2) 无独立整文件上传路径（抽象未暴露），统一走分块协议，单块读取上界 = chunkSize；
 *   3) 覆盖同名走**原子覆盖**：setUploadOverwrite(true) → 重新 init → 立即复位 false，
 *      同一工作线程内完成；绝不"先删旧文件再传"（§8.4）；
 *   4) 分块大小单一来源：一律用 backend->chunkSize()，不硬编码（§8.5）；
 *   5) 每个任务带自增代号 gen，回主线程时校验，杜绝过期异步结果污染新任务。
 ****************************************************************************/
#include "TransferManager.h"

#include "core/AppPaths.h"
#include "core/Crypto.h"
#include "core/Types.h"
#include "core/Util.h"
#include "net/Backend.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

namespace cv {
namespace {

// ============================ 冻结常量（契约 §2）============================
// ⚠ 分块大小**不硬编码**：唯一来源是 backend->chunkSize()（契约 §8.5）。若后端给出
//    无效值（<=0），本轮上传 fail-fast 中止——绝不静默采用可能与服务端失配的本地值。
constexpr qint64 kDownloadChunkSize    = 8 * 1024 * 1024;   // 下载每次 Range 拉 8 MiB
constexpr int    kDownloadConcurrency  = 4;                 // 下载并发段数
constexpr int    kMaxConcurrent        = 3;                 // 上传并发块数
constexpr int    kMaxRetries           = 3;                 // 单块最大重试次数
constexpr qint64 kStreamUploadThreshold = 8 * 1024 * 1024;  // 大文件阈值（> 该值走分块日志提示）

// IO / 网络线程池大小（哈希 + 读块 + 阻塞式请求）
constexpr int kIoPoolThreads = 3;

// 服务端「可识别失败」前缀（与 net/HttpBackend 的约定一致；此处仅按前缀判定，
// 不 include HttpBackend，保持只依赖抽象接口）
const QString kConflictPrefix      = QStringLiteral("[conflict]");
const QString kMissingChunksPrefix = QStringLiteral("[missing-chunks]");
const QString kInvalidChunksPrefix = QStringLiteral("[invalid-chunks]");

bool isConflictError(const QString &e) { return e.startsWith(kConflictPrefix); }
bool isMissingChunksError(const QString &e) { return e.startsWith(kMissingChunksPrefix); }
bool isInvalidChunksError(const QString &e) { return e.startsWith(kInvalidChunksPrefix); }

// 从错误文案里解析分块序号列表（"missing=[0,2]" / "invalid=[1]"）
QVector<int> parseSeqList(const QString &e)
{
    QVector<int> out;
    const QStringList keys = {QStringLiteral("missing=["), QStringLiteral("invalid=[")};
    for (const QString &key : keys) {
        const int k = e.indexOf(key);
        if (k < 0)
            continue;
        const int start = k + key.size();
        const int end   = e.indexOf(QLatin1Char(']'), start);
        if (end < 0)
            continue;
        const QString body = e.mid(start, end - start);
        const QStringList toks = body.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &tok : toks) {
            bool      okNum = false;
            const int v     = tok.trimmed().toInt(&okNum);
            if (okNum)
                out.append(v);
        }
    }
    return out;
}

// 纯函数：在线程池里安全执行（不碰任何 QObject / 成员）。
// 流式算整文件 SHA-256（Crypto::sha256File 每次只读 1 MiB，绝不整文件入内存）。
UploadScanResult scanUpload(const QString &path)
{
    UploadScanResult r;
    const QFileInfo  info(path);
    r.name = info.fileName();
    if (r.name.isEmpty())
        r.name = path;
    if (!info.exists() || !info.isFile()) {
        r.err = QStringLiteral("文件不存在或不是普通文件：%1").arg(path);
        return r;
    }
    r.size  = info.size();
    r.mtime = info.lastModified().toMSecsSinceEpoch();
    r.hash  = Crypto::sha256File(path);
    if (r.hash.isEmpty()) {
        r.err = QStringLiteral("读取文件失败（无法计算 SHA-256）：%1").arg(path);
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace

// =========================================================================
//  构造 / 析构
// =========================================================================

TransferManager::TransferManager(Backend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
{
    m_pool = new QThreadPool(this);
    m_pool->setMaxThreadCount(kIoPoolThreads);

    m_manifestFile = AppPaths::configDir() + QStringLiteral("/upload_manifest.json");
    QDir().mkpath(AppPaths::configDir());

    refresh();
}

TransferManager::~TransferManager()
{
    // 丢弃未开始的任务并等待在途任务结束，避免回调打到半析构对象上
    if (m_pool) {
        m_pool->clear();
        m_pool->waitForDone();
    }
}

// =========================================================================
//  QML 属性
// =========================================================================

void TransferManager::refresh()
{
    const int  active = m_pending.size() + (m_hasCurrent ? 1 : 0);
    const bool busy   = (active > 0);

    QString text;
    if (!busy) {
        text = QStringLiteral("空闲");
    } else {
        QString verb;
        if (m_hasCurrent)
            verb = (m_cur.kind == TransferKind::Upload) ? QStringLiteral("上传中")
                                                        : QStringLiteral("下载中");
        else
            verb = (m_pending.first().kind == TransferKind::Upload) ? QStringLiteral("上传中")
                                                                    : QStringLiteral("下载中");
        // 形如 "… 上传中 3/8"
        text = QStringLiteral("… %1 %2/%3").arg(verb).arg(m_batchDone).arg(m_batchDone + active);
    }

    const bool dirty = (active != m_activeCount) || (busy != m_busy) || (text != m_statusText);
    m_activeCount    = active;
    m_busy           = busy;
    m_statusText     = text;
    if (dirty)
        emit changed();
}

QString TransferManager::newTaskId()
{
    return QStringLiteral("task-%1").arg(++m_taskSeq);
}

// =========================================================================
//  入队 / 取消
// =========================================================================

void TransferManager::enqueueUpload(const QStringList &localPaths, const QString &dir)
{
    if (localPaths.isEmpty())
        return;
    for (const QString &p : localPaths) {
        Task t;
        t.id        = newTaskId();
        t.kind      = TransferKind::Upload;
        t.localPath = p;
        t.dir       = dir;
        t.name      = QFileInfo(p).fileName(); // 基名（如 报告.pdf）
        m_pending.append(t);
        // 任务身份（基名 / 类型 / 总量）——上层据此在 TransferModel 建行；每任务仅一次
        emit taskStarted(t.id, t.name, TransferKind::Upload, QFileInfo(p).size());
    }
    emit logMessage(QStringLiteral("info"),
                    QStringLiteral("加入上传队列 %1 项 → 目录「%2」")
                        .arg(localPaths.size())
                        .arg(dir.isEmpty() ? QStringLiteral("根目录") : dir));
    refresh();
    pump();
}

void TransferManager::enqueueDownload(const QStringList &fileIds, const QString &destDir)
{
    if (fileIds.isEmpty())
        return;
    for (const QString &id : fileIds) {
        Task t;
        t.id      = newTaskId();
        t.kind    = TransferKind::Download;
        t.fileId  = id;
        t.destDir = destDir;
        t.name    = id; // 真实文件名在 statById 后补齐
        m_pending.append(t);
    }
    emit logMessage(QStringLiteral("info"),
                    QStringLiteral("加入下载队列 %1 项 → 目录「%2」")
                        .arg(fileIds.size())
                        .arg(resolveDownloadDir(destDir)));
    refresh();
    pump();
}

void TransferManager::setDefaultDownloadDir(const QString &dir)
{
    // 由 Application 从 Settings 注入（改设置后重新注入即生效）
    m_defaultDownloadDir = dir;
}

QString TransferManager::resolveDownloadDir(const QString &destDir) const
{
    // 回退链：显式 destDir > 注入的默认下载目录 > AppPaths::downloadDir()（兜底）
    if (!destDir.isEmpty())
        return destDir;
    if (!m_defaultDownloadDir.isEmpty())
        return m_defaultDownloadDir;
    return AppPaths::downloadDir();
}

void TransferManager::cancelCurrent()
{
    if (!m_hasCurrent) {
        if (!m_pending.isEmpty()) {
            m_pending.clear();
            emit logMessage(QStringLiteral("warn"), QStringLiteral("已清空排队中的传输任务"));
            refresh();
        }
        return;
    }

    m_cur.cancelRequested = true;
    emit logMessage(QStringLiteral("warn"),
                    QStringLiteral("用户取消：%1")
                        .arg(m_cur.kind == TransferKind::Upload ? m_cur.name : m_cur.fileId));

    if (m_cur.kind == TransferKind::Upload && !m_cur.uploadId.isEmpty()) {
        // 通知服务端丢弃会话；但保留本地 manifest，下次仍可续传
        const QString uploadId = m_cur.uploadId;
        Backend      *be       = m_backend;
        QThreadPool  *pool     = m_pool;
        pool->start([be, uploadId]() {
            if (be)
                be->cancelUpload(uploadId);
        });
    }
    // 下载：保留 .part，断点续传语义不变

    finishTask(false, QStringLiteral("已取消"));
}

// =========================================================================
//  队列推进
// =========================================================================

void TransferManager::pump()
{
    if (m_hasCurrent)
        return;
    if (m_pending.isEmpty()) {
        m_batchDone = 0;
        refresh();
        return;
    }
    m_cur        = m_pending.takeFirst();
    m_cur.gen    = ++m_genSeq;
    m_hasCurrent = true;
    m_cur.state  = TransferState::Queued;
    refresh();
    startCurrent();
}

void TransferManager::startCurrent()
{
    if (!m_backend) {
        finishTask(false, QStringLiteral("未配置数据后端（Backend）"));
        return;
    }
    if (m_cur.kind == TransferKind::Upload)
        beginUpload();
    else
        beginDownload();
}

void TransferManager::finishTask(bool ok, const QString &message)
{
    if (!m_hasCurrent)
        return;
    const QString id       = m_cur.id;
    const bool    canceled = m_cur.cancelRequested;

    m_cur.state = ok ? TransferState::Completed
                     : (canceled ? TransferState::Canceled : TransferState::Failed);
    m_cur.readySegments.clear();
    m_cur.writeQueue.clear();
    m_hasCurrent = false;
    m_cur        = Task();
    ++m_batchDone;

    if (canceled)
        m_pending.clear(); // 取消 = 终止整批

    if (!message.isEmpty()) {
        emit logMessage(ok ? QStringLiteral("info")
                           : (canceled ? QStringLiteral("warn") : QStringLiteral("error")),
                        message);
    }
    emit taskFinished(id, ok, message);
    refresh();
    QTimer::singleShot(0, this, [this]() { pump(); });
}

// =========================================================================
//  上传：扫描 → init → 并发 PUT → complete
// =========================================================================

void TransferManager::beginUpload()
{
    m_cur.state = TransferState::Hashing;
    refresh();
    emit logMessage(QStringLiteral("info"), QStringLiteral("开始扫描上传：%1").arg(m_cur.localPath));

    const QString             path = m_cur.localPath;
    const qint64              gen  = m_cur.gen;
    QThreadPool              *pool = m_pool;
    QPointer<TransferManager> self(this);
    pool->start([self, path, gen]() {
        const UploadScanResult r = scanUpload(path);
        if (self)
            QMetaObject::invokeMethod(
                self.data(), [self, gen, r]() { if (self) self->onUploadScanned(gen, r); },
                Qt::QueuedConnection);
    });
}

void TransferManager::onUploadScanned(qint64 gen, const UploadScanResult &r)
{
    if (!m_hasCurrent || m_cur.kind != TransferKind::Upload || m_cur.gen != gen)
        return; // 过期回调
    if (m_cur.cancelRequested) {
        finishTask(false, QStringLiteral("已取消"));
        return;
    }
    if (!r.ok) {
        finishTask(false, QStringLiteral("读取本机文件失败：%1").arg(r.err));
        return;
    }

    // 分块大小唯一来源：后端（§8.5）。无效值（<=0）→ fail-fast，绝不静默兜底：
    // 兜底一个与服务端不同的块大小会让 putChunk 的 offset/seq 失配 → 服务端 400。
    const qint64 chunkBytes = m_backend ? m_backend->chunkSize() : 0;
    if (chunkBytes <= 0) {
        finishTask(false,
                   QStringLiteral("后端未提供有效的分块大小（chunkSize()=%1），已中止上传以免与"
                                  "服务端失配")
                       .arg(chunkBytes));
        return;
    }

    m_cur.name        = r.name;
    m_cur.mtime       = r.mtime;
    m_cur.total       = r.size;
    m_cur.fullHash    = r.hash;
    m_cur.chunkSize   = chunkBytes;
    m_cur.chunks      = makeChunkPlan(r.size, chunkBytes);
    m_cur.totalChunks = m_cur.chunks.size();
    m_cur.done        = 0;
    m_cur.doneSeq.clear();
    m_cur.inflightSeq.clear();
    m_cur.retryWait.clear();
    m_cur.retries.clear();
    m_cur.resumeHit    = false;
    m_cur.completeSent = false;
    m_cur.uploadId.clear();
    m_cur.state = TransferState::Running;
    // 任务开始即发一次进度（done=0），让模型立刻显示 0%
    emit taskProgress(m_cur.id, 0, m_cur.total);

    if (m_cur.total > kStreamUploadThreshold)
        emit logMessage(QStringLiteral("info"),
                        QStringLiteral("大文件 %1（%2）走分块流式上传（逐块 seek 读，内存上界 %3）")
                            .arg(m_cur.name, Util::humanSize(m_cur.total),
                                 Util::humanSize(m_cur.chunkSize)));

    // 本地 manifest 续传：size/mtime/chunk_size/full_hash 全一致才复用
    QString   manifestUploadId;
    QSet<int> manifestSeqs;
    if (loadManifest(m_cur.localPath, m_cur.total, m_cur.mtime, m_cur.chunkSize, m_cur.fullHash,
                     &manifestUploadId, &manifestSeqs)) {
        m_cur.resumeHit = true;
        m_cur.uploadId  = manifestUploadId;
        m_cur.doneSeq   = manifestSeqs;
        emit logMessage(QStringLiteral("info"),
                        QStringLiteral("命中本地断点：已确认 %1/%2 块，尝试复用 upload_id=%3")
                            .arg(manifestSeqs.size())
                            .arg(m_cur.totalChunks)
                            .arg(manifestUploadId));
    }
    refresh();
    requestBeginUpload(false);
}

void TransferManager::requestBeginUpload(bool overwrite)
{
    // parentId 语义：'' → 根（用 kRootId，兼容 HttpBackend("root"→"") 与本地引擎）
    const QString parentId = m_cur.dir.isEmpty() ? kRootId : m_cur.dir;
    const QString name     = m_cur.name;
    const qint64  size     = m_cur.total;
    const QString hash     = m_cur.fullHash;
    const qint64  gen      = m_cur.gen;

    Backend                  *be   = m_backend;
    QThreadPool              *pool = m_pool;
    QPointer<TransferManager> self(this);
    pool->start([self, be, parentId, name, size, hash, gen, overwrite]() {
        Result<UploadTicket> r; // 默认 ok=false；下面各分支都会赋值
        if (overwrite) {
            // §8.4 原子覆盖：置真 → 发起 init → 立即复位，三步在同一工作线程内顺序完成，
            // 保证「只对随后一次上传生效」；绝不"先删旧文件再传"（服务端无回收站）。
            be->setUploadOverwrite(true);
            r = be->beginUpload(parentId, name, size, hash);
            be->setUploadOverwrite(false);
        } else {
            r = be->beginUpload(parentId, name, size, hash);
        }
        if (self)
            QMetaObject::invokeMethod(
                self.data(), [self, gen, r]() { if (self) self->onBeginUpload(gen, r); },
                Qt::QueuedConnection);
    });

    emit logMessage(QStringLiteral("info"),
                    QStringLiteral("init 上传会话：%1（%2，目标=%3，hash=%4%5）")
                        .arg(name, Util::humanSize(size),
                             m_cur.dir.isEmpty() ? QStringLiteral("根目录") : m_cur.dir, hash,
                             overwrite ? QStringLiteral("，覆盖同名") : QString()));
}

void TransferManager::onBeginUpload(qint64 gen, const Result<UploadTicket> &r)
{
    if (!m_hasCurrent || m_cur.kind != TransferKind::Upload || m_cur.gen != gen)
        return;
    if (m_cur.cancelRequested) {
        finishTask(false, QStringLiteral("已取消"));
        return;
    }
    if (!r.ok) {
        if (isConflictError(r.error)) {
            onUploadConflict(r.error);
            return;
        }
        finishTask(false, QStringLiteral("初始化上传会话失败：%1").arg(r.error));
        return;
    }

    const UploadTicket &t = r.value;
    if (t.instant) {
        m_cur.done = m_cur.total;
        emit taskProgress(m_cur.id, m_cur.total, m_cur.total);
        clearManifest(m_cur.localPath);
        finishTask(true, QStringLiteral("秒传完成：%1（服务端已存在相同内容）").arg(m_cur.name));
        return;
    }

    m_cur.uploadId = t.uploadId;
    // 服务端已收字节数 → 连续前缀；与本地 manifest 已确认集合求并（PUT 幂等，重复无害）
    for (int seq = 0; seq < m_cur.totalChunks; ++seq) {
        const ChunkRange &c = m_cur.chunks.at(seq);
        if (c.offset + c.length <= t.received)
            m_cur.doneSeq.insert(seq);
    }
    saveManifest();
    updateUploadProgress();
    emit logMessage(QStringLiteral("info"),
                    QStringLiteral("会话就绪 upload_id=%1，已确认 %2/%3 块")
                        .arg(t.uploadId)
                        .arg(m_cur.doneSeq.size())
                        .arg(m_cur.totalChunks));

    if (m_cur.totalChunks == 0) { // 空文件：直接合并
        requestComplete();
        return;
    }
    pumpChunks();
}

void TransferManager::onUploadConflict(const QString &errorText)
{
    m_cur.awaitingConflict = true;
    m_cur.state            = TransferState::Paused;
    refresh();
    emit logMessage(QStringLiteral("warn"), QStringLiteral("命中同名文件：%1").arg(errorText));
    emit conflictDetected(m_cur.id, m_cur.dir, m_cur.name);
}

void TransferManager::resolveConflict(const QString &taskId, bool overwrite)
{
    if (!m_hasCurrent || m_cur.id != taskId || !m_cur.awaitingConflict) {
        emit logMessage(QStringLiteral("warn"),
                        QStringLiteral("忽略无对应冲突的应答（taskId=%1）").arg(taskId));
        return;
    }
    m_cur.awaitingConflict = false;

    if (!overwrite) {
        finishTask(false, QStringLiteral("已跳过同名文件「%1」").arg(m_cur.name));
        return;
    }

    // 覆盖：走服务端**原子覆盖**语义（§8.4）——丢弃本地进度/manifest，重新 init 一次。
    // 绝不"先删旧文件再传"（服务端无回收站，中途失败会永久丢原文件）。
    m_cur.overwrite = true;
    m_cur.state     = TransferState::Running;
    clearManifest(m_cur.localPath);
    m_cur.doneSeq.clear();
    m_cur.inflightSeq.clear();
    m_cur.retryWait.clear();
    m_cur.retries.clear();
    m_cur.resumeHit    = false;
    m_cur.completeSent = false;
    refresh();
    requestBeginUpload(true); // 置真 → init → 复位 在同一工作线程内原子完成
}

void TransferManager::pumpChunks()
{
    if (!m_hasCurrent || m_cur.kind != TransferKind::Upload || m_cur.cancelRequested)
        return;

    for (int seq = 0; seq < m_cur.totalChunks; ++seq) {
        if (!m_hasCurrent || m_cur.kind != TransferKind::Upload || m_cur.cancelRequested)
            return;
        if (m_cur.doneSeq.contains(seq) || m_cur.inflightSeq.contains(seq)
            || m_cur.retryWait.contains(seq))
            continue;
        if (m_cur.inflightSeq.size() >= kMaxConcurrent)
            break;
        launchChunkUpload(seq);
    }

    const bool allDone =
        (m_cur.totalChunks == 0)
        || (m_cur.doneSeq.size() >= m_cur.totalChunks && m_cur.inflightSeq.isEmpty()
            && m_cur.retryWait.isEmpty());
    if (allDone && !m_cur.completeSent) {
        m_cur.completeSent = true;
        requestComplete();
    }
}

void TransferManager::launchChunkUpload(int seq)
{
    if (seq < 0 || seq >= m_cur.chunks.size())
        return;

    m_cur.inflightSeq.insert(seq);

    const QString    path     = m_cur.localPath;
    const ChunkRange plan     = m_cur.chunks.at(seq);
    const QString    uploadId = m_cur.uploadId;
    const qint64     gen      = m_cur.gen;

    Backend                  *be   = m_backend;
    QThreadPool              *pool = m_pool;
    QPointer<TransferManager> self(this);
    pool->start([self, be, path, plan, uploadId, seq, gen]() {
        Ok r = err(QStringLiteral("内部错误"));
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            r = err(QStringLiteral("打开本机文件失败：%1").arg(f.errorString()));
        } else if (!f.seek(plan.offset)) {
            r = err(QStringLiteral("定位分块失败：%1").arg(f.errorString()));
            f.close();
        } else {
            const QByteArray data = f.read(plan.length);
            f.close();
            if (data.size() != plan.length) {
                r = err(QStringLiteral("读取分块 %1 失败（期望 %2 字节，实得 %3）")
                            .arg(seq)
                            .arg(plan.length)
                            .arg(data.size()));
            } else {
                // 分块 SHA-256 由 net 层（HttpBackend::putChunk）统一计算并带 X-Chunk-SHA256
                r = be->putChunk(uploadId, plan.offset, data);
            }
        }
        if (self)
            QMetaObject::invokeMethod(
                self.data(), [self, gen, seq, r]() { if (self) self->onChunkUploaded(gen, seq, r); },
                Qt::QueuedConnection);
    });
}

void TransferManager::onChunkUploaded(qint64 gen, int seq, const Ok &r)
{
    if (!m_hasCurrent || m_cur.kind != TransferKind::Upload || m_cur.gen != gen)
        return;
    if (m_cur.cancelRequested) {
        m_cur.inflightSeq.remove(seq);
        m_cur.retryWait.remove(seq);
        return;
    }
    m_cur.inflightSeq.remove(seq);

    if (!r.ok) {
        const int tries = m_cur.retries.value(seq, 0);
        if (tries < kMaxRetries) {
            m_cur.retries.insert(seq, tries + 1);
            m_cur.retryWait.insert(seq); // 退避期间不参与补位（避免与定时器重复派发）
            const int delay = (tries == 0) ? 500 : (tries == 1 ? 1000 : 2000);
            emit logMessage(QStringLiteral("warn"),
                            QStringLiteral("分块 %1 失败（第 %2 次重试，%3 ms 后再试）：%4")
                                .arg(seq)
                                .arg(tries + 1)
                                .arg(delay)
                                .arg(r.error));
            QTimer::singleShot(delay, this, [this, gen, seq]() {
                if (!m_hasCurrent || m_cur.kind != TransferKind::Upload || m_cur.gen != gen
                    || m_cur.cancelRequested)
                    return;
                m_cur.retryWait.remove(seq);
                if (m_cur.doneSeq.contains(seq))
                    return;
                launchChunkUpload(seq);
                pumpChunks();
            });
            return;
        }
        finishTask(false, QStringLiteral("分块 %1 重试耗尽：%2").arg(seq).arg(r.error));
        return;
    }

    m_cur.doneSeq.insert(seq);
    m_cur.retries.remove(seq);
    m_cur.retryWait.remove(seq);
    saveManifest();
    updateUploadProgress();
    pumpChunks();
}

void TransferManager::updateUploadProgress()
{
    if (!m_hasCurrent)
        return;
    qint64 doneBytes = 0;
    for (int seq : m_cur.doneSeq) {
        if (seq >= 0 && seq < m_cur.chunks.size())
            doneBytes += m_cur.chunks.at(seq).length;
    }
    m_cur.done = qMin(doneBytes, m_cur.total);
    emit taskProgress(m_cur.id, m_cur.done, m_cur.total);
}

void TransferManager::requestComplete()
{
    const QString uploadId = m_cur.uploadId;
    const qint64  gen      = m_cur.gen;

    Backend                  *be   = m_backend;
    QThreadPool              *pool = m_pool;
    QPointer<TransferManager> self(this);
    emit logMessage(QStringLiteral("info"),
                    QStringLiteral("所有分块已传完，请求合并（upload_id=%1）").arg(uploadId));
    pool->start([self, be, uploadId, gen]() {
        const Result<FileItem> r = be->finishUpload(uploadId);
        if (self)
            QMetaObject::invokeMethod(
                self.data(), [self, gen, r]() { if (self) self->onComplete(gen, r); },
                Qt::QueuedConnection);
    });
}

void TransferManager::onComplete(qint64 gen, const Result<FileItem> &r)
{
    if (!m_hasCurrent || m_cur.kind != TransferKind::Upload || m_cur.gen != gen)
        return;
    if (m_cur.cancelRequested) {
        finishTask(false, QStringLiteral("已取消"));
        return;
    }

    if (r.ok) {
        clearManifest(m_cur.localPath);
        emit logMessage(QStringLiteral("info"),
                        QStringLiteral("上传完成：%1（file_id=%2）").arg(m_cur.name, r.value.id));
        finishTask(true, QStringLiteral("上传完成：%1").arg(m_cur.name));
        return;
    }

    if (isMissingChunksError(r.error) || isInvalidChunksError(r.error)) {
        const QVector<int> seqs = parseSeqList(r.error);
        bool               any  = false;
        for (int s : seqs) {
            if (s >= 0 && s < m_cur.totalChunks) {
                m_cur.doneSeq.remove(s);
                m_cur.retries.remove(s);
                m_cur.retryWait.remove(s);
                any = true;
            }
        }
        if (!any) {
            // 哈希不符且无坏分块：重传无济于事，保留会话供排查
            finishTask(false, QStringLiteral("合并上传失败：%1").arg(r.error));
            return;
        }
        m_cur.completeSent = false;
        emit logMessage(QStringLiteral("warn"),
                        QStringLiteral("服务端报告缺块/坏块，正在补传：%1").arg(r.error));
        pumpChunks();
        return;
    }

    finishTask(false, QStringLiteral("合并上传失败：%1").arg(r.error));
}

// =========================================================================
//  下载：statById → 4 路 Range 窗口 → 按序落盘 → 收尾
// =========================================================================

void TransferManager::beginDownload()
{
    m_cur.state = TransferState::Hashing; // 查询元数据阶段
    refresh();

    const QString fileId = m_cur.fileId;
    const qint64  gen    = m_cur.gen;

    Backend                  *be   = m_backend;
    QThreadPool              *pool = m_pool;
    QPointer<TransferManager> self(this);
    pool->start([self, be, fileId, gen]() {
        const Result<FileItem> r = be->statById(fileId);
        if (self)
            QMetaObject::invokeMethod(
                self.data(), [self, gen, r]() { if (self) self->onDownloadStat(gen, r); },
                Qt::QueuedConnection);
    });
    emit logMessage(QStringLiteral("info"), QStringLiteral("开始下载：file_id=%1").arg(fileId));
}

void TransferManager::onDownloadStat(qint64 gen, const Result<FileItem> &r)
{
    if (!m_hasCurrent || m_cur.kind != TransferKind::Download || m_cur.gen != gen)
        return;
    if (m_cur.cancelRequested) {
        finishTask(false, QStringLiteral("已取消"));
        return;
    }
    if (!r.ok) {
        finishTask(false, QStringLiteral("获取文件元数据失败：%1").arg(r.error));
        return;
    }

    const FileItem &f = r.value;
    if (f.isDir) {
        finishTask(false, QStringLiteral("暂不支持下载文件夹"));
        return;
    }

    // 文件名取末段并清洗，杜绝路径穿越写盘
    QString nm = QFileInfo(f.name).fileName();
    if (nm.isEmpty())
        nm = m_cur.fileId;
    m_cur.name  = nm;
    m_cur.total = f.size;

    // 任务身份（服务端文件名 / 类型 / 总量）——上层据此在 TransferModel 建行；每任务仅一次
    emit taskStarted(m_cur.id, m_cur.name, TransferKind::Download, m_cur.total);

    const QString dest = resolveDownloadDir(m_cur.destDir);
    if (!QDir().mkpath(dest)) {
        finishTask(false, QStringLiteral("无法创建保存目录：%1").arg(dest));
        return;
    }
    m_cur.finalPath = Util::joinPath(dest, m_cur.name);
    m_cur.partPath  = m_cur.finalPath + QStringLiteral(".part");

    qint64 offset = 0;
    if (QFile::exists(m_cur.partPath))
        offset = QFileInfo(m_cur.partPath).size(); // 断点：.part 当前大小
    if (offset > m_cur.total) {                    // .part 异常（比目标还大）→ 丢弃重来
        QFile::remove(m_cur.partPath);
        offset = 0;
    }

    m_cur.done             = offset;
    m_cur.nextReqOffset    = offset;
    m_cur.inflightSegments = 0;
    m_cur.writing          = false;
    m_cur.readySegments.clear();
    m_cur.writeQueue.clear();
    m_cur.state = TransferState::Running;
    emit taskProgress(m_cur.id, m_cur.done, m_cur.total);

    if (m_cur.total <= 0) { // 空文件
        finalizeDownload();
        return;
    }
    if (offset >= m_cur.total) { // 已下完，直接收尾
        finalizeDownload();
        return;
    }

    emit logMessage(QStringLiteral("info"),
                    QStringLiteral("下载 %1（%2）→ %3%4")
                        .arg(m_cur.name, Util::humanSize(m_cur.total), m_cur.finalPath,
                             offset > 0 ? QStringLiteral("，从 offset=%1 续传").arg(offset)
                                        : QString()));
    pumpDownload();
}

void TransferManager::pumpDownload()
{
    while (m_hasCurrent && m_cur.kind == TransferKind::Download && !m_cur.cancelRequested
           && m_cur.inflightSegments < kDownloadConcurrency && m_cur.nextReqOffset < m_cur.total) {
        const qint64 off = m_cur.nextReqOffset;
        const qint64 len = qMin<qint64>(kDownloadChunkSize, m_cur.total - off);
        ++m_cur.inflightSegments;
        m_cur.nextReqOffset += len;
        launchRangeRead(off, len);
    }
}

void TransferManager::launchRangeRead(qint64 offset, qint64 length)
{
    const QString fileId = m_cur.fileId;
    const qint64  gen    = m_cur.gen;

    Backend                  *be   = m_backend;
    QThreadPool              *pool = m_pool;
    QPointer<TransferManager> self(this);
    pool->start([self, be, fileId, offset, length, gen]() {
        const Result<QByteArray> r = be->getRange(fileId, offset, length);
        if (self)
            QMetaObject::invokeMethod(
                self.data(),
                [self, gen, offset, r]() { if (self) self->onRangeData(gen, offset, r); },
                Qt::QueuedConnection);
    });
}

void TransferManager::onRangeData(qint64 gen, qint64 offset, const Result<QByteArray> &r)
{
    if (!m_hasCurrent || m_cur.kind != TransferKind::Download || m_cur.gen != gen)
        return;
    if (m_cur.cancelRequested)
        return;
    if (m_cur.inflightSegments > 0)
        --m_cur.inflightSegments;

    if (!r.ok) {
        finishTask(false, QStringLiteral("分段下载失败（offset=%1）：%2").arg(offset).arg(r.error));
        return;
    }

    const QByteArray data = r.value;
    const qint64     expected = qMin<qint64>(kDownloadChunkSize, m_cur.total - offset);
    if (data.size() != expected) {
        finishTask(false,
                   QStringLiteral("数据段长度不符（offset=%1，期望 %2 字节，实得 %3）")
                       .arg(offset)
                       .arg(expected)
                       .arg(data.size()));
        return;
    }

    // 乱序到达先缓存（起点低于已写位置的重复段直接丢弃），按序推进写盘
    if (offset >= m_cur.done && !m_cur.readySegments.contains(offset))
        m_cur.readySegments.insert(offset, data);

    drainReady();
    maybeWrite();
    pumpDownload();
    checkDownloadDone();
}

void TransferManager::drainReady()
{
    for (auto it = m_cur.readySegments.begin(); it != m_cur.readySegments.end();) {
        if (it.key() != m_cur.done)
            break; // 段序尚未接上
        m_cur.writeQueue.append(qMakePair(it.key(), it.value()));
        it = m_cur.readySegments.erase(it);
    }
}

void TransferManager::maybeWrite()
{
    if (!m_hasCurrent || m_cur.kind != TransferKind::Download)
        return;
    if (m_cur.writing || m_cur.writeQueue.isEmpty() || m_cur.cancelRequested)
        return;
    const QPair<qint64, QByteArray> seg = m_cur.writeQueue.takeFirst();
    m_cur.writing                       = true;
    launchWrite(seg.first, seg.second);
}

void TransferManager::launchWrite(qint64 offset, const QByteArray &data)
{
    const QString partPath = m_cur.partPath;
    const qint64  gen      = m_cur.gen;

    QThreadPool              *pool = m_pool;
    QPointer<TransferManager> self(this);
    pool->start([self, partPath, data, offset, gen]() {
        bool    ok = false;
        QString e;
        QFile   part(partPath);
        if (!part.open(QIODevice::ReadWrite)) {
            e = part.errorString();
        } else if (!part.seek(offset)) {
            e = QStringLiteral("seek 失败：%1").arg(part.errorString());
            part.close();
        } else if (part.write(data) != data.size()) {
            e = QStringLiteral("写入不足：%1").arg(part.errorString());
            part.close();
        } else {
            part.close();
            ok = true;
        }
        if (self)
            QMetaObject::invokeMethod(
                self.data(), [self, gen, ok, e]() { if (self) self->onSegmentWritten(gen, ok, e); },
                Qt::QueuedConnection);
    });
}

void TransferManager::onSegmentWritten(qint64 gen, bool ok, const QString &err)
{
    if (!m_hasCurrent || m_cur.kind != TransferKind::Download || m_cur.gen != gen)
        return;
    m_cur.writing = false;
    if (m_cur.cancelRequested)
        return;
    if (!ok) {
        finishTask(false, QStringLiteral("写入下载文件失败：%1").arg(err));
        return;
    }

    m_cur.done = QFileInfo(m_cur.partPath).size(); // 实际落盘字节 = .part 当前大小
    emit taskProgress(m_cur.id, m_cur.done, m_cur.total);

    if (m_cur.total > 0 && m_cur.done >= m_cur.total) {
        finalizeDownload();
        return;
    }
    drainReady();
    maybeWrite();
    pumpDownload();
    checkDownloadDone();
}

void TransferManager::checkDownloadDone()
{
    if (!m_hasCurrent || m_cur.kind != TransferKind::Download)
        return;
    if (m_cur.writing || m_cur.inflightSegments > 0 || !m_cur.readySegments.isEmpty()
        || !m_cur.writeQueue.isEmpty())
        return;
    if (m_cur.total > 0 && m_cur.done >= m_cur.total)
        finalizeDownload();
}

void TransferManager::finalizeDownload()
{
    if (m_cur.total > 0) {
        const qint64 partSize = QFileInfo(m_cur.partPath).size();
        if (partSize != m_cur.total) {
            emit logMessage(QStringLiteral("warn"),
                            QStringLiteral("下载大小不符（.part=%1，期望=%2），仍按现状落盘")
                                .arg(partSize)
                                .arg(m_cur.total));
        }
    } else {
        // 空文件：确保存在一个空的 .part 以便改名落盘
        if (!QFile::exists(m_cur.partPath)) {
            QFile empty(m_cur.partPath);
            if (!empty.open(QIODevice::WriteOnly)) {
                finishTask(false, QStringLiteral("创建空文件失败：%1").arg(empty.errorString()));
                return;
            }
            empty.close();
        }
    }

    if (QFile::exists(m_cur.finalPath) && !QFile::remove(m_cur.finalPath))
        emit logMessage(QStringLiteral("warn"),
                        QStringLiteral("无法删除已存在的目标文件：%1").arg(m_cur.finalPath));

    if (!QFile::rename(m_cur.partPath, m_cur.finalPath)) {
        finishTask(false,
                   QStringLiteral("重命名落盘失败：%1 → %2").arg(m_cur.partPath, m_cur.finalPath));
        return;
    }

    const QString saved = m_cur.finalPath;
    emit logMessage(QStringLiteral("info"), QStringLiteral("下载完成：%1").arg(saved));
    finishTask(true, QStringLiteral("已保存到 %1").arg(saved));
}

// =========================================================================
//  工具：分块表 / manifest
// =========================================================================

QList<ChunkRange> TransferManager::makeChunkPlan(qint64 size, qint64 chunkBytes) const
{
    QList<ChunkRange> plan;
    // chunkBytes 由调用方保证 > 0（无效时已在 onUploadScanned 处 fail-fast）；此处不再兜底
    if (size <= 0 || chunkBytes <= 0)
        return plan;
    qint64 off = 0;
    while (off < size) {
        const qint64 len = qMin<qint64>(chunkBytes, size - off);
        plan.append(ChunkRange{off, len});
        off += len;
    }
    return plan;
}

bool TransferManager::loadManifest(const QString &path, qint64 size, qint64 mtime, qint64 chunkBytes,
                                   const QString &hash, QString *uploadId, QSet<int> *seqs) const
{
    QFile f(m_manifestFile);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject())
        return false;

    const QJsonObject entry = doc.object().value(path).toObject();
    if (entry.isEmpty())
        return false;

    const qint64  esize   = entry.value(QStringLiteral("size")).toVariant().toLongLong();
    const qint64  emtime  = entry.value(QStringLiteral("mtime")).toVariant().toLongLong();
    const qint64  echunk  = entry.value(QStringLiteral("chunk_size")).toVariant().toLongLong();
    const QString ehash   = entry.value(QStringLiteral("full_hash")).toString();
    // size/mtime/chunk_size/full_hash 全一致才续传（块大小变了 seq 语义随之变化）
    if (esize != size || emtime != mtime || echunk != chunkBytes || ehash != hash)
        return false; // 源文件或块大小已变化，废弃该记录（从头传）

    if (uploadId)
        *uploadId = entry.value(QStringLiteral("upload_id")).toString();
    if (seqs) {
        seqs->clear();
        const QJsonArray arr = entry.value(QStringLiteral("uploaded_seqs")).toArray();
        for (const QJsonValue &v : arr)
            seqs->insert(v.toInt());
    }
    return true;
}

void TransferManager::saveManifest() const
{
    if (!m_hasCurrent)
        return;
    QDir().mkpath(AppPaths::configDir());

    QJsonObject root;
    {
        QFile f(m_manifestFile);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject())
                root = doc.object();
            f.close();
        }
    }

    QJsonObject entry;
    entry.insert(QStringLiteral("abs_path"), m_cur.localPath);
    entry.insert(QStringLiteral("name"), m_cur.name);
    entry.insert(QStringLiteral("size"), double(m_cur.total));
    entry.insert(QStringLiteral("mtime"), double(m_cur.mtime));
    entry.insert(QStringLiteral("full_hash"), m_cur.fullHash);
    entry.insert(QStringLiteral("upload_id"), m_cur.uploadId);
    entry.insert(QStringLiteral("chunk_size"), double(m_cur.chunkSize)); // 单一来源（§8.5）
    QJsonArray uploaded;
    for (int s : m_cur.doneSeq)
        uploaded.append(s);
    entry.insert(QStringLiteral("uploaded_seqs"), uploaded);
    root.insert(m_cur.localPath, entry);

    QFile f(m_manifestFile);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.close();
    }
}

void TransferManager::clearManifest(const QString &path) const
{
    QFile f(m_manifestFile);
    if (!f.open(QIODevice::ReadOnly))
        return;
    QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    if (root.isEmpty())
        return;

    root.remove(path);
    if (root.isEmpty()) {
        QFile::remove(m_manifestFile);
        return;
    }
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.close();
    }
}

} // namespace cv

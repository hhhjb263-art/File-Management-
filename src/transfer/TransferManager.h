/****************************************************************************
 * transfer/TransferManager.h —— 传输队列（分块上传 / Range 并发下载）
 *
 * 职责（接口见 docs/整合实现契约.md 第 2 节，已冻结）：
 *   · 分块上传：init → 3 块并发 PUT chunk → complete；秒传（done:true）直接完成
 *   · 本地 manifest（size/mtime/full_hash 三者一致才续传；complete 成功后清除）
 *   · Range 下载：4 路并发窗口，乱序到达先缓存、按序落盘（写盘串行化，防写坏 .part）
 *   · 队列与进度、同名冲突询问（409 → conflictDetected → resolveConflict）
 *
 * 依赖边界：只依赖 net/Backend.h 抽象接口（不 include HttpBackend）；不 include
 * controllers / data / qml。所有耗时操作（哈希、读块、阻塞式网络）都在线程池里跑。
 *
 * 两条 lead 裁决（契约 §8.4 / §8.5）：
 *   · 覆盖同名必须原子：命中冲突 → conflictDetected → resolveConflict(true) →
 *     backend->setUploadOverwrite(true) → 重新 init → 立即复位 false；
 *     严禁"先删旧文件再传"（服务端无回收站）。
 *   · 分块大小单一来源：offset 步长 / seq 推导一律用 backend->chunkSize()，不硬编码。
 *
 * 线程模型（无锁：主线程独占状态）：
 *   线程池 lambda 只捕获「值拷贝」与 QPointer；绝不读写本对象成员；
 *   结果一律经 QMetaObject::invokeMethod(self, …, Qt::QueuedConnection) 回主线程。
 *   每个任务带一个自增「代号 gen」，回主线程时校验，避免过期结果污染新任务。
 ****************************************************************************/
#pragma once

#include "core/Types.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>

class QThreadPool;

namespace cv {

class Backend;

// 单个分块在文件中的位置：从 offset 起读取 length 字节（内部数据结构）
struct ChunkRange
{
    qint64 offset = 0;
    qint64 length = 0;
};

// 上传前的本地扫描结果（线程池算好后按值回传主线程）
struct UploadScanResult
{
    bool    ok = false;
    QString err;
    QString name;
    qint64  size  = 0;
    qint64  mtime = 0; // 毫秒时间戳
    QString hash;      // 整文件 SHA-256（小写十六进制）
};

class TransferManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int     activeCount READ activeCount NOTIFY changed)
    Q_PROPERTY(QString statusText  READ statusText  NOTIFY changed)
    Q_PROPERTY(bool    busy        READ busy        NOTIFY changed)

public:
    explicit TransferManager(Backend *backend, QObject *parent = nullptr);
    ~TransferManager() override;

    // ---- QML 属性（冻结签名）----
    int     activeCount() const { return m_activeCount; }
    QString statusText() const { return m_statusText; }
    bool    busy() const { return m_busy; }

    // ---- 入队（冻结签名）----
    // 上传：本地文件绝对路径列表 → 远端目录（dir 为相对路径，'' = 根目录）
    Q_INVOKABLE void enqueueUpload(const QStringList &localPaths, const QString &dir);
    // 下载：fileId 列表 → 本地保存目录
    Q_INVOKABLE void enqueueDownload(const QStringList &fileIds, const QString &destDir);
    // 取消当前任务（并终止当前批次剩余的排队任务）
    Q_INVOKABLE void cancelCurrent();

signals:
    void changed();
    // 任务开始（含展示所需信息）——上层据此在 TransferModel 中建行；每任务只发一次。
    // fileName：上传=本地文件基名；下载=服务端文件名（非 file id）。total 未知时可为 0。
    void taskStarted(const QString &taskId, const QString &fileName, TransferKind kind,
                     qint64 total);
    void taskProgress(const QString &taskId, qint64 done, qint64 total);
    void taskFinished(const QString &taskId, bool ok, const QString &message);
    // 同名冲突：由 TransferController 弹「是否覆盖」，再回调解续
    void conflictDetected(const QString &taskId, const QString &dir, const QString &name);
    void logMessage(const QString &level, const QString &text);

public slots:
    // 覆盖 = true / 跳过 = false（冻结签名）
    void resolveConflict(const QString &taskId, bool overwrite);

private:
    // 单个传输任务的完整状态（上传字段与下载字段各自独立使用）
    struct Task
    {
        QString       id;
        TransferKind  kind  = TransferKind::Upload;
        TransferState state = TransferState::Queued;
        qint64        gen   = 0; // 任务代号：回主线程时校验，拒绝过期结果

        // ---- 上传 ----
        QString           localPath; // 本地绝对路径
        QString           dir;       // 远端目标目录（'' = 根目录）
        QString           name;      // 文件名
        qint64            mtime     = 0; // 毫秒时间戳（manifest 校验）
        qint64            chunkSize = 0; // 本次使用的分块字节数（唯一来源 backend->chunkSize()）
        QString           fullHash;  // 整文件 SHA-256
        QList<ChunkRange> chunks;    // 分块偏移/长度表
        QString           uploadId;  // 服务端会话 id
        QSet<int>         doneSeq;      // 已确认分块
        QSet<int>         inflightSeq;  // 在途分块
        QSet<int>         retryWait;    // 退避等待中的分块（不参与补位，防重发）
        QHash<int, int>   retries;      // 各分块已重试次数
        int               totalChunks = 0;
        bool              overwrite     = false;
        bool              resumeHit     = false;
        bool              completeSent  = false;

        // ---- 下载 ----
        QString                          fileId;
        QString                          destDir;
        QString                          finalPath;
        QString                          partPath; // <final>.part
        QMap<qint64, QByteArray>         readySegments; // 乱序到达缓存（起点 → 数据）
        QList<QPair<qint64, QByteArray>> writeQueue;    // 按序写盘队列
        int                              inflightSegments = 0;
        qint64                           nextReqOffset    = 0;
        bool                             writing          = false;

        // ---- 通用 ----
        qint64  total = 0;
        qint64  done  = 0;
        bool    cancelRequested  = false;
        bool    awaitingConflict = false;
    };

    // ---- 队列推进 ----
    void    refresh();      // 重算 activeCount / statusText / busy 并择机 emit changed
    void    pump();         // 无在途任务时启动队首
    void    startCurrent(); // 按 kind 分派
    QString newTaskId();

    // ---- 上传（gen 为任务代号，用于丢弃过期回调）----
    void beginUpload();
    void onUploadScanned(qint64 gen, const UploadScanResult &r);
    // overwrite=true 时在同一次 init 内原子完成「置真 → 发起 → 复位」（§8.4）
    void requestBeginUpload(bool overwrite);
    void onBeginUpload(qint64 gen, const Result<UploadTicket> &r);
    void onUploadConflict(const QString &errorText);
    void pumpChunks();
    void launchChunkUpload(int seq);
    void onChunkUploaded(qint64 gen, int seq, const Ok &r);
    void requestComplete();
    void onComplete(qint64 gen, const Result<FileItem> &r);
    void updateUploadProgress();

    // ---- 下载 ----
    void beginDownload();
    void onDownloadStat(qint64 gen, const Result<FileItem> &r);
    void pumpDownload();
    void launchRangeRead(qint64 offset, qint64 length);
    void onRangeData(qint64 gen, qint64 offset, const Result<QByteArray> &r);
    void drainReady();
    void maybeWrite();
    void launchWrite(qint64 offset, const QByteArray &data);
    void onSegmentWritten(qint64 gen, bool ok, const QString &err);
    void checkDownloadDone();
    void finalizeDownload();

    // ---- 收尾与工具 ----
    void finishTask(bool ok, const QString &message);
    // 分块表：块大小由调用方传入（取自 backend->chunkSize()，单一来源，§8.5）
    QList<ChunkRange> makeChunkPlan(qint64 size, qint64 chunkBytes) const;

    // ---- manifest（断点续传）----
    bool loadManifest(const QString &path, qint64 size, qint64 mtime, qint64 chunkBytes,
                      const QString &hash, QString *uploadId, QSet<int> *seqs) const;
    void saveManifest() const;
    void clearManifest(const QString &path) const;

    // ---- 成员（主线程独占，无锁）----
    Backend     *m_backend = nullptr;
    QThreadPool *m_pool    = nullptr;

    QList<Task> m_pending; // 待处理队列（不含当前任务）
    Task        m_cur;     // 当前任务
    bool        m_hasCurrent = false;

    int     m_activeCount = 0;
    QString m_statusText  = QStringLiteral("空闲");
    bool    m_busy        = false;

    int    m_batchDone = 0; // 本批次已完成任务数（statusText 用）
    qint64 m_taskSeq   = 0; // taskId 自增序号
    qint64 m_genSeq    = 0; // 任务代号自增序号

    QString m_manifestFile;
};

} // namespace cv

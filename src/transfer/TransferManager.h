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
    // openWhenDone：下载完成后发 fileReady(本地路径)，供"打开（系统默认程序）"用
    Q_INVOKABLE void enqueueDownload(const QStringList &fileIds, const QString &destDir,
                                     bool openWhenDone = false);
    // 取消当前任务（并终止当前批次剩余的排队任务）
    Q_INVOKABLE void cancelCurrent();

    // 启动时恢复「上次没传完」的上传：读 manifest，把本地文件仍在的条目重新入队，
    // pipeline 会命中同一 manifest 从断点继续。返回恢复的任务数。
    Q_INVOKABLE int restoreInterrupted();

    // 「重新传输」：按 taskId 在完成/失败任务的留存表里找回原参数，重新入队。
    // 上传 → 重新上传同一个本地文件到原云端目录；下载 → 重新下载同一个 fileId 到原目录
    // （.part 还在就自动续传；被"取消"清掉过就从 0 开始）。
    Q_INVOKABLE bool retryTask(const QString &taskId);

    // 暂停 / 继续**当前任务**。因为引擎一次只跑一个任务，暂停它会同时挡住后面的队列
    // （这正是"暂停传输"该有的语义）。已在途的分块/区间会自然收尾，不再派发新的。
    Q_INVOKABLE void pauseCurrent();
    Q_INVOKABLE void resumeCurrent();
    bool            currentPaused() const;

    // 默认下载目录（由 Application 从 Settings 注入）。
    // 回退链：enqueueDownload 的显式 destDir > 本默认目录 > AppPaths::downloadDir()。
    void    setDefaultDownloadDir(const QString &dir);
    QString defaultDownloadDir() const { return m_defaultDownloadDir; }

signals:
    void changed();
    // 任务开始（含展示所需信息）——上层据此在 TransferModel 中建行；每任务只发一次。
    // fileName：上传=本地文件基名；下载=服务端文件名（非 file id）。total 未知时可为 0。
    // localPath：本机落地的绝对路径（下载=写完的位置；上传=源文件）
    // remotePath：云端目标位置（上传=目标路径；下载=空）
    // 这两个是"传输栏里显示文件地址"用的 —— 任务行里本来就有这两个字段，
    // 只是没随信号传出来，所以界面拿不到。
    void taskStarted(const QString &taskId, const QString &fileName, TransferKind kind,
                     const QString &localPath, const QString &remotePath,
                     qint64 total);
    void taskProgress(const QString &taskId, qint64 done, qint64 total);
    void taskFinished(const QString &taskId, bool ok, const QString &message);
    // 暂停态变化（上层据此把行状态切成「已暂停 / 传输中」，并更新按钮文案）
    void taskPaused(const QString &taskId, bool paused);
    // 上传**成功合并**后的远端条目（含 rev/content_hash）。
    // 同步引擎要拿它写索引：rev 是云端权威信号，缺了会写出过期索引 ⇒ 下轮误判"云端变了"。
    void taskCompleted(const QString &taskId, const FileItem &item);
    // 取消任务后磁盘占用会变（服务端删掉会话的临时分块），且这一刻**晚于** taskFinished，
    // 所以单独发一个信号，让上层在"服务端清理完成"之后再刷用量（否则数字是旧的）。
    void storageChanged();
    // 任务被**取消**：语义是"放弃"，上层应把该行从队列里移除（与"失败"不同，失败要留痕）。
    void taskCancelled(const QString &taskId);
    // 下载完成且该任务要求"下完就打开" → 携带本地绝对路径（上层用系统默认程序打开）
    void fileReady(const QString &path);
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
        bool              paused        = false;   // 用户暂停：泵循环不再派发新分块/区间
        bool              completeSent  = false;

        // ---- 下载 ----
        QString                          fileId;
        QString                          destDir;
        QString                          finalPath;
        QString                          partPath; // <final>.part
        QMap<qint64, QByteArray>         readySegments; // 乱序到达缓存（起点 → 数据）
        QList<QPair<qint64, QByteArray>> writeQueue;    // 按序写盘队列
        int                              inflightSegments = 0;
        QHash<qint64, int>               segRetries;   // 各分段已重试次数（下载）
        qint64                           nextReqOffset    = 0;
        bool                             writing          = false;
        bool                             openWhenDone     = false; // 下载完成后自动用系统程序打开

        // ---- 通用 ----
        qint64  total = 0;
        qint64  done  = 0;
        bool    cancelRequested  = false;
        bool    awaitingConflict = false;
    };

    // 按 gen 找到在途任务（找不到 = 过期回调，调用方应直接 return）
    Task *taskForGen(qint64 gen) const;
    // 把 m_active 里的任务移到 m_graveyard（延迟回收，避免回调栈内 use-after-free）
    void   retireTask(Task *t);
    void   reapGraveyard();

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
    // 下载分段失败/长度不符时重试该段（网络抖动、服务端重启都不该让整个任务失败）
    void retrySegment(qint64 gen, qint64 offset, qint64 length, const QString &why);
    void drainReady();
    void maybeWrite();
    void launchWrite(qint64 offset, const QByteArray &data);
    void onSegmentWritten(qint64 gen, bool ok, const QString &err);
    void checkDownloadDone();
    void finalizeDownload();

    // ---- 收尾与工具 ----
    void finishTask(bool ok, const QString &message);
    // 下载目标目录回退链：显式 destDir > 注入的默认目录 > AppPaths::downloadDir()
    QString resolveDownloadDir(const QString &destDir) const;
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

    QList<Task> m_pending; // 待处理队列（不含在途任务）

    // ── 文件级并发（2026-10-18）──
    // 原来只有一个 m_cur 任务在跑；现在最多 kFileConcurrency 个**同时**跑。
    // 任务在堆上分配 ⇒ 指针稳定（QHash 扩容/rehash 不会让 m_cur 悬空）。
    // 未完成的任务留在 m_active（key = gen，全局唯一，回调靠它定位任务）。
    QHash<qint64, Task *> m_active;
    // 正在被某条回调链处理的任务。**保留这个名字是为了让流水线函数体一字不改** ——
    // 那些函数里的 m_cur.xxx 已机械替换成 m_cur->xxx。
    // 约定：只在"处理某任务的回调"内有效；pump() 会 save/restore 它。
    Task *m_cur = nullptr;
    // finishTask 后不能立刻 delete（调用栈里可能还有人用 m_cur），
    // 先挪进待回收列表，下次 pump() 开头统一回收。
    QList<Task *> m_graveyard;
    // 还在等服务端 cancelUpload 落地的任务数；减到 0 才发 storageChanged（只发一次）
    int m_cancelPending = 0;
    // 未完成下载任务的持久化（重启客户端后回到传输列表）；
    // 每次入队/结束/取消都整体重写 = 天然自清理，不需要单独删条目。
    // 「重新传输」用的参数快照。
    // ⚠️ 不能用 m_graveyard：pump() 每次开头 reapGraveyard() 会把已结束的 Task 对象删掉，
    //    于是用户点 ↻ 时参数早没了（表现为"点了没反应"）。这里存纯数据，不受回收影响。
    struct FinishedParams {
        QString      id;
        TransferKind kind = TransferKind::Upload;
        QString      name;
        QString      fileId;     // 下载用
        QString      localPath;  // 上传源文件
        QString      dir;        // 上传目标目录
        QString      destDir;    // 下载目标目录
    };
    QList<FinishedParams> m_finished;
    // 同上那份快照的落盘版本：**重启后历史行上的 ↻ 也能用**（否则重启就丢参数）
    QString m_finishedFile;
    void saveFinishedParams() const;
    void loadFinishedParams();
    QString m_dlTasksFile;
    void saveDownloadTasks() const;

    int     m_activeCount = 0;
    QString m_statusText  = QStringLiteral("空闲");
    bool    m_busy        = false;

    int    m_batchDone = 0; // 本批次已完成任务数（statusText 用）
    qint64 m_taskSeq   = 0; // taskId 自增序号
    qint64 m_genSeq    = 0; // 任务代号自增序号

    QString m_manifestFile;

    // 由 Application 注入的默认下载目录（为空时回退 AppPaths::downloadDir()）
    QString m_defaultDownloadDir;
};

} // namespace cv

/****************************************************************************
 * sync/SyncEngine.h —— 同步引擎：调度 + 执行 + 推进索引 + 写日志（FR-3-1/3-8/3-9）
 *
 * ── 职责边界 ──
 *   SyncPlan 只做"该干什么"的纯决策；本类负责"真的去干"：扫本地、拉云端、执行动作、
 *   在**每个动作真正成功后**推进 LocalIndex、并把每一步写进同步日志。
 *
 * ── 调度（FR-3-1）──
 *   · 启动时：对每个启用的同步对做一次**全量对账**
 *   · 之后：FileWatcher 防抖满 5 秒 → 对账一次（事件驱动）
 *   · 兜底：每 scanIntervalMin 分钟（默认 10）**全量对账**一次，防止漏事件（网络抖动、
 *     监听句柄耗尽、文件被别的进程改名等都可能漏）
 *
 * ── 线程模型（重要）──
 *   本地扫描 + SHA-256 计算放 **QThreadPool**（纯文件 IO，慢且不碰网络）；
 *   所有 Backend 调用留在**主线程**（HttpBackend 的 QNetworkAccessManager 归属主线程，
 *   跨线程用它是未定义行为）。执行阶段**每个动作只占一个事件循环轮次**
 *   （QTimer::singleShot(0) 驱动），界面能在动作之间重绘。
 *
 *   ⚠️ 已知限制（诚实标注）：`Backend` 只有 listFolder/usage/getRange/分享 的异步变体，
 *      **上传/下载/删除没有异步实现** ⇒ 单个动作执行期间仍会阻塞主线程。
 *      小文件/本地引擎无感；大文件上传下载会看到界面卡顿。要彻底解决需给 Backend
 *      补 uploadAsync/downloadAsync（与 projects 既有的"异步变体加法式扩展"同一套路）。
 *
 * ── 绝不静默丢数据 ──
 *   · 云端删除用 moveToTrash（进云端回收站，可恢复）
 *   · 本地删除**不直接抹掉**：先搬进 <AppData>/sync-trash/<pairId>/<时间戳>/ 保留，
 *     失败时逐个回退为"跳过并报错"，绝不在搬移失败时硬删
 *   · 冲突按 SyncPlan 的语义保留两份
 ****************************************************************************/
#pragma once

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QVector>

#include "core/Types.h"
#include "sync/SyncPlan.h"

class QTimer;

namespace cv {

class TransferManager;   // 同步的上传动作可交给它执行（文件级并发）

class Backend;
class FileWatcher;
class LocalIndex;

class SyncEngine : public QObject
{
    Q_OBJECT
public:
    // 注入传输管理器：注入后，**上传动作**走 TransferManager（4 文件并发 + 3 块并发），
    // 引擎不再自己 hash/init/putChunk/finish（那条路会阻塞主线程且完全串行）。
    // 传 nullptr 则退回原来的内联实现（harness / 旧路径可对照）。
    void setTransferManager(TransferManager *tm);
    SyncEngine(Backend *backend, LocalIndex *index, QObject *parent = nullptr);
    ~SyncEngine() override;

    // 由控制器注入当前同步对集合（启用与否由 pair.enabled 决定）
    void setPairs(const QVector<SyncPair> &pairs);
    void setDeviceName(const QString &name);
    void setScanIntervalMin(int minutes);   // 兜底对账周期，默认 10
    void setDebounceMs(int ms);             // 传给 FileWatcher，默认 5000

    void start();                            // 启动全量对账 + 起兜底定时器
    void stop();
    bool isRunning() const { return m_running; }

    // 手动触发一次对账：pairId 为空 → 全部启用对
    void reconcileNow(const QString &pairId = QString());

    // 动作 → 中文可读（同步日志用，FR-3-9）
    static QString actionText(SyncAction::Kind kind);
    // 对账结果摘要（日志/状态条用）
    static QString countsText(const SyncCounts &c);
    // 本地删除的"保险箱"根目录
    static QString localTrashRoot();

signals:
    // status 用 int 传（SyncStatus），避免 QML 侧枚举注册依赖
    void pairStatus(const QString &pairId, int status, const QString &message);
    void pairSettled(const SyncPair &after);              // 一次对账结束（含 lastSync/pending/conflict）
    void pairProgress(const QString &pairId, const QString &detail, int done, int total);
    void logAppended(const QString &line);
    void allFinished();

private:
    void enqueue(const QString &pairId, const QString &reason);
    void startNext();
    void scanLocalPhase();
    void fetchRemotePhase();
    void planPhase();
    void executePhase();
    void finishCurrent(const QString &error);

    // 把云端 remoteRel 下载到本地 localRel（先写 .tmp 再改名，避免留下半截文件）
    bool downloadTo(const QString &remoteRel, const QString &localRel, QString &err);
    // 两端确认一致后，按**观察到的**状态推进索引（rev 以云端为准）
    void recordFromObserved(const QString &relPath, const SyncSnapshot::Item &local,
                            const SyncSnapshot::Item &remote);

    SyncPair *pairById(const QString &pairId);

    Backend          *m_backend = nullptr;
    LocalIndex       *m_index = nullptr;
    FileWatcher      *m_watcher = nullptr;
    QTimer           *m_fallback = nullptr;

    QVector<SyncPair> m_pairs;
    QString           m_deviceName;
    int               m_scanIntervalMin = 10;
    bool              m_running = false;
    bool              m_busy = false;

    // 当前正在对账的上下文
    struct Run
    {
        QString      pairId;
        QString      reason;
        SyncPair     pair;
        SyncSnapshot local;
        SyncSnapshot remote;
        SyncSnapshot lastSynced;
        SyncPlanResult plan;
        int          actionIndex = 0;
        // 本轮内「云端目录路径 → 目录 id」缓存。
        // ⚠️ 原来**每个文件**上传前都调一次 ensureFolderPath（一次 HTTP 往返），
        //    1482 个文件里绝大多数还在同几个目录下 ⇒ 白跑 1400+ 次请求。
        //    这就是"同步很慢"里最容易被忽略的一大块（比并发度还狠）。
        QHash<QString, QString> remoteDirIds;
        QVector<QString> log;
        bool         failed = false;
        QString      error;
    };
    // ---- 批量上传（仅在注入 TransferManager 时启用）----
    // 引擎把「连续的、同一云端目录」的上传动作一次性 enqueue 给 TransferManager，
    // 然后**不阻塞**：每个任务结束时由回调推进计数，整批完成后再推进 actionIndex。
    TransferManager *m_tm = nullptr;
    bool     m_upBatchActive    = false;   // 批次在飞：executePhase 直接返回，不重复推进
    bool     m_upCollecting     = false;   // 正在 enqueueUpload 里（用于认领 taskStarted 的 id）
    int      m_upBatchLastIndex = 0;       // 本批最后一个动作的下标（收尾时 actionIndex = +1）
    int      m_upBatchRemaining = 0;       // 还没结束的任务数
    QHash<QString, QString>  m_upByAbs;    // 绝对路径 → relPath（enqueue 期间认领 taskId）
    QHash<QString, QString>  m_upTaskRel;  // taskId → relPath
    QHash<QString, FileItem> m_upItems;    // taskId → 完成后返回的远端条目（取 rev/hash）

    bool startUploadBatch();
    void onTransferStarted(const QString &taskId, const QString &fileName, TransferKind kind,
                           const QString &localPath, const QString &remotePath, qint64 total);
    void onTransferCompleted(const QString &taskId, const FileItem &item);
    void onTransferFinished(const QString &taskId, bool ok, const QString &message);

    Run  m_run;
    QQueue<QString> m_queue;   // 待对账的 pairId
};

} // namespace cv

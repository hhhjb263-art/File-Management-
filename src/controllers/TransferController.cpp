/****************************************************************************
 * controllers/TransferController.cpp —— 传输队列控制器实现
 ****************************************************************************/
#include "TransferController.h"

#include "core/Settings.h"
#include "data/TransferModel.h" // cv::TransferModel / cv::TransferTask（契约 §4）

#include <QDesktopServices>
#include <QDir>
#include <QStandardPaths>
#include <QUrl>

// 并行防守：TransferManager.h 未落地时仍可独立编译（属性回默认值）。
// 头文件一到，下面 include 生效，本控制器自动完整接线。
#if __has_include("transfer/TransferManager.h")
#  include "transfer/TransferManager.h"
#  define CV_HAVE_TRANSFERMANAGER 1
#endif

namespace cv {
namespace {

// 与 FileController / AppController 保持一致的错误映射：
// 传输路径的失败（TransferManager 直接透出的 error）也走这里，
// 使 401 在「文件列表」与「传输」两条路径上文案一致（均可引导去设置令牌）。
QString friendlyError(const QString &raw)
{
    if (raw.isEmpty())
        return QStringLiteral("传输失败（无错误详情）");

    if (raw.startsWith(QStringLiteral("[pin-mismatch]")))
        return QStringLiteral("服务器证书指纹与已固定记录不一致，可能存在中间人攻击；"
                              "本次连接已被拒绝。若确认是服务器换证，请在【设置】页清除"
                              "该主机信任后重试。");
    if (raw.startsWith(QStringLiteral("[unsupported]")))
        return QStringLiteral("服务端暂不支持此功能");

    if (raw.contains(QStringLiteral("HTTP 401")))
        return QStringLiteral("令牌无效，请检查访问令牌");
    if (raw.contains(QStringLiteral("HTTP 403")))
        return QStringLiteral("服务器拒绝访问（403）");
    if (raw.contains(QStringLiteral("HTTP 404")))
        return QStringLiteral("目标不存在（404）");
    if (raw.startsWith(QStringLiteral("[conflict]")) || raw.contains(QStringLiteral("HTTP 409")))
        return QStringLiteral("该目录下已存在同名文件，请选择覆盖或跳过");
    if (raw.contains(QStringLiteral("HTTP 507")))
        return QStringLiteral("服务器空间不足（507）");
    if (raw.contains(QStringLiteral("HTTP 413")))
        return QStringLiteral("文件超出服务器单次上限（413）");
    if (raw.contains(QStringLiteral("HTTP 422")))
        return QStringLiteral("文件内容校验失败（哈希不符）");

    return raw; // 超时 / 网络错误 / 服务端原文等原样透出
}

} // namespace

TransferController::TransferController(TransferManager *manager, QObject *parent)
    : QObject(parent)
    , m_manager(manager)
{
#ifdef CV_HAVE_TRANSFERMANAGER
    if (m_manager) {
        // manager 自身用 changed() 汇总通知；这里转成三个细粒度属性信号
        connect(m_manager, &TransferManager::changed, this, &TransferController::pullState);

        // 任务开始：在模型里建行（fileName/kind/total 来自信号；state=排队中）
        connect(m_manager, &TransferManager::taskStarted, this,
                [this](const QString &taskId, const QString &fileName, TransferKind kind,
                       qint64 total) {
                    if (m_taskModel) {
                        TransferTask t;
                        t.id       = taskId;
                        t.fileName = fileName;
                        t.kind     = kind;
                        t.total    = total;
                        t.state    = TransferState::Queued;
                        m_taskModel->addTask(t); // 不存在则插入，已存在退化为更新（去重）
                    }
                    m_runningRows.remove(taskId);
                });

        // 进度：只更新 done/total/progress/progressRatio（TransferModel 内部用
        // dataChanged 指定角色，不整表刷新）；首次进度把「排队中」提升为「传输中」。
        connect(m_manager, &TransferManager::taskProgress, this,
                [this](const QString &taskId, qint64 done, qint64 total) {
                    if (m_taskModel) {
                        m_taskModel->updateProgress(taskId, done, total);
                        if (!m_runningRows.contains(taskId)) {
                            m_runningRows.insert(taskId);
                            m_taskModel->updateState(taskId, TransferState::Running, QString());
                        }
                    }
                    emit taskProgress(taskId, done, total);
                });

        // 结束：置 完成/失败 + message（失败文案走 friendlyError，401 → 令牌无效）
        connect(m_manager, &TransferManager::taskFinished, this,
                [this](const QString &taskId, bool ok, const QString &message) {
                    const QString shown = ok ? message : friendlyError(message);
                    if (m_taskModel) {
                        m_taskModel->updateState(
                            taskId, ok ? TransferState::Completed : TransferState::Failed,
                            ok ? QString() : shown);
                    }
                    m_runningRows.remove(taskId);
                    emit taskFinished(taskId, ok, shown);
                    pullState(); // activeCount / busy 可能变化
                });

        // 冲突：置「已暂停」+ 待用户决定提示
        connect(m_manager, &TransferManager::conflictDetected, this,
                [this](const QString &taskId, const QString &dir, const QString &name) {
                    if (m_taskModel) {
                        m_taskModel->updateState(taskId, TransferState::Paused,
                                                 QStringLiteral("同名文件已存在，请选择覆盖或跳过"));
                    }
                    m_runningRows.remove(taskId);
                    emit conflictDetected(taskId, dir, name);
                });

        connect(m_manager, &TransferManager::logMessage, this, &TransferController::logMessage);
    }
#endif

    if (!m_manager)
        m_statusText = QStringLiteral("传输组件未就绪");
    pullState();
}

void TransferController::setTaskModel(TransferModel *model)
{
    // 允许 nullptr（解绑）；可重复调用，后者生效（不迁移旧模型数据）。
    m_taskModel = model;
    if (!m_taskModel)
        m_runningRows.clear();
}

void TransferController::pullState()
{
    int     ac = 0;
    QString st;
    bool    bz = false;

#ifdef CV_HAVE_TRANSFERMANAGER
    if (m_manager) {
        ac = m_manager->activeCount();
        st = m_manager->statusText();
        bz = m_manager->busy();
    }
#endif

    const bool acChanged = (ac != m_activeCount);
    const bool stChanged = (st != m_statusText);
    const bool bzChanged = (bz != m_busy);

    m_activeCount = ac;
    m_statusText  = st;
    m_busy        = bz;

    if (acChanged)
        emit activeCountChanged();
    if (stChanged)
        emit statusTextChanged();
    if (bzChanged)
        emit busyChanged();
}

void TransferController::upload(const QStringList &paths, const QString &dir)
{
    if (paths.isEmpty()) {
        emit errorOccurred(QStringLiteral("未选择要上传的文件"));
        return;
    }
#ifdef CV_HAVE_TRANSFERMANAGER
    if (m_manager) {
        m_manager->enqueueUpload(paths, dir);
        pullState();
        return;
    }
#endif
    emit errorOccurred(QStringLiteral("传输组件未就绪，无法上传"));
}

void TransferController::download(const QStringList &ids, const QString &dest)
{
    if (ids.isEmpty()) {
        emit errorOccurred(QStringLiteral("未选择要下载的文件"));
        return;
    }
#ifdef CV_HAVE_TRANSFERMANAGER
    if (m_manager) {
        m_manager->enqueueDownload(ids, dest);
        pullState();
        return;
    }
#endif
    emit errorOccurred(QStringLiteral("传输组件未就绪，无法下载"));
}

void TransferController::cancel()
{
#ifdef CV_HAVE_TRANSFERMANAGER
    if (m_manager) {
        m_manager->cancelCurrent();
        pullState();
    }
#endif
}

void TransferController::resolveConflict(const QString &id, bool overwrite)
{
#ifdef CV_HAVE_TRANSFERMANAGER
    if (m_manager) {
        // 覆盖语义（setUploadOverwrite）由 TransferManager 按契约 §8.2 内部处理
        m_manager->resolveConflict(id, overwrite);
        pullState();
    }
#else
    Q_UNUSED(id);
    Q_UNUSED(overwrite);
#endif
}

void TransferController::openLocalFolder()
{
    QString dir = Settings::instance().downloadDir();
    if (dir.isEmpty() || !QDir(dir).exists()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (!dir.isEmpty() && !QDir(dir).exists())
            QDir().mkpath(dir); // 系统下载目录不存在则创建
    }

    if (dir.isEmpty()) {
        emit errorOccurred(QStringLiteral("未找到可打开的本地目录"));
        return;
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    emit logMessage(QStringLiteral("INFO"),
                    QStringLiteral("打开本地目录：%1").arg(dir));
}

} // namespace cv

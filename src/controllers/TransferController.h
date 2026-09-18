/****************************************************************************
 * controllers/TransferController.h —— 传输队列控制器
 *
 * 契约：docs/整合实现契约.md §3（冻结 API）
 *   · 属性：activeCount、statusText、busy
 *   · 方法：upload(paths,dir)、download(ids,dest)、cancel()、
 *           resolveConflict(id,overwrite)、openLocalFolder()
 *   包装 transfer/TransferManager（契约 §2 冻结 API）。
 *
 * 本控制器是 TransferManager 的薄包装：
 *   · 属性直接镜像 manager 的 activeCount / statusText / busy；
 *   · 冲突（同名 409）由 manager 发 conflictDetected，控制器转成信号交 QML 弹框，
 *     用户选择后 QML 调 resolveConflict(id, overwrite) 回投（覆盖的
 *     setUploadOverwrite 语义由 TransferManager 按契约 §8.2 内部处理）；
 *   · 队列进度镜像到 data/TransferModel（契约 §4 角色名），供传输队列页渲染进度条。
 *
 * 并行注记：TransferManager.h 若尚未落地，本文件用 __has_include 做编译期防守——
 * 头文件到达即自动接线；未到时属性回默认值、方法只给出「未就绪」提示，不崩。
 ****************************************************************************/
#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace cv {

class TransferManager;
class TransferModel;

class TransferController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int     activeCount READ activeCount NOTIFY activeCountChanged)
    Q_PROPERTY(QString statusText  READ statusText  NOTIFY statusTextChanged)
    Q_PROPERTY(bool    busy        READ busy        NOTIFY busyChanged)
    Q_PROPERTY(bool    supported   READ supported   CONSTANT)

public:
    explicit TransferController(TransferManager *manager, QObject *parent = nullptr);

    int     activeCount() const { return m_activeCount; }
    QString statusText() const { return m_statusText; }
    bool    busy() const { return m_busy; }
    bool    supported() const { return m_manager != nullptr; }

    // ---- 契约 §3 冻结方法 ----
    Q_INVOKABLE void upload(const QStringList &paths, const QString &dir);
    Q_INVOKABLE void download(const QStringList &ids, const QString &dest);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void resolveConflict(const QString &id, bool overwrite);
    Q_INVOKABLE void openLocalFolder(); // 打开本地下载目录（系统文件管理器）

    // ---- 加法式（契约 §3 之外，不改任何冻结签名）----
    // 绑定 QML 数据模型以镜像队列进度（data/TransferModel，契约 §4 角色名）。
    // 语义：允许传 nullptr（解绑，此后不再写模型）；可重复调用，后者生效
    //       （不迁移旧数据）。应由装配方在应用启动、开始传输前调用一次。
    void setTaskModel(TransferModel *model);

signals:
    void activeCountChanged();
    void statusTextChanged();
    void busyChanged();

    // 透传 TransferManager 的信号（供 QML 队列页 / 冲突弹框）
    void taskProgress(const QString &taskId, qint64 done, qint64 total);
    void taskFinished(const QString &taskId, bool ok, const QString &message);
    void conflictDetected(const QString &taskId, const QString &dir, const QString &name);
    void logMessage(const QString &level, const QString &text);

    void errorOccurred(const QString &message);

private:
    void pullState(); // 从 manager 拉取三个属性并在变化时发信号

    TransferManager *m_manager = nullptr;
    TransferModel   *m_taskModel = nullptr; // 可为空：未绑定则不写模型
    // 已从「排队中」提升为「传输中」的 taskId（首次进度到达时提升，完成/冲突时移除）
    QSet<QString> m_runningRows;

    int     m_activeCount = 0;
    QString m_statusText;
    bool    m_busy = false;
};

} // namespace cv

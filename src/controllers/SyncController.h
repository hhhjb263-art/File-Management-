/****************************************************************************
 * controllers/SyncController.h —— 同步控制器（真实实现）
 *
 * 契约（docs/整合实现契约.md §3 冻结 API 的加法式升级）：
 *   · supported 由 false 改为 true；unsupportedNotice 保留（恒空），
 *     以便既有 QML 的置灰兜底代码零改动继续编译
 *   · 所有同步对 CRUD 都在这里做校验并落盘（Settings::syncPairs），再驱动 SyncEngine；
 *     模型只负责展示
 *
 * 为什么校验放在控制器：本地目录必须真实存在，否则"同步对已建立"是假象 ——
 * FileWatcher 会静默跳过不存在的目录，用户会以为在同步而其实什么都没发生。
 ****************************************************************************/
#pragma once

#include <QObject>
#include <QVariantList>
#include <QVector>

#include "core/Types.h"

namespace cv {

class TransferManager;   // setTransferManager 只用到指针，前向声明足够
class Backend;
class LocalIndex;
class SyncEngine;
class SyncPairModel;

class SyncController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool    supported         READ supported         CONSTANT)
    Q_PROPERTY(QString unsupportedNotice READ unsupportedNotice CONSTANT)
    Q_PROPERTY(bool    running           READ running           NOTIFY runningChanged)
    Q_PROPERTY(bool    autoSync          READ autoSync          WRITE setAutoSync          NOTIFY autoSyncChanged)
    Q_PROPERTY(int     scanIntervalMin   READ scanIntervalMin   WRITE setScanIntervalMin   NOTIFY scanIntervalChanged)
    Q_PROPERTY(bool    hasPairs          READ hasPairs          NOTIFY pairsChanged)
    Q_PROPERTY(int     pairCount         READ pairCount         NOTIFY pairsChanged)
    Q_PROPERTY(QVariantList logEntries   READ logEntries        NOTIFY logChanged)
    Q_PROPERTY(QString deviceName        READ deviceName        CONSTANT)
    Q_PROPERTY(QString emptyText         READ emptyText         CONSTANT)

public:
    SyncController(Backend *backend, LocalIndex *index, SyncPairModel *model,
                   QObject *parent = nullptr);
    ~SyncController() override;

    bool    supported() const { return true; }
    QString unsupportedNotice() const { return QString(); }

    bool running() const;
    bool autoSync() const;
    void setAutoSync(bool on);
    int  scanIntervalMin() const;
    void setScanIntervalMin(int minutes);

    bool hasPairs() const;
    int  pairCount() const;
    QVariantList logEntries() const;
    QString deviceName() const;
    QString emptyText() const;

    // ---- 同步对 CRUD：返回空字符串表示成功，否则是给用户看的中文原因 ----
    Q_INVOKABLE QString addPair(const QString &localPath, const QString &remotePath, int mode);
    Q_INVOKABLE void    removePair(const QString &pairId);
    Q_INVOKABLE void    setPairEnabled(const QString &pairId, bool on);
    Q_INVOKABLE void    setPairMode(const QString &pairId, int mode);
    // FR-3-7 选择性同步：text 为**每行一个**云端目录（相对该对的 remotePath）。
    // 传 QString 而不是 QStringList：QML 侧是一个 TextArea，换行分隔最自然，
    // 也避免 JS 数组到 QStringList 的隐式转换歧义。
    Q_INVOKABLE void    setPairExcludes(const QString &pairId, const QString &text);

    // 把 TransferManager 透传给引擎：注入后同步的上传动作走它的文件级并发
    void setTransferManager(TransferManager *tm);

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void syncNow(const QString &pairId);
    Q_INVOKABLE void clearLog();

    Q_INVOKABLE QString statusText(int status) const;
    Q_INVOKABLE QString modeText(int mode) const;
    // 本地删除的"保险箱"位置（界面上要告诉用户去哪儿找回，不能只默默删）
    Q_INVOKABLE QString localTrashLocation() const;

signals:
    void runningChanged();
    void autoSyncChanged();
    void scanIntervalChanged();
    void pairsChanged();
    void logChanged();
    void statusMessage(const QString &message, bool ok);

private:
    void loadPairs();
    void persistPairs();
    void pushToEngine();
    void appendLog(const QString &line);
    SyncPair *findPair(const QString &pairId);

    Backend       *m_backend = nullptr;
    LocalIndex    *m_index = nullptr;
    SyncPairModel *m_model = nullptr;
    SyncEngine    *m_engine = nullptr;

    QVector<SyncPair> m_pairs;
    QVariantList      m_log;          // 最新的在前
    QString           m_deviceName;
};

} // namespace cv

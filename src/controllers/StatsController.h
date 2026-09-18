/****************************************************************************
 * controllers/StatsController.h —— 存储统计控制器
 *
 * 契约：docs/整合实现契约.md §3（冻结 API）
 *   · 属性：freeBytes、totalBytes、freeText
 *   · 方法：refresh()  —— 对接 GET /api/v1/storage
 *
 * 说明：容量数据来自 Backend::usage()（HttpBackend 走 GET /api/v1/storage，
 * MockBackend 走本地引擎）。控制器只在主线程调用后端并读值，不引入锁。
 ****************************************************************************/
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

namespace cv {

class Backend;

class StatsController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qint64  freeBytes  READ freeBytes  NOTIFY statsChanged)
    Q_PROPERTY(qint64  totalBytes READ totalBytes NOTIFY statsChanged)
    Q_PROPERTY(QString freeText   READ freeText   NOTIFY statsChanged)
    Q_PROPERTY(bool    supported  READ supported  NOTIFY statsChanged)
    // 本机磁盘（加法式）：纯本机 QStorageInfo 查询，无网络、不阻塞主线程。
    Q_PROPERTY(QVariantList localDisks READ localDisks NOTIFY localDisksChanged)

public:
    explicit StatsController(Backend *backend, QObject *parent = nullptr);

    qint64  freeBytes() const { return m_freeBytes; }
    qint64  totalBytes() const { return m_totalBytes; }
    QString freeText() const { return m_freeText; }
    bool    supported() const { return m_supported; }
    QVariantList localDisks() const { return m_localDisks; }

    // 查询服务器剩余空间并刷新属性（失败时 freeBytes=-1 且 freeText 给出原因）
    Q_INVOKABLE void refresh();

    // 刷新本机磁盘列表。每项为 QVariantMap，键名冻结：
    //   name(如 "C:")  label  totalText  freeText  usedText  usedRatio(0..1)  isDefault
    // （totalText/freeText/usedText 用 Util::humanSize；isDefault = 下载目录所在卷）
    Q_INVOKABLE void refreshLocalDisks();

signals:
    void statsChanged();
    void localDisksChanged();
    void logMessage(const QString &level, const QString &text);

private:
    Backend *m_backend = nullptr;
    qint64   m_freeBytes = -1;   // -1 表示不可用 / 尚未获取
    qint64   m_totalBytes = 0;
    QString  m_freeText;
    bool     m_supported = true;
    int      m_usageSeq = 0;     // 查询序号：丢弃乱序回包（连点刷新时只认最新一次）
    QVariantList m_localDisks;   // 本机磁盘条目（纯本机查询，见 refreshLocalDisks）
};

} // namespace cv

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

namespace cv {

class Backend;

class StatsController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qint64  freeBytes  READ freeBytes  NOTIFY statsChanged)
    Q_PROPERTY(qint64  totalBytes READ totalBytes NOTIFY statsChanged)
    Q_PROPERTY(QString freeText   READ freeText   NOTIFY statsChanged)
    Q_PROPERTY(bool    supported  READ supported  NOTIFY statsChanged)

public:
    explicit StatsController(Backend *backend, QObject *parent = nullptr);

    qint64  freeBytes() const { return m_freeBytes; }
    qint64  totalBytes() const { return m_totalBytes; }
    QString freeText() const { return m_freeText; }
    bool    supported() const { return m_supported; }

    // 查询服务器剩余空间并刷新属性（失败时 freeBytes=-1 且 freeText 给出原因）
    Q_INVOKABLE void refresh();

signals:
    void statsChanged();
    void logMessage(const QString &level, const QString &text);

private:
    Backend *m_backend = nullptr;
    qint64   m_freeBytes = -1;   // -1 表示不可用 / 尚未获取
    qint64   m_totalBytes = 0;
    QString  m_freeText;
    bool     m_supported = true;
    int      m_usageSeq = 0;     // 查询序号：丢弃乱序回包（连点刷新时只认最新一次）
};

} // namespace cv

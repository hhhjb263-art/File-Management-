/****************************************************************************
 * controllers/ShareController.h —— 分享链接控制器
 *
 * 契约：docs/整合实现契约.md §3（冻结 API）。服务端已实现分享（POST/GET
 * /api/v1/shares、DELETE /api/v1/shares/:id），本控制器对接之。
 *   · 属性：supported(=true)、unsupportedNotice(保留, 空串)、count、busy
 *   · 方法：refresh()、create(fileId,code,expireDays,maxDownloads)、
 *           revoke(shareId)、copyLink(url)、at(i)
 *   · 信号：sharesChanged / busyChanged / supportedChanged /
 *           created(url,code) / errorOccurred / statusMessage / logMessage
 *
 * 线程模型：全部走 Backend 的**异步变体**（HttpBackend 真异步），绝不阻塞主线程。
 * 用 m_seq 序号丢弃乱序回包。
 ****************************************************************************/
#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include "core/Types.h" // ShareLink

namespace cv {

class Backend;

class ShareController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool    supported         READ supported         NOTIFY supportedChanged)
    Q_PROPERTY(QString unsupportedNotice READ unsupportedNotice CONSTANT)
    Q_PROPERTY(int     count             READ count             NOTIFY sharesChanged)
    Q_PROPERTY(bool    busy              READ busy              NOTIFY busyChanged)
    // 两栏分桶计数（refresh 时按 state 计算）：active vs 其余三种（revoked/expired/exhausted）
    Q_PROPERTY(int     activeCount       READ activeCount       NOTIFY sharesChanged)
    Q_PROPERTY(int     invalidCount      READ invalidCount      NOTIFY sharesChanged)

public:
    explicit ShareController(Backend *backend = nullptr, QObject *parent = nullptr);

    bool    supported() const;        // 服务端已实现 → true
    QString unsupportedNotice() const { return QString(); } // 保留字段，恒空串
    int     count() const { return m_shares.size(); }
    bool    busy() const { return m_busy; }
    int     activeCount() const;      // 进行中（state == "active"）条数
    int     invalidCount() const;     // 已失效（其余三种状态）条数

    Q_INVOKABLE void refresh(); // 拉取分享列表
    Q_INVOKABLE void create(const QString &fileId, const QString &code, int expireDays,
                            int maxDownloads); // 创建分享
    Q_INVOKABLE void revoke(const QString &shareId); // 撤销分享
    Q_INVOKABLE void copyLink(const QString &url);   // 复制链接到剪贴板
    Q_INVOKABLE void cleanupInvalid();               // 清除当前调用者名下全部无效分享（不可恢复）
    Q_INVOKABLE QVariantMap at(int i) const;         // 第 i 条（越界返回空 map）

signals:
    void sharesChanged();
    void busyChanged();
    void supportedChanged();
    void created(const QString &url, const QString &code); // 成功创建：链接 + 明文提取码（仅此一次）
    void errorOccurred(const QString &message);
    void statusMessage(const QString &message, bool ok);
    void logMessage(const QString &level, const QString &text);

private:
    void        setBusy(bool on);
    static QString friendlyError(const QString &raw); // 后端失败 → 中文可读

    Backend           *m_backend = nullptr;
    QVector<ShareLink> m_shares;
    bool               m_busy = false;
    int                m_seq  = 0; // 序号：丢弃乱序回包
};

} // namespace cv

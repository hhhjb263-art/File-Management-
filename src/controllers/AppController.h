/****************************************************************************
 * controllers/AppController.h —— 应用 / 连接控制器（QML 唯一的顶层入口）
 *
 * 契约：docs/整合实现契约.md §3（冻结 API）
 *   · 属性：serverUrl、accessToken、statusText、trustSelfSigned、unsupportedNotice
 *   · 方法：healthCheck()、refreshStorage()、saveSettings()
 *
 * 另按契约 §8.3 追加 TOFU（自签名证书信任）注入能力（不改任何冻结签名）：
 *   · 信号 trustPromptRequested(hostPort, fingerprint, subject, issuer, validity)
 *   · 方法 answerTrustPrompt(bool)          —— QML 证书确认框回调
 *   · 方法 pinnedFingerprint() / clearPinnedFingerprint() —— 设置页「查看 / 清除指纹」
 *   未接线（无接收者）或超时 → fail-closed（拒绝），绝不静默信任。
 *
 * 线程模型：后端为「同步阻塞」接口（HttpBackend 内部用 QEventLoop），
 * 本控制器在主线程调用后端并直接读回结果；不引入线程与锁。
 ****************************************************************************/
#pragma once

#include <QObject>
#include <QString>

namespace cv {

class Backend;

class AppController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString serverUrl         READ serverUrl         WRITE setServerUrl         NOTIFY serverUrlChanged)
    Q_PROPERTY(QString accessToken       READ accessToken       WRITE setAccessToken       NOTIFY accessTokenChanged)
    Q_PROPERTY(QString statusText        READ statusText        NOTIFY statusTextChanged)
    Q_PROPERTY(bool    trustSelfSigned   READ trustSelfSigned   WRITE setTrustSelfSigned   NOTIFY trustSelfSignedChanged)
    Q_PROPERTY(QString unsupportedNotice READ unsupportedNotice CONSTANT)
    Q_PROPERTY(bool    busy              READ busy              NOTIFY busyChanged)
    // 服务端版本号（来自 /healthz 的 version 字段；未探活/不可用时为空）
    Q_PROPERTY(QString serverVersion     READ serverVersion     NOTIFY serverVersionChanged)
    // 自动刷新配置（加法式）：读写 Settings 并立即落盘，供设置页绑定。
    Q_PROPERTY(bool autoRefresh         READ autoRefresh         WRITE setAutoRefresh         NOTIFY autoRefreshChanged)
    Q_PROPERTY(int  autoRefreshInterval READ autoRefreshInterval WRITE setAutoRefreshInterval NOTIFY autoRefreshIntervalChanged)

public:
    explicit AppController(Backend *backend, QObject *parent = nullptr);
    ~AppController() override;

    QString serverUrl() const { return m_serverUrl; }
    void    setServerUrl(const QString &url);

    QString accessToken() const { return m_accessToken; }
    void    setAccessToken(const QString &token);

    QString statusText() const { return m_statusText; }
    QString serverVersion() const { return m_serverVersion; }

    bool trustSelfSigned() const { return m_trustSelfSigned; }
    void setTrustSelfSigned(bool on);

    QString unsupportedNotice() const;

    bool busy() const { return m_busy; }

    // ---- 自动刷新（加法式；读写 Settings 并落盘）----
    bool autoRefresh() const;
    void setAutoRefresh(bool on);
    int  autoRefreshInterval() const;
    void setAutoRefreshInterval(int sec);

    // ---- 契约 §3 冻结方法 ----
    Q_INVOKABLE void healthCheck();     // GET /healthz（HttpBackend）或探活（其它后端）
    Q_INVOKABLE void refreshStorage();  // GET /api/v1/storage（经 Backend::usage）
    Q_INVOKABLE void saveSettings();    // 持久化 serverUrl / accessToken / trustSelfSigned

    // ---- 契约 §8.3 追加：TOFU ----
    Q_INVOKABLE void answerTrustPrompt(bool accept);
    Q_INVOKABLE QString pinnedFingerprint() const;    // 当前 serverUrl 主机的已固定指纹
    Q_INVOKABLE void    clearPinnedFingerprint();     // 清除当前 serverUrl 主机的指纹

signals:
    void serverUrlChanged();
    void accessTokenChanged();
    void statusTextChanged();
    void serverVersionChanged();
    void trustSelfSignedChanged();
    void busyChanged();
    void autoRefreshChanged();
    void autoRefreshIntervalChanged();

    // 首次连接自签名服务器时请求人工确认（QML CertPinDialog 处理）
    void trustPromptRequested(const QString &hostPort,
                              const QString &fingerprint,
                              const QString &subject,
                              const QString &issuer,
                              const QString &validity);
    void logMessage(const QString &level, const QString &text);

private:
    void    applyBackendConfig(); // 把 serverUrl / token / trustSelfSigned 推入后端
    void    installTrustPrompt(); // 契约 §8.3：向后端注入 TOFU 确认回调
    bool    requestTrust(const QString &hostPort, const QString &fingerprint,
                         const QString &subject, const QString &issuer, const QString &validity);
    void    setStatus(const QString &text, bool ok);
    void    setBusy(bool on);
    QString hostPort() const;     // 由 serverUrl 推导 "host:port"（与 HttpBackend 固定键一致）

    Backend *m_backend = nullptr;
    QString  m_serverUrl;
    QString  m_accessToken;
    QString  m_serverVersion;   // /healthz 的 version 字段
    QString  m_statusText;
    bool     m_trustSelfSigned = true;
    bool     m_busy = false;

    // TOFU 确认等待状态（仅主线程访问）
    bool m_trustPromptPending = false;
    bool m_trustAnswer = false;

signals:
    // 内部信号：QML 回答后唤醒等待中的嵌套事件循环
    void trustPromptAnswered();
};

} // namespace cv

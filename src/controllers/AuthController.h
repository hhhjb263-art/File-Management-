/****************************************************************************
 * controllers/AuthController.h —— 账户控制器（真实登录 / 注册 / 会话管理）
 *
 * 契约：docs/整合实现契约.md §3（本轮更新，从 Unsupported 移至支持）
 *   · 属性：supported(=true)、initializing、legacyMode、busy、loggedIn、
 *           userId、userName、displayName、errorKind、errorText、retryAfterSeconds
 *   · 方法：login(username,password)、registerUser(username,password,displayName)
 *           logout()、refreshMe()、startupCheck()
 *   · 信号：loggedIn(userId,userName,displayName)、loggedOut(reason)、
 *           authError(kind,message)、registered()、statusMessage、logMessage
 *
 * 令牌归属（冻结）：登录成功后由本控制器统一调用
 *   `Settings::setToken(token)` + `App.setAccessToken(token)`（AppController 是
 *   「物理注入桥」，把令牌推入 HttpBackend）；**不让两个地方各存一份令牌**。
 *
 * 向后兼容：若服务端没有账号体系（GET /api/v1/auth/me 返回 404/501），
 * startupCheck 把 `legacyMode` 置真 → QML 不显示登录门、继续用 Settings::token()。
 ****************************************************************************/
#pragma once

#include <QObject>
#include <QString>

namespace cv {

class Backend;
class AppController;
class HttpBackend;

class AuthController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool    supported           READ supported           CONSTANT)
    Q_PROPERTY(bool    initializing        READ initializing        NOTIFY initializingChanged)
    Q_PROPERTY(bool    legacyMode          READ legacyMode          NOTIFY legacyModeChanged)
    Q_PROPERTY(bool    busy                READ busy                NOTIFY busyChanged)
    Q_PROPERTY(bool    loggedIn            READ loggedIn            NOTIFY loggedInChanged)
    Q_PROPERTY(QString userId              READ userId              NOTIFY userIdChanged)
    Q_PROPERTY(QString userName            READ userName            NOTIFY userNameChanged)
    Q_PROPERTY(QString displayName         READ displayName         NOTIFY displayNameChanged)
    Q_PROPERTY(QString errorKind           READ errorKind           NOTIFY errorChanged)
    Q_PROPERTY(QString errorText           READ errorText           NOTIFY errorChanged)
    Q_PROPERTY(int     retryAfterSeconds   READ retryAfterSeconds   NOTIFY errorChanged)
    Q_PROPERTY(bool    rememberPassword    READ rememberPassword    WRITE setRememberPassword NOTIFY rememberPasswordChanged)
    Q_PROPERTY(QString savedUserName       READ savedUserName       NOTIFY savedLoginChanged)
    Q_PROPERTY(QString savedPassword       READ savedPassword       NOTIFY savedLoginChanged)

public:
    explicit AuthController(Backend *backend, AppController *app, QObject *parent = nullptr);

    bool    supported() const { return true; }
    bool    initializing() const { return m_initializing; }
    bool    legacyMode() const { return m_legacyMode; }
    bool    busy() const { return m_busy; }
    bool    loggedIn() const { return m_loggedIn; }
    QString userId() const { return m_userId; }
    QString userName() const { return m_userName; }
    QString displayName() const { return m_displayName; }
    QString errorKind() const { return m_errorKind; }
    QString errorText() const { return m_errorText; }
    int     retryAfterSeconds() const { return m_retryAfterSeconds; }
    bool    rememberPassword() const { return m_rememberPassword; }
    QString savedUserName() const { return m_savedUserName; }
    QString savedPassword() const { return m_savedPassword; }

    // ---- 契约方法（Q_INVOKABLE，供 QML 调用）----
    Q_INVOKABLE void login(const QString &username, const QString &password);
    Q_INVOKABLE void registerUser(const QString &username, const QString &password,
                                   const QString &displayName);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void refreshMe();   // 用当前令牌刷新当前用户（启动 / 恢复时调用）
    Q_INVOKABLE void startupCheck(); // 启动判定：探测服务端是否具备账号体系
    Q_INVOKABLE void clearSavedPassword(); // 关闭「记住密码」时清掉已存密码
    void setRememberPassword(bool on);   // Q_PROPERTY WRITE 用

public slots:
    // 由 HttpBackend::unauthorized() 触发（收到 401 且持有令牌）。
    void onUnauthorized();
    // 由 AppController::serverUrlChanged 触发：切服务器时清空会话与登录态。
    void onServerUrlChanged();
    // 显式重置会话（清令牌 + 登录态）；reason 透传给 loggedOut。
    void resetSession(const QString &reason = QString());

signals:
    void loggedIn(const QString &userId, const QString &userName, const QString &displayName);
    void loggedOut(const QString &reason); // reason: "user" | "expired" | "server-changed"
    void rememberPasswordChanged();
    void savedLoginChanged();
    void authError(const QString &kind, const QString &message);
    void registered(); // 注册成功（切换到登录态由 QML 处理）
    void statusMessage(const QString &message, bool ok);
    void logMessage(const QString &level, const QString &text);

    void initializingChanged();
    void legacyModeChanged();
    void busyChanged();
    void loggedInChanged();
    void userIdChanged();
    void userNameChanged();
    void displayNameChanged();
    void errorChanged();

private:
    // 清内存态（不碰 Settings/App）；emit 由调用方负责。
    void clearState();
    // 清令牌（Settings + App 注入后端）+ 清内存态；不 emit loggedOut。
    void clearSession();
    // 把 HttpBackend 的错误（带前缀）映射为界面可用的 errorKind/errorText/retryAfterSeconds。
    void applyError(const QString &prefixedError, int retryAfter);

    // 采纳一次成功认证的结果：写入用户属性 + 持久化令牌（storeToken 时）。
    // 参数用普通类型而非 HttpBackend::AuthUser，避免本头文件依赖具体后端实现。
    void adoptUser(const QString &id, const QString &username, const QString &displayName,
                   const QString &token, bool storeToken);

    // 带守卫的 setter（无变化不发 NOTIFY）
    void setInitializing(bool v);
    void setLegacyMode(bool v);
    void setBusy(bool v);
    void setLoggedIn(bool v);
    void setUserId(const QString &v);
    void setUserName(const QString &v);
    void setDisplayName(const QString &v);
    void setError(const QString &kind, const QString &text, int retryAfter);

    HttpBackend  *m_hb = nullptr;
    AppController *m_app = nullptr;

    bool   m_initializing = true;
    bool   m_legacyMode   = false;
    bool   m_busy         = false;
    bool   m_loggedIn     = false;
    QString m_userId;
    QString m_userName;
    QString m_displayName;
    QString m_errorKind;
    QString m_errorText;
    int    m_retryAfterSeconds = -1;
    bool    m_rememberPassword = false;
    QString m_savedUserName;
    QString m_savedPassword;
};

} // namespace cv

/****************************************************************************
 * controllers/AuthController.cpp —— 账户控制器实现（真实登录 / 注册 / 会话）
 ****************************************************************************/
#include "AuthController.h"

#include "controllers/AppController.h"
#include "core/Settings.h"
#include "net/HttpBackend.h"

namespace cv {

AuthController::AuthController(Backend *backend, AppController *app, QObject *parent)
    : QObject(parent)
    , m_hb(qobject_cast<HttpBackend *>(backend))
    , m_app(app)
{
    // 回想上次登录用户名（仅用于登录框预填，不暗示已登录）。
    m_userName = Settings::instance().lastUser();
}

// ---------------------------------------------------------------------------
//  启动判定：探测服务端是否具备账号体系
// ---------------------------------------------------------------------------
void AuthController::startupCheck()
{
    if (!m_hb) {
        setInitializing(false);
        return;
    }
    setInitializing(true);
    setError(QString(), QString(), -1);

    const HttpBackend::AuthResult res = m_hb->authMe();

    if (res.ok) {
        // 令牌有效（或 legacy 模式下本就无令牌，/me 返回 200 视为已登录）。
        adoptUser(res.session.user.id, res.session.user.username,
                  res.session.user.displayName, res.session.token, /*storeToken=*/true);
        setLegacyMode(false);
        setLoggedIn(true);
        setInitializing(false);
        return;
    }

    const QString e = res.error;
    if (e.startsWith(QStringLiteral("[unauthorized]"))) {
        // 令牌无效（可能过期 / 服务端换了密钥）→ 清掉，回到需登录。
        clearSession();
        setLegacyMode(false);
        setLoggedIn(false);
    } else if (e.contains(QStringLiteral("HTTP 404")) || e.contains(QStringLiteral("HTTP 501"))) {
        // 服务端无账号体系 → legacy 模式：不显示登录门，继续用 Settings::token()。
        setLegacyMode(true);
        setLoggedIn(false);
    } else {
        // 网络错误 / 服务端 5xx：无法判定，显示登录门（用户可重试）。
        setLegacyMode(false);
        setLoggedIn(false);
    }
    setInitializing(false);
}

void AuthController::refreshMe()
{
    if (!m_hb)
        return;
    const HttpBackend::AuthResult res = m_hb->authMe();
    if (res.ok) {
        setUserId(res.session.user.id);
        setUserName(res.session.user.username);
        setDisplayName(res.session.user.displayName);
    } else if (res.error.startsWith(QStringLiteral("[unauthorized]"))) {
        onUnauthorized();
    }
    // 其它（网络等）不动状态，避免误踢登录态。
}

// ---------------------------------------------------------------------------
//  登录 / 注册 / 登出
// ---------------------------------------------------------------------------
void AuthController::login(const QString &username, const QString &password)
{
    if (m_busy || !m_hb)
        return;
    setError(QString(), QString(), -1);

    if (username.trimmed().isEmpty() || password.isEmpty()) {
        setError(QStringLiteral("bad-input"),
                 QStringLiteral("请输入用户名和密码"), -1);
        emit authError(m_errorKind, m_errorText);
        return;
    }

    setBusy(true);
    // 清掉任何残留令牌，避免登录请求带上旧令牌。
    if (m_app)
        m_app->setAccessToken(QString());
    const HttpBackend::AuthResult res =
        m_hb->authLogin(username, password, QStringLiteral("CloudVault-Desktop"));
    setBusy(false);

    if (res.ok) {
        adoptUser(res.session.user.id, res.session.user.username,
                  res.session.user.displayName, res.session.token, /*storeToken=*/true);
        Settings::instance().setLastUser(username);
        Settings::instance().sync();
        setLegacyMode(false);
        setLoggedIn(true);
        emit loggedIn(m_userId, m_userName, m_displayName);
    } else {
        applyError(res.error, res.retryAfter);
        emit authError(m_errorKind, m_errorText);
    }
}

void AuthController::registerUser(const QString &username, const QString &password,
                                  const QString &displayName)
{
    if (m_busy || !m_hb)
        return;
    setError(QString(), QString(), -1);

    if (username.trimmed().isEmpty() || password.isEmpty()) {
        setError(QStringLiteral("bad-input"),
                 QStringLiteral("用户名和密码均不能为空"), -1);
        emit authError(m_errorKind, m_errorText);
        return;
    }

    setBusy(true);
    const HttpBackend::AuthResult res =
        m_hb->authRegister(username, password, displayName);
    setBusy(false);

    if (res.ok) {
        setError(QString(), QString(), -1);
        Settings::instance().setLastUser(username);
        Settings::instance().sync();
        emit registered();
        emit statusMessage(QStringLiteral("注册成功，请登录"), true);
    } else {
        applyError(res.error, res.retryAfter);
        emit authError(m_errorKind, m_errorText);
    }
}

void AuthController::logout()
{
    const bool wasLoggedIn = m_loggedIn;
    setLoggedIn(false);
    if (m_hb)
        m_hb->authLogout(); // best-effort，忽略结果
    clearSession();
    if (wasLoggedIn)
        emit loggedOut(QStringLiteral("user"));
}

// ---------------------------------------------------------------------------
//  信号驱动的会话重置
// ---------------------------------------------------------------------------
void AuthController::onUnauthorized()
{
    // 仅当确实处于登录态才视作"过期"并提示；否则静默清令牌。
    const bool wasLoggedIn = m_loggedIn;
    clearSession();
    if (wasLoggedIn) {
        emit loggedOut(QStringLiteral("expired"));
        emit statusMessage(QStringLiteral("登录已过期，请重新登录"), false);
    }
}

void AuthController::onServerUrlChanged()
{
    // 切换服务器：无论是否登录都清空（避免把 A 的令牌发给 B）。
    resetSession(QStringLiteral("server-changed"));
}

void AuthController::resetSession(const QString &reason)
{
    const bool wasLoggedIn = m_loggedIn;
    clearSession();
    setLegacyMode(false); // 未知新服务器，先不当作 legacy
    if (wasLoggedIn)
        emit loggedOut(reason.isEmpty() ? QStringLiteral("server-changed") : reason);
}

// ---------------------------------------------------------------------------
//  内部
// ---------------------------------------------------------------------------
void AuthController::adoptUser(const QString &id, const QString &username,
                               const QString &displayName, const QString &token,
                               bool storeToken)
{
    setUserId(id);
    setUserName(username);
    setDisplayName(displayName);
    if (storeToken && !token.isEmpty()) {
        Settings::instance().setToken(token);
        Settings::instance().sync();
        if (m_app)
            m_app->setAccessToken(token);
    }
}

void AuthController::clearState()
{
    setUserId(QString());
    setUserName(Settings::instance().lastUser()); // 保留上次用户名用于预填
    setDisplayName(QString());
    setLoggedIn(false);
    setError(QString(), QString(), -1);
}

void AuthController::clearSession()
{
    Settings::instance().setToken(QString());
    Settings::instance().sync();
    if (m_app)
        m_app->setAccessToken(QString());
    clearState();
}

void AuthController::applyError(const QString &err, int retryAfter)
{
    QString kind, text;
    if (err.startsWith(QStringLiteral("[unauthorized]"))) {
        kind = QStringLiteral("bad-credentials");
        text = QStringLiteral("用户名或密码错误");
    } else if (err.startsWith(QStringLiteral("[forbidden]"))) {
        kind = QStringLiteral("disabled");
        text = QStringLiteral("该账号已被禁用，请联系管理员");
    } else if (err.startsWith(QStringLiteral("[too-many-requests]"))) {
        kind = QStringLiteral("locked");
        text = QStringLiteral("尝试次数过多，账号已被锁定");
    } else if (err.startsWith(QStringLiteral("[bad-request]"))) {
        kind = QStringLiteral("bad-input");
        text = QStringLiteral("输入不合规，请检查用户名与密码");
    } else if (err.startsWith(QStringLiteral("[network]"))) {
        kind = QStringLiteral("network");
        text = QStringLiteral("网络不可达，请检查服务器地址");
    } else {
        kind = QStringLiteral("server");
        text = QStringLiteral("服务器错误，请稍后重试");
    }
    setError(kind, text, retryAfter);
}

// ---- 带守卫的 setter（避免无变化也发 NOTIFY）----
void AuthController::setInitializing(bool v)
{
    if (m_initializing == v)
        return;
    m_initializing = v;
    emit initializingChanged();
}
void AuthController::setLegacyMode(bool v)
{
    if (m_legacyMode == v)
        return;
    m_legacyMode = v;
    emit legacyModeChanged();
}
void AuthController::setBusy(bool v)
{
    if (m_busy == v)
        return;
    m_busy = v;
    emit busyChanged();
}
void AuthController::setLoggedIn(bool v)
{
    if (m_loggedIn == v)
        return;
    m_loggedIn = v;
    emit loggedInChanged();
}
void AuthController::setUserId(const QString &v)
{
    if (m_userId == v)
        return;
    m_userId = v;
    emit userIdChanged();
}
void AuthController::setUserName(const QString &v)
{
    if (m_userName == v)
        return;
    m_userName = v;
    emit userNameChanged();
}
void AuthController::setDisplayName(const QString &v)
{
    if (m_displayName == v)
        return;
    m_displayName = v;
    emit displayNameChanged();
}
void AuthController::setError(const QString &kind, const QString &text, int retryAfter)
{
    if (m_errorKind == kind && m_errorText == text && m_retryAfterSeconds == retryAfter)
        return;
    m_errorKind         = kind;
    m_errorText         = text;
    m_retryAfterSeconds = retryAfter;
    emit errorChanged();
}

} // namespace cv

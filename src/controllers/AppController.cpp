/****************************************************************************
 * controllers/AppController.cpp —— 应用 / 连接控制器实现
 ****************************************************************************/
#include "AppController.h"

#include "core/AppPaths.h"
#include "core/Settings.h"
#include "core/Util.h"
#include "net/Backend.h"
#include "net/HttpBackend.h"    // TOFU 注入 / health() 附加能力（controllers → net 允许）

#include <QEventLoop>
#include <QSettings>
#include <QTimer>
#include <QUrl>

namespace cv {
namespace {

// 与契约 §1 一致：把后端失败信息（[unsupported] 前缀 / HTTP 状态码 + 服务端 error）
// 归一成中文可操作提示，绝不吞错。
QString friendlyError(const QString &raw)
{
    if (raw.isEmpty())
        return QStringLiteral("未知错误");

    // 契约 §8.3：指纹不一致（疑似中间人）→ 用户可见警告，且不提供「仍然继续」。
    // net 层的可识别前缀为 "[pin-mismatch] "（方括号 + 空格）。
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
    if (raw.contains(QStringLiteral("HTTP 507")))
        return QStringLiteral("服务器空间不足（507）");
    if (raw.contains(QStringLiteral("HTTP 413")))
        return QStringLiteral("文件超出服务器单次上限（413）");
    if (raw.contains(QStringLiteral("HTTP 422")))
        return QStringLiteral("文件内容校验失败（哈希不符）");

    return raw;
}

constexpr int kTrustPromptTimeoutMs = 60 * 1000; // 证书确认框等待上限

} // namespace

AppController::AppController(Backend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
{
    Settings &s = Settings::instance();
    m_serverUrl   = s.serverUrl();
    m_accessToken = s.token();
    m_downloadDir = s.downloadDir(); // 默认下载目录（可能为空 → 传输层回退 AppPaths::downloadDir()）
    {
        // 信任开关没有对应的 Settings 访问器，直接读写与 HttpBackend 指纹固定同一
        // 份 INI（AppPaths::settingsFile()），键 tls/trustSelfSigned。
        QSettings raw(AppPaths::settingsFile(), QSettings::IniFormat);
        m_trustSelfSigned = raw.value(QStringLiteral("tls/trustSelfSigned"), true).toBool();
    }

    applyBackendConfig();  // 立即同步到后端，QML 改完即生效
    installTrustPrompt();  // 契约 §8.3

    // 指纹不匹配（换了服务端证书 / 疑似中间人）→ 原样转发给 QML。
    // 此前该情形在 HttpBackend 里静默失败（fail-closed 但界面无感），
    // 用户只能自己猜；转发后界面会弹「服务器证书已变更」并提供「清除记录并重新信任」。
    if (m_backend)
        QObject::connect(m_backend, &Backend::certPinMismatch,
                         this, &AppController::certPinMismatch);

    m_statusText = m_backend ? QStringLiteral("就绪") : QStringLiteral("未配置数据源");
}

AppController::~AppController() = default;

// ---------------------------------------------------------------------------
//  配置
// ---------------------------------------------------------------------------

void AppController::applyBackendConfig()
{
    if (auto *hb = qobject_cast<HttpBackend *>(m_backend)) {
        hb->setBaseUrl(m_serverUrl);
        hb->setToken(m_accessToken);
        hb->setTrustSelfSigned(m_trustSelfSigned);
    }
}

void AppController::installTrustPrompt()
{
    // 契约 §8.3：net 层不弹窗，由界面层注入确认回调；未注入 → HttpBackend 一律 fail-closed。
    if (auto *hb = qobject_cast<HttpBackend *>(m_backend)) {
        hb->setTrustPrompt([this](const QString &hostPort, const QString &fingerprint,
                                  const QString &subject, const QString &issuer,
                                  const QString &validity) {
            return requestTrust(hostPort, fingerprint, subject, issuer, validity);
        });
    }
}

void AppController::setServerUrl(const QString &url)
{
    const QString v = url.trimmed();
    if (v == m_serverUrl)
        return;
    m_serverUrl = v;
    if (auto *hb = qobject_cast<HttpBackend *>(m_backend)) {
        hb->setBaseUrl(v);
        // 🔴 缺陷修复：切换服务器必须清空令牌，否则会把 A 的令牌继续发给 B。
        hb->setToken(QString());
    }
    m_accessToken.clear(); // 同步清空内存镜像
    emit serverUrlChanged();
    emit accessTokenChanged();
    emit sessionReset(); // 通知 Auth 清登录态 + 持久化令牌
}

void AppController::setAccessToken(const QString &token)
{
    const QString v = token.trimmed();
    if (v == m_accessToken)
        return;
    m_accessToken = v;
    if (auto *hb = qobject_cast<HttpBackend *>(m_backend))
        hb->setToken(v);
    emit accessTokenChanged();
}

void AppController::setTrustSelfSigned(bool on)
{
    if (on == m_trustSelfSigned)
        return;
    m_trustSelfSigned = on;
    if (auto *hb = qobject_cast<HttpBackend *>(m_backend))
        hb->setTrustSelfSigned(on);
    emit trustSelfSignedChanged();
}

QString AppController::unsupportedNotice() const
{
    return QStringLiteral("服务端暂不支持：登录 / 用户体系、回收站、文件版本、分享链接、"
                          "标签、全文检索。相关入口已置灰。");
}

// ---- 自动刷新（加法式；读写 Settings 并立即落盘）----

bool AppController::autoRefresh() const
{
    return Settings::instance().autoRefreshEnabled();
}

void AppController::setAutoRefresh(bool on)
{
    Settings &s = Settings::instance();
    if (s.autoRefreshEnabled() == on) {
        return;
    }
    s.setAutoRefreshEnabled(on);
    s.sync(); // 立即落盘，重启后仍生效
    emit autoRefreshChanged();
}

int AppController::autoRefreshInterval() const
{
    return Settings::instance().autoRefreshIntervalSec();
}

void AppController::setAutoRefreshInterval(int sec)
{
    Settings &s = Settings::instance();
    const int clamped = qBound(5, sec, 3600); // 合法范围 5..3600 秒
    if (s.autoRefreshIntervalSec() == clamped) {
        return;
    }
    s.setAutoRefreshIntervalSec(clamped);
    s.sync(); // 立即落盘
    emit autoRefreshIntervalChanged();
}

// ---- 默认下载目录（加法式）----

void AppController::setDownloadDir(const QString &dir)
{
    const QString v = dir.trimmed();
    if (v == m_downloadDir)
        return;
    m_downloadDir = v;
    Settings::instance().setDownloadDir(v); // 只写 Settings（内存），落盘由 saveSettings() 负责
    emit downloadDirChanged();              // Application 监听 → 重新注入 TransferManager（改完即生效）
}

void AppController::saveSettings()
{
    Settings &s = Settings::instance();
    s.setServerUrl(m_serverUrl);
    s.setToken(m_accessToken);
    s.setDownloadDir(m_downloadDir); // 默认下载目录一并持久化

    QSettings raw(AppPaths::settingsFile(), QSettings::IniFormat);
    raw.setValue(QStringLiteral("tls/trustSelfSigned"), m_trustSelfSigned);
    raw.sync();
    s.sync();

    applyBackendConfig(); // 兜底再推一次（setter 已推，防御外部直接改字段的场景）
    // 通知 Application 重新注入下载目录（即便是外部直接改字段、未走 setter 也能生效）
    emit downloadDirChanged();
    setStatus(QStringLiteral("✓ 设置已保存"), true);
    emit logMessage(QStringLiteral("INFO"),
                    QStringLiteral("设置已保存（服务器=%1，允许自签名证书=%2）")
                        .arg(m_serverUrl.isEmpty() ? QStringLiteral("(默认)") : m_serverUrl,
                             m_trustSelfSigned ? QStringLiteral("是") : QStringLiteral("否")));
}

// ---------------------------------------------------------------------------
//  探活 / 空间
// ---------------------------------------------------------------------------

void AppController::healthCheck()
{
    if (!m_backend) {
        setStatus(QStringLiteral("✗ 未配置数据源"), false);
        return;
    }

    setBusy(true);
    bool    ok = false;
    QString err;

    QString version;   // 远程服务的版本号（/healthz 的 version 字段）
    if (auto *hb = qobject_cast<HttpBackend *>(m_backend)) {
        // 远程服务：走真实探活 GET /healthz（免鉴权）
        const Result<bool> r = hb->health();
        ok = r.ok;
        if (!ok) {
            err = r.error;
        } else {
            version = hb->serverVersion();
        }
    } else {
        // 其它后端（本地引擎）：用「列根目录」作为可用性探针
        const Result<QVector<FileItem>> r = m_backend->listFolder(kRootId);
        ok = r.ok;
        if (!ok)
            err = r.error;
    }

    setBusy(false);

    if (m_serverVersion != version) {
        m_serverVersion = version;
        emit serverVersionChanged();
    }

    if (ok) {
        setStatus(version.isEmpty()
                      ? QStringLiteral("✓ 连接正常（%1）").arg(m_backend->displayName())
                      : QStringLiteral("✓ 连接正常（%1 v%2）")
                            .arg(m_backend->displayName(), version),
                  true);
        emit logMessage(QStringLiteral("INFO"),
                        QStringLiteral("健康检查通过：%1").arg(m_serverUrl));
    } else {
        setStatus(QStringLiteral("✗ 连接失败：%1").arg(friendlyError(err)), false);
        emit logMessage(QStringLiteral("ERROR"),
                        QStringLiteral("健康检查失败：%1").arg(err));
    }
}

void AppController::refreshStorage()
{
    if (!m_backend) {
        setStatus(QStringLiteral("✗ 未配置数据源"), false);
        return;
    }

    const Result<UsageStats> r = m_backend->usage();
    if (!r.ok) {
        setStatus(QStringLiteral("✗ 空间查询失败：%1").arg(friendlyError(r.error)), false);
        return;
    }

    const UsageStats u = r.value;
    if (u.total > 0) {
        const qint64 freeB = qMax<qint64>(0, u.total - u.used);
        setStatus(QStringLiteral("服务器剩余：%1 / %2")
                      .arg(Util::humanSize(freeB), Util::humanSize(u.total)),
                  true);
    } else {
        setStatus(QStringLiteral("服务器空间：不可用"), false);
    }
}

// ---------------------------------------------------------------------------
//  TOFU（契约 §8.3）
// ---------------------------------------------------------------------------

bool AppController::requestTrust(const QString &hostPort, const QString &fingerprint,
                                 const QString &subject, const QString &issuer,
                                 const QString &validity)
{
    // 已有待决确认 → 直接拒绝（fail-closed），避免并发请求堆叠弹窗。
    if (m_trustPromptPending)
        return false;

    // 无 QML 接线（无人处理 trustPromptRequested）→ 直接拒绝，绝不静默信任，
    // 也避免在无接收者时白白阻塞事件循环。
    if (receivers(SIGNAL(trustPromptRequested(QString, QString, QString, QString, QString))) == 0) {
        emit logMessage(QStringLiteral("WARN"),
                        QStringLiteral("首次连接 %1 需要确认证书，但界面未接线，已拒绝该连接")
                            .arg(hostPort));
        return false;
    }

    m_trustPromptPending = true;
    m_trustAnswer = false;

    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(this, &AppController::trustPromptAnswered, &loop, &QEventLoop::quit);

    // 先把请求发出去（QML 弹框），再进事件循环等回答
    emit trustPromptRequested(hostPort, fingerprint, subject, issuer, validity);
    timer.start(kTrustPromptTimeoutMs);
    loop.exec();

    const bool timedOut = !timer.isActive(); // 计时器已触发 => 超时
    timer.stop();
    m_trustPromptPending = false;

    if (timedOut) {
        setStatus(QStringLiteral("⚠ 证书确认超时，已拒绝连接"), false);
        emit logMessage(QStringLiteral("WARN"),
                        QStringLiteral("证书确认超时（%1），已拒绝连接").arg(hostPort));
        return false;
    }
    return m_trustAnswer;
}

void AppController::answerTrustPrompt(bool accept)
{
    if (!m_trustPromptPending)
        return; // 无待决请求（迟到 / 重复回调），忽略
    m_trustAnswer = accept;
    emit trustPromptAnswered();
}

QString AppController::hostPort() const
{
    const QUrl u(m_serverUrl);
    if (!u.isValid() || u.host().isEmpty())
        return QString();
    int port = u.port();
    if (port < 0) {
        const bool https =
            (u.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0);
        port = https ? 443 : 8080;
    }
    return QStringLiteral("%1:%2").arg(u.host()).arg(port);
}

QString AppController::pinnedFingerprint() const
{
    if (auto *hb = qobject_cast<HttpBackend *>(m_backend)) {
        const QString key = hostPort();
        if (!key.isEmpty())
            return hb->pinnedFingerprint(key);
    }
    return QString();
}

void AppController::clearPinnedFingerprint()
{
    if (auto *hb = qobject_cast<HttpBackend *>(m_backend)) {
        const QString key = hostPort();
        if (key.isEmpty()) {
            setStatus(QStringLiteral("✗ 服务器地址无效，无法定位指纹"), false);
            return;
        }
        hb->clearPinnedFingerprint(key);
        setStatus(QStringLiteral("✓ 已清除 %1 的证书指纹，下次连接将重新确认").arg(key), true);
        emit logMessage(QStringLiteral("INFO"),
                        QStringLiteral("已清除证书指纹固定记录：%1").arg(key));
    }
}

// ---------------------------------------------------------------------------
//  内部
// ---------------------------------------------------------------------------

void AppController::setStatus(const QString &text, bool ok)
{
    m_statusText = text;
    emit statusTextChanged();
    if (!ok) {
        // 失败与告警另走日志出口，便于界面在日志区留痕（不吞错）
        emit logMessage(QStringLiteral("WARN"), text);
    }
}

void AppController::setBusy(bool on)
{
    if (on == m_busy)
        return;
    m_busy = on;
    emit busyChanged();
}

} // namespace cv

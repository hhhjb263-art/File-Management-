#include "Settings.h"

#include "AppPaths.h"
#include "Crypto.h"
#include "Util.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace cv {
namespace {

// 默认值
constexpr int    kDefaultConcurrency   = 3;
constexpr int    kDefaultChunkSizeMB   = 5;
constexpr int    kDefaultScanInterval  = 10;

// 自动刷新默认值 / 合法区间（秒）
constexpr int    kDefaultAutoRefreshInterval = 30;
constexpr int    kMinAutoRefreshSec          = 5;
constexpr int    kMaxAutoRefreshSec          = 3600;

QString syncModeToString(SyncMode mode)
{
    switch (mode) {
    case SyncMode::TwoWay:       return QStringLiteral("two-way");
    case SyncMode::UploadOnly:   return QStringLiteral("upload");
    case SyncMode::DownloadOnly: return QStringLiteral("download");
    }
    return QStringLiteral("two-way");
}

SyncMode syncModeFromString(const QString &s)
{
    if (s == QStringLiteral("upload"))   return SyncMode::UploadOnly;
    if (s == QStringLiteral("download")) return SyncMode::DownloadOnly;
    return SyncMode::TwoWay;
}

} // namespace

Settings::Settings(QObject *parent)
    : QObject(parent)
    , m_settings(AppPaths::settingsFile(), QSettings::IniFormat)
{
    // Qt6 的 QSettings 已移除 setIniCodec()：INI 文件一律按 UTF-8 处理，
    // 且此处构造时已显式指定 QSettings::IniFormat，无需再做编码设置。
}

Settings &Settings::instance()
{
    static Settings s;
    return s;
}

// ---------------- 后端 ----------------

QString Settings::backendMode() const
{
    return m_settings.value(QStringLiteral("backend/mode"), QStringLiteral("local")).toString();
}
void Settings::setBackendMode(const QString &mode) { m_settings.setValue(QStringLiteral("backend/mode"), mode); }

QString Settings::serverUrl() const
{
    return m_settings.value(QStringLiteral("backend/serverUrl"), QStringLiteral("http://127.0.0.1:8080")).toString();
}
void Settings::setServerUrl(const QString &url) { m_settings.setValue(QStringLiteral("backend/serverUrl"), url); }

QString Settings::token() const { return m_settings.value(QStringLiteral("backend/token")).toString(); }
void    Settings::setToken(const QString &token) { m_settings.setValue(QStringLiteral("backend/token"), token); }

QString Settings::lastUser() const { return m_settings.value(QStringLiteral("backend/lastUser")).toString(); }
void    Settings::setLastUser(const QString &user) { m_settings.setValue(QStringLiteral("backend/lastUser"), user); }

bool    Settings::rememberPassword() const { return m_settings.value(QStringLiteral("auth/rememberPassword"), false).toBool(); }
void    Settings::setRememberPassword(bool on) { m_settings.setValue(QStringLiteral("auth/rememberPassword"), on); }

QString Settings::savedUser() const { return m_settings.value(QStringLiteral("auth/savedUser")).toString(); }
void    Settings::setSavedUser(const QString &user) { m_settings.setValue(QStringLiteral("auth/savedUser"), user); }

QString Settings::savedPassword() const { return m_settings.value(QStringLiteral("auth/savedPassword")).toString(); }
void    Settings::setSavedPassword(const QString &pwd) { m_settings.setValue(QStringLiteral("auth/savedPassword"), pwd); }

// ---------------- 外观 ----------------

bool Settings::darkMode() const { return m_settings.value(QStringLiteral("ui/darkMode"), false).toBool(); }
void Settings::setDarkMode(bool dark) { m_settings.setValue(QStringLiteral("ui/darkMode"), dark); }

// ---------------- 常规 ----------------

bool Settings::launchAtLogin() const { return m_settings.value(QStringLiteral("general/launchAtLogin"), false).toBool(); }
void Settings::setLaunchAtLogin(bool on) { m_settings.setValue(QStringLiteral("general/launchAtLogin"), on); }

bool Settings::closeToTray() const { return m_settings.value(QStringLiteral("general/closeToTray"), true).toBool(); }
void Settings::setCloseToTray(bool on) { m_settings.setValue(QStringLiteral("general/closeToTray"), on); }

QString Settings::downloadDir() const
{
    return m_settings.value(QStringLiteral("general/downloadDir"), AppPaths::downloadDir()).toString();
}
void Settings::setDownloadDir(const QString &dir) { m_settings.setValue(QStringLiteral("general/downloadDir"), dir); }

// ---------------- 传输 ----------------

int  Settings::concurrency() const { return m_settings.value(QStringLiteral("transfer/concurrency"), kDefaultConcurrency).toInt(); }
void Settings::setConcurrency(int n) { m_settings.setValue(QStringLiteral("transfer/concurrency"), qMax(1, n)); }

int  Settings::chunkSizeMB() const { return m_settings.value(QStringLiteral("transfer/chunkMB"), kDefaultChunkSizeMB).toInt(); }
void Settings::setChunkSizeMB(int mb) { m_settings.setValue(QStringLiteral("transfer/chunkMB"), qBound(1, mb, 64)); }

// ---------------- 同步 ----------------

bool Settings::autoSync() const { return m_settings.value(QStringLiteral("sync/auto"), true).toBool(); }
void Settings::setAutoSync(bool on) { m_settings.setValue(QStringLiteral("sync/auto"), on); }

int  Settings::fullScanIntervalMin() const
{
    return m_settings.value(QStringLiteral("sync/scanIntervalMin"), kDefaultScanInterval).toInt();
}
void Settings::setFullScanIntervalMin(int minutes)
{
    m_settings.setValue(QStringLiteral("sync/scanIntervalMin"), qMax(1, minutes));
}

// ---------------- 自动刷新 ----------------

bool Settings::autoRefreshEnabled() const
{
    return m_settings.value(QStringLiteral("general/autoRefresh"), true).toBool();
}
void Settings::setAutoRefreshEnabled(bool on)
{
    m_settings.setValue(QStringLiteral("general/autoRefresh"), on);
}

int Settings::autoRefreshIntervalSec() const
{
    // 读取时也钳制：防手改 INI 写入越界值。
    return qBound(kMinAutoRefreshSec,
                  m_settings.value(QStringLiteral("general/autoRefreshIntervalSec"),
                                   kDefaultAutoRefreshInterval).toInt(),
                  kMaxAutoRefreshSec);
}
void Settings::setAutoRefreshIntervalSec(int sec)
{
    m_settings.setValue(QStringLiteral("general/autoRefreshIntervalSec"),
                        qBound(kMinAutoRefreshSec, sec, kMaxAutoRefreshSec));
}

QVector<SyncPair> Settings::syncPairs() const
{
    QVector<SyncPair> pairs;
    const QString     raw = m_settings.value(QStringLiteral("sync/pairs")).toString();
    if (raw.isEmpty())
        return pairs;

    const QJsonArray arr = QJsonDocument::fromJson(raw.toUtf8()).array();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        SyncPair p;
        p.id       = o.value(QStringLiteral("id")).toString();
        p.localPath  = o.value(QStringLiteral("local")).toString();
        p.remotePath = o.value(QStringLiteral("remote")).toString();
        p.mode       = syncModeFromString(o.value(QStringLiteral("mode")).toString());
        p.enabled    = o.value(QStringLiteral("enabled")).toBool(true);
        if (p.id.isEmpty())
            p.id = Crypto::randomHex(8);
        pairs.append(p);
    }
    return pairs;
}

void Settings::setSyncPairs(const QVector<SyncPair> &pairs)
{
    QJsonArray arr;
    for (const SyncPair &p : pairs) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), p.id);
        o.insert(QStringLiteral("local"), p.localPath);
        o.insert(QStringLiteral("remote"), p.remotePath);
        o.insert(QStringLiteral("mode"), syncModeToString(p.mode));
        o.insert(QStringLiteral("enabled"), p.enabled);
        arr.append(o);
    }
    m_settings.setValue(QStringLiteral("sync/pairs"),
                        QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

void Settings::sync() { m_settings.sync(); }

// ---------------- 分享提取码本地缓存 ----------------

QVariantMap Settings::shareCodes() const
{
    return m_settings.value(QStringLiteral("share/codes")).toMap();
}

void Settings::setShareCode(const QString &id, const QString &code)
{
    if (id.isEmpty())
        return;
    QVariantMap m = shareCodes();
    m.insert(id, code);
    m_settings.setValue(QStringLiteral("share/codes"), m);
}

void Settings::dropShareCodes(const QStringList &ids)
{
    if (ids.isEmpty())
        return;
    QVariantMap m = shareCodes();
    for (const QString &id : ids)
        m.remove(id);
    m_settings.setValue(QStringLiteral("share/codes"), m);
}

} // namespace cv

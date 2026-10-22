#include "AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace cv {
namespace {

QString writableLocation(QStandardPaths::StandardLocation loc, const QString &fallback)
{
    QString path = QStandardPaths::writableLocation(loc);
    if (path.isEmpty())
        path = fallback;
    return path;
}

} // namespace

bool AppPaths::isPortable()
{
    // 便携模式判定：**exe 同目录**存在 portable.txt。
    // 用途：把整个 bin/ 拷进 U 盘/移动硬盘，插上任何 Windows 就能用，
    // 配置、缓存、日志、下载全在 exe 旁边，拔走不留痕（也不污染那台机器）。
    static const bool portable = QFileInfo::exists(
        QCoreApplication::applicationDirPath() + QStringLiteral("/portable.txt"));
    return portable;
}

QString AppPaths::rootDir()
{
    if (isPortable())
        return QCoreApplication::applicationDirPath() + QStringLiteral("/portable");

    const QString base =
        writableLocation(QStandardPaths::AppLocalDataLocation, QDir::homePath() + QStringLiteral("/.cloudvault"));
    return base + QStringLiteral("/CloudVault");
}

QString AppPaths::configDir()   { return rootDir() + QStringLiteral("/config"); }
QString AppPaths::logDir()      { return rootDir() + QStringLiteral("/logs"); }
QString AppPaths::cacheDir()    { return rootDir() + QStringLiteral("/cache"); }
QString AppPaths::dataDir()     { return rootDir() + QStringLiteral("/store"); }

QString AppPaths::downloadDir()
{
    // 便携模式：默认下载到 exe 旁边的 downloads/（否则会散落到每台机器的「下载」目录里）
    if (isPortable())
        return QCoreApplication::applicationDirPath() + QStringLiteral("/portable/downloads");
    QString dir = writableLocation(QStandardPaths::DownloadLocation, QDir::homePath() + QStringLiteral("/Downloads"));
    return dir + QStringLiteral("/CloudVault");
}

QString AppPaths::settingsFile() { return configDir() + QStringLiteral("/settings.ini"); }
QString AppPaths::indexDbFile()  { return configDir() + QStringLiteral("/sync.db"); }
QString AppPaths::localDbFile()  { return dataDir() + QStringLiteral("/db.json"); }

void AppPaths::ensureDirs()
{
    const QStringList dirs = {rootDir(),  configDir(), logDir(),
                              cacheDir(), dataDir(),   downloadDir()};
    for (const QString &d : dirs)
        QDir().mkpath(d);
}

} // namespace cv

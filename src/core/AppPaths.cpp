#include "AppPaths.h"

#include <QDir>
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

QString AppPaths::rootDir()
{
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

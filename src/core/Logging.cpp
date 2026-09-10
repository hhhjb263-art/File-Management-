#include "Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>
#include <QtGlobal>
#include <cstdio>

namespace cv {
namespace {

QMutex     g_mutex;
QFile      g_logFile;
QString    g_logPath;

const char *levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return "DBG";
    case QtInfoMsg:     return "INF";
    case QtWarningMsg:  return "WRN";
    case QtCriticalMsg: return "ERR";
    case QtFatalMsg:    return "FTL";
    }
    return "???";
}

void handleMessage(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("MM-dd HH:mm:ss.zzz"));
    const QString line =
        QStringLiteral("[%1][%2] %3").arg(QString::fromLatin1(levelName(type)), stamp, msg);

    QMutexLocker locker(&g_mutex);

    std::fprintf(stderr, "%s\n", qPrintable(line));
    std::fflush(stderr);

    if (g_logFile.isOpen()) {
        QTextStream out(&g_logFile);
        out.setEncoding(QStringConverter::Utf8);
        out << line;
        if (ctx.function && std::strlen(ctx.function) > 0)
            out << QStringLiteral("   (") << QString::fromLatin1(ctx.function) << QLatin1Char(')');
        out << Qt::endl;
    }

    if (type == QtFatalMsg)
        std::abort();
}

} // namespace

void Logging::install(const QString &dir)
{
    QDir().mkpath(dir);
    g_logPath = dir + QStringLiteral("/cloudvault.log");

    QMutexLocker locker(&g_mutex);
    if (g_logFile.isOpen())
        g_logFile.close();
    g_logFile.setFileName(g_logPath);
    g_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);

    qInstallMessageHandler(handleMessage);
}

QString Logging::logFilePath()
{
    return g_logPath;
}

} // namespace cv

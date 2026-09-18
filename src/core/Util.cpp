#include "Util.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHostInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

namespace cv {

QString Util::humanSize(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("-");
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);

    constexpr double KB = 1024.0;
    const double     mb = KB * 1024.0;
    const double     gb = mb * 1024.0;
    const double     tb = gb * 1024.0;

    auto fmt = [](double v) { return QString::number(v, 'f', v >= 100 ? 0 : (v >= 10 ? 1 : 2)); };

    if (bytes < mb)
        return QStringLiteral("%1 KB").arg(fmt(double(bytes) / KB));
    if (bytes < gb)
        return QStringLiteral("%1 MB").arg(fmt(double(bytes) / mb));
    if (bytes < tb)
        return QStringLiteral("%1 GB").arg(fmt(double(bytes) / gb));
    return QStringLiteral("%1 TB").arg(fmt(double(bytes) / tb));
}

QString Util::humanSpeed(double bytesPerSec)
{
    if (bytesPerSec <= 0)
        return QStringLiteral("—");
    return humanSize(qint64(bytesPerSec)) + QStringLiteral("/s");
}

QString Util::relativeTime(const QDateTime &dt)
{
    if (!dt.isValid())
        return QStringLiteral("—");

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const qint64    sec = dt.secsTo(now);

    if (sec < 60)
        return QStringLiteral("刚刚");
    if (sec < 3600)
        return QStringLiteral("%1 分钟前").arg(sec / 60);
    if (sec < 86400)
        return QStringLiteral("%1 小时前").arg(sec / 3600);
    if (sec < 86400 * 2)
        return QStringLiteral("昨天");
    if (sec < 86400 * 30)
        return QStringLiteral("%1 天前").arg(sec / 86400);

    return dt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd"));
}

QString Util::formatTime(const QDateTime &dt)
{
    if (!dt.isValid())
        return QStringLiteral("—");
    return dt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString Util::formatDate(const QDateTime &dt)
{
    if (!dt.isValid())
        return QStringLiteral("—");
    return dt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd"));
}

QString Util::typeName(int type)
{
    switch (FileType(type)) {
    case FileType::Folder:   return QStringLiteral("文件夹");
    case FileType::Document: return QStringLiteral("文档");
    case FileType::Image:    return QStringLiteral("图片");
    case FileType::Video:    return QStringLiteral("视频");
    case FileType::Audio:    return QStringLiteral("音频");
    case FileType::Archive:  return QStringLiteral("压缩包");
    case FileType::Code:     return QStringLiteral("代码");
    default:                 return QStringLiteral("其它");
    }
}

QString Util::typeIcon(int type)
{
    switch (FileType(type)) {
    case FileType::Folder:   return QStringLiteral("qrc:/icons/folder.svg");
    case FileType::Document: return QStringLiteral("qrc:/icons/doc.svg");
    case FileType::Image:    return QStringLiteral("qrc:/icons/image.svg");
    case FileType::Video:    return QStringLiteral("qrc:/icons/video.svg");
    case FileType::Audio:    return QStringLiteral("qrc:/icons/audio.svg");
    case FileType::Archive:  return QStringLiteral("qrc:/icons/archive.svg");
    case FileType::Code:     return QStringLiteral("qrc:/icons/code.svg");
    default:                 return QStringLiteral("qrc:/icons/file.svg");
    }
}

FileType Util::fileTypeOf(const QString &name, bool isDir)
{
    if (isDir)
        return FileType::Folder;

    const QString ext = QFileInfo(name).suffix().toLower();

    static const QSet<QString> images = {
        QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("gif"),  QStringLiteral("bmp"),  QStringLiteral("webp"),
        QStringLiteral("svg"),  QStringLiteral("heic"), QStringLiteral("ico"),
        QStringLiteral("tiff"), QStringLiteral("avif")};
    static const QSet<QString> videos = {
        QStringLiteral("mp4"),  QStringLiteral("mkv"),  QStringLiteral("mov"),
        QStringLiteral("avi"),  QStringLiteral("flv"),  QStringLiteral("wmv"),
        QStringLiteral("webm"), QStringLiteral("ts"),   QStringLiteral("m4v")};
    static const QSet<QString> audios = {
        QStringLiteral("mp3"), QStringLiteral("wav"),  QStringLiteral("flac"),
        QStringLiteral("aac"), QStringLiteral("ogg"),  QStringLiteral("m4a"),
        QStringLiteral("ape"), QStringLiteral("wma")};
    static const QSet<QString> docs = {
        QStringLiteral("pdf"),  QStringLiteral("doc"),  QStringLiteral("docx"),
        QStringLiteral("xls"),  QStringLiteral("xlsx"), QStringLiteral("ppt"),
        QStringLiteral("pptx"), QStringLiteral("txt"),  QStringLiteral("md"),
        QStringLiteral("csv"),  QStringLiteral("rtf"),  QStringLiteral("epub"),
        QStringLiteral("wps"),  QStringLiteral("et"),   QStringLiteral("dps")};
    static const QSet<QString> archives = {
        QStringLiteral("zip"), QStringLiteral("rar"), QStringLiteral("7z"),
        QStringLiteral("tar"), QStringLiteral("gz"),  QStringLiteral("bz2"),
        QStringLiteral("xz"),  QStringLiteral("iso"), QStringLiteral("dmg")};
    static const QSet<QString> codes = {
        QStringLiteral("c"),   QStringLiteral("cpp"),  QStringLiteral("h"),
        QStringLiteral("hpp"), QStringLiteral("js"),   QStringLiteral("ts"),
        QStringLiteral("py"),  QStringLiteral("java"), QStringLiteral("go"),
        QStringLiteral("rs"),  QStringLiteral("json"), QStringLiteral("xml"),
        QStringLiteral("yaml"), QStringLiteral("yml"), QStringLiteral("sh"),
        QStringLiteral("sql"), QStringLiteral("qml"),  QStringLiteral("css"),
        QStringLiteral("html")};

    if (images.contains(ext))   return FileType::Image;
    if (videos.contains(ext))   return FileType::Video;
    if (audios.contains(ext))   return FileType::Audio;
    if (docs.contains(ext))     return FileType::Document;
    if (archives.contains(ext)) return FileType::Archive;
    if (codes.contains(ext))    return FileType::Code;
    return FileType::Other;
}

QString Util::fileKindOf(const QString &name)
{
    return typeName(int(fileTypeOf(name, false)));
}

QString Util::urlToLocalPath(const QString &url)
{
    return QUrl(url).toLocalFile();
}

QString Util::joinPath(const QString &dir, const QString &name)
{
    if (dir.isEmpty() || dir == QStringLiteral("/"))
        return name;
    return dir.endsWith(QLatin1Char('/')) ? dir + name : dir + QLatin1Char('/') + name;
}

QString Util::uniquePath(const QString &path)
{
    if (!QFileInfo::exists(path))
        return path;

    const QFileInfo info(path);
    const QString   dir  = info.absolutePath();
    const QString   base = info.completeBaseName();
    const QString   ext  = info.suffix();

    for (int i = 1; i < 10000; ++i) {
        // 逐参链式替换：Qt6 已移除 arg(int, const QString &) 等混合重载
        const QString candidate = ext.isEmpty()
                                      ? QStringLiteral("%1/%2 (%3)").arg(dir).arg(base).arg(i)
                                      : QStringLiteral("%1/%2 (%3).%4").arg(dir).arg(base).arg(i).arg(ext);
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    return path;
}

QString Util::conflictPath(const QString &path, const QString &device)
{
    const QFileInfo info(path);
    const QString   stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString   ext   = info.suffix();
    const QString   base  = QStringLiteral("%1/%2 (冲突-%3-%4)")
                                .arg(info.absolutePath(), info.completeBaseName(), device, stamp);
    return ext.isEmpty() ? base : base + QLatin1Char('.') + ext;
}

QString Util::sanitizeName(const QString &name)
{
    QString out = name;
    static const QRegularExpression illegal(QStringLiteral("[\\\\/:*?\"<>|]"));
    out.remove(illegal);
    out = out.trimmed();
    if (out.isEmpty())
        out = QStringLiteral("未命名");
    return out;
}

QString Util::deviceName()
{
    QString host = QHostInfo::localHostName();
    if (host.isEmpty())
        host = QStringLiteral("本机");
    return host;
}

QString Util::initialsOf(const QString &name)
{
    if (name.isEmpty())
        return QStringLiteral("?");
    // 中文取最后一个字，英文取首字母
    const ushort first = name.at(0).unicode();
    if (first > 0x2E80)
        return QString(name.at(name.size() - 1));
    return QString(name.at(0)).toUpper();
}

} // namespace cv

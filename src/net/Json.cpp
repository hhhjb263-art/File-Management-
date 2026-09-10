#include "Json.h"

#include <QJsonArray>

namespace cv {
namespace Json {

QString dtToString(const QDateTime &dt)
{
    return dt.isValid() ? dt.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QDateTime dtFromString(const QString &s)
{
    return s.isEmpty() ? QDateTime() : QDateTime::fromString(s, Qt::ISODateWithMs);
}

// ---------------- FileItem ----------------

QJsonObject toJson(const FileItem &f)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), f.id);
    o.insert(QStringLiteral("parentId"), f.parentId);
    o.insert(QStringLiteral("path"), f.path);
    o.insert(QStringLiteral("name"), f.name);
    o.insert(QStringLiteral("isDir"), f.isDir);
    o.insert(QStringLiteral("size"), double(f.size));
    o.insert(QStringLiteral("modified"), dtToString(f.modified));
    o.insert(QStringLiteral("hash"), f.hash);
    o.insert(QStringLiteral("rev"), f.rev);
    o.insert(QStringLiteral("version"), f.version);
    o.insert(QStringLiteral("type"), int(f.type));
    o.insert(QStringLiteral("trashed"), f.trashed);
    o.insert(QStringLiteral("trashedAt"), dtToString(f.trashedAt));
    o.insert(QStringLiteral("device"), f.device);

    QJsonArray tags;
    for (const QString &t : f.tagIds)
        tags.append(t);
    o.insert(QStringLiteral("tags"), tags);
    return o;
}

FileItem fileFromJson(const QJsonObject &o)
{
    FileItem f;
    f.id        = o.value(QStringLiteral("id")).toString();
    f.parentId  = o.value(QStringLiteral("parentId")).toString();
    f.path      = o.value(QStringLiteral("path")).toString();
    f.name      = o.value(QStringLiteral("name")).toString();
    f.isDir     = o.value(QStringLiteral("isDir")).toBool();
    f.size      = qint64(o.value(QStringLiteral("size")).toDouble());
    f.modified  = dtFromString(o.value(QStringLiteral("modified")).toString());
    f.hash      = o.value(QStringLiteral("hash")).toString();
    f.rev       = o.value(QStringLiteral("rev")).toString();
    f.version   = o.value(QStringLiteral("version")).toInt(1);
    f.type      = FileType(o.value(QStringLiteral("type")).toInt());
    f.trashed   = o.value(QStringLiteral("trashed")).toBool();
    f.trashedAt = dtFromString(o.value(QStringLiteral("trashedAt")).toString());
    f.device    = o.value(QStringLiteral("device")).toString();

    const QJsonArray tags = o.value(QStringLiteral("tags")).toArray();
    for (const QJsonValue &v : tags)
        f.tagIds.append(v.toString());
    return f;
}

// ---------------- FileVersion ----------------

QJsonObject toJson(const FileVersion &v)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), v.id);
    o.insert(QStringLiteral("fileId"), v.fileId);
    o.insert(QStringLiteral("version"), v.version);
    o.insert(QStringLiteral("size"), double(v.size));
    o.insert(QStringLiteral("created"), dtToString(v.created));
    o.insert(QStringLiteral("device"), v.device);
    o.insert(QStringLiteral("note"), v.note);
    return o;
}

FileVersion versionFromJson(const QJsonObject &o)
{
    FileVersion v;
    v.id      = o.value(QStringLiteral("id")).toString();
    v.fileId  = o.value(QStringLiteral("fileId")).toString();
    v.version = o.value(QStringLiteral("version")).toInt();
    v.size    = qint64(o.value(QStringLiteral("size")).toDouble());
    v.created = dtFromString(o.value(QStringLiteral("created")).toString());
    v.device  = o.value(QStringLiteral("device")).toString();
    v.note    = o.value(QStringLiteral("note")).toString();
    return v;
}

// ---------------- ShareLink ----------------

QJsonObject toJson(const ShareLink &s)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), s.id);
    o.insert(QStringLiteral("fileId"), s.fileId);
    o.insert(QStringLiteral("fileName"), s.fileName);
    o.insert(QStringLiteral("isDir"), s.isDir);
    o.insert(QStringLiteral("url"), s.url);
    o.insert(QStringLiteral("code"), s.code);
    o.insert(QStringLiteral("created"), dtToString(s.created));
    o.insert(QStringLiteral("expire"), dtToString(s.expire));
    o.insert(QStringLiteral("maxDownloads"), s.maxDownloads);
    o.insert(QStringLiteral("downloads"), s.downloads);
    o.insert(QStringLiteral("revoked"), s.revoked);
    return o;
}

ShareLink shareFromJson(const QJsonObject &o)
{
    ShareLink s;
    s.id           = o.value(QStringLiteral("id")).toString();
    s.fileId       = o.value(QStringLiteral("fileId")).toString();
    s.fileName     = o.value(QStringLiteral("fileName")).toString();
    s.isDir        = o.value(QStringLiteral("isDir")).toBool();
    s.url          = o.value(QStringLiteral("url")).toString();
    s.code         = o.value(QStringLiteral("code")).toString();
    s.created      = dtFromString(o.value(QStringLiteral("created")).toString());
    s.expire       = dtFromString(o.value(QStringLiteral("expire")).toString());
    s.maxDownloads = o.value(QStringLiteral("maxDownloads")).toInt();
    s.downloads    = o.value(QStringLiteral("downloads")).toInt();
    s.revoked      = o.value(QStringLiteral("revoked")).toBool();
    return s;
}

// ---------------- Tag ----------------

QJsonObject toJson(const Tag &t)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), t.id);
    o.insert(QStringLiteral("name"), t.name);
    o.insert(QStringLiteral("color"), t.color);
    return o;
}

Tag tagFromJson(const QJsonObject &o)
{
    Tag t;
    t.id    = o.value(QStringLiteral("id")).toString();
    t.name  = o.value(QStringLiteral("name")).toString();
    t.color = o.value(QStringLiteral("color")).toString();
    return t;
}

// ---------------- UserInfo ----------------

QJsonObject toJson(const UserInfo &u)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), u.id);
    o.insert(QStringLiteral("name"), u.name);
    o.insert(QStringLiteral("email"), u.email);
    o.insert(QStringLiteral("quota"), double(u.quota));
    return o;
}

UserInfo userFromJson(const QJsonObject &o)
{
    UserInfo u;
    u.id    = o.value(QStringLiteral("id")).toString();
    u.name  = o.value(QStringLiteral("name")).toString();
    u.email = o.value(QStringLiteral("email")).toString();
    u.quota = qint64(o.value(QStringLiteral("quota")).toDouble());
    return u;
}

// ---------------- UsageStats ----------------

QJsonObject toJson(const UsageStats &s)
{
    QJsonObject o;
    o.insert(QStringLiteral("total"), double(s.total));
    o.insert(QStringLiteral("used"), double(s.used));
    o.insert(QStringLiteral("versions"), double(s.versions));
    o.insert(QStringLiteral("trash"), double(s.trash));
    o.insert(QStringLiteral("fileCount"), double(s.fileCount));
    o.insert(QStringLiteral("folderCount"), double(s.folderCount));
    o.insert(QStringLiteral("shareCount"), double(s.shareCount));

    QJsonArray byType;
    for (const auto &p : s.byType) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), p.first);
        item.insert(QStringLiteral("size"), double(p.second));
        byType.append(item);
    }
    o.insert(QStringLiteral("byType"), byType);

    QJsonArray largest;
    for (const auto &p : s.largest) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), p.first);
        item.insert(QStringLiteral("size"), double(p.second));
        largest.append(item);
    }
    o.insert(QStringLiteral("largest"), largest);
    return o;
}

UsageStats usageFromJson(const QJsonObject &o)
{
    UsageStats s;
    s.total       = qint64(o.value(QStringLiteral("total")).toDouble());
    s.used        = qint64(o.value(QStringLiteral("used")).toDouble());
    s.versions    = qint64(o.value(QStringLiteral("versions")).toDouble());
    s.trash       = qint64(o.value(QStringLiteral("trash")).toDouble());
    s.fileCount   = qint64(o.value(QStringLiteral("fileCount")).toDouble());
    s.folderCount = qint64(o.value(QStringLiteral("folderCount")).toDouble());
    s.shareCount  = qint64(o.value(QStringLiteral("shareCount")).toDouble());

    const QJsonArray byType = o.value(QStringLiteral("byType")).toArray();
    for (const QJsonValue &v : byType) {
        const QJsonObject item = v.toObject();
        s.byType.append(
            qMakePair(item.value(QStringLiteral("name")).toString(),
                      qint64(item.value(QStringLiteral("size")).toDouble())));
    }

    const QJsonArray largest = o.value(QStringLiteral("largest")).toArray();
    for (const QJsonValue &v : largest) {
        const QJsonObject item = v.toObject();
        s.largest.append(
            qMakePair(item.value(QStringLiteral("name")).toString(),
                      qint64(item.value(QStringLiteral("size")).toDouble())));
    }
    return s;
}

// ---------------- 其它 ----------------

bool fileLess(const FileItem &a, const FileItem &b)
{
    if (a.isDir != b.isDir)
        return a.isDir;
    return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
}

} // namespace Json
} // namespace cv

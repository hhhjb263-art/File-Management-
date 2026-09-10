/****************************************************************************
 * 服务端 REST 接口约定（与 MockBackend 行为一一对应）
 *
 *   POST   /api/auth/login                      -> { token, user }
 *   GET    /api/files?parent=<id>               -> { items: [...] }
 *   GET    /api/files/tree?path=<path>          -> { items: [...] }
 *   GET    /api/trash                           -> { items: [...] }
 *   GET    /api/files/<id>                      -> { item }
 *   GET    /api/files/stat?path=<path>          -> { item }
 *   POST   /api/folders                         -> { item }          body: {parent,name}
 *   POST   /api/folders/mkdirp                  -> { item }          body: {path}
 *   POST   /api/files/<id>/rename               -> {}                body: {name}
 *   POST   /api/files/<id>/trash                -> {}
 *   POST   /api/files/<id>/restore              -> {}
 *   DELETE /api/files/<id>                      -> {}
 *   DELETE /api/trash                           -> {}
 *   PUT    /api/files/<id>/tags                 -> {}                body: {tags:[]}
 *   POST   /api/uploads                         -> { instant, uploadId, received, fileId }
 *   PUT    /api/uploads/<id>/chunk              -> {}                header Content-Range
 *   POST   /api/uploads/<id>/finish             -> { item }
 *   DELETE /api/uploads/<id>                    -> {}
 *   GET    /api/files/<id>/content              -> 二进制             header Range
 *   GET    /api/files/<id>/versions             -> { items: [...] }
 *   POST   /api/files/<id>/versions/<vid>/restore -> {}
 *   GET    /api/shares                          -> { items: [...] }
 *   POST   /api/shares                          -> { item }
 *   POST   /api/shares/<id>/revoke              -> {}
 *   POST   /api/shares/<id>/touch               -> {}
 *   GET    /api/tags                            -> { items: [...] }
 *   POST   /api/tags                            -> { item }
 *   DELETE /api/tags/<id>                       -> {}
 *   GET    /api/search?keyword=&tags=&type=     -> { items: [...] }
 *   GET    /api/stats/usage                     -> { stats }
 ****************************************************************************/
#include "HttpBackend.h"

#include "Json.h"
#include "core/Crypto.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QThreadStorage>
#include <QTimer>
#include <QUrlQuery>

namespace cv {
namespace {

constexpr int kTimeoutMs = 30000;

} // namespace

HttpBackend::HttpBackend(const QString &baseUrl, QObject *parent)
    : Backend(parent)
    , m_baseUrl(baseUrl)
{
    while (m_baseUrl.endsWith(QLatin1Char('/')))
        m_baseUrl.chop(1);
}

void HttpBackend::setBaseUrl(const QString &url)
{
    m_baseUrl = url;
    while (m_baseUrl.endsWith(QLatin1Char('/')))
        m_baseUrl.chop(1);
}

void HttpBackend::setToken(const QString &token) { m_token = token; }

QNetworkAccessManager *HttpBackend::nam()
{
    // QNetworkAccessManager 只能在创建它的线程使用，因此每线程独立实例
    static QThreadStorage<QNetworkAccessManager *> storage;
    if (!storage.hasLocalData())
        storage.setLocalData(new QNetworkAccessManager);
    return storage.localData();
}

HttpBackend::Response HttpBackend::request(Method method, const QString &path,
                                           const QByteArray &body,
                                           const QHash<QByteArray, QByteArray> &headers)
{
    QNetworkRequest req(QUrl(m_baseUrl + path));
    req.setRawHeader("Accept", "application/json");
    if (!m_token.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    if (!body.isEmpty() && !headers.contains("Content-Type"))
        req.setRawHeader("Content-Type", "application/json");
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        req.setRawHeader(it.key(), it.value());

    QNetworkReply *reply = nullptr;
    switch (method) {
    case Method::Get:    reply = nam()->get(req); break;
    case Method::Post:   reply = nam()->post(req, body); break;
    case Method::Put:    reply = nam()->put(req, body); break;
    case Method::Delete: reply = nam()->deleteResource(req); break;
    }

    Response out;

    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(kTimeoutMs);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
        reply->deleteLater();
        out.error = QStringLiteral("请求超时");
        return out;
    }
    timer.stop();

    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.body   = reply->readAll();
    if (reply->error() != QNetworkReply::NoError && out.body.isEmpty())
        out.error = reply->errorString();

    // 服务端返回的错误信息优先展示
    if (out.status >= 400) {
        const QJsonObject o = QJsonDocument::fromJson(out.body).object();
        const QString     msg = o.value(QStringLiteral("message")).toString();
        out.error = msg.isEmpty() ? QStringLiteral("服务返回 %1").arg(out.status) : msg;
    }

    reply->deleteLater();
    return out;
}

// ------------------------------ 账户 ------------------------------

Result<UserInfo> HttpBackend::login(const QString &user, const QString &password)
{
    QJsonObject body;
    body.insert(QStringLiteral("user"), user);
    body.insert(QStringLiteral("password"), password);

    const Response r = request(Method::Post, QStringLiteral("/api/auth/login"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!r.ok())
        return Result<UserInfo>::fail(r.error.isEmpty() ? QStringLiteral("登录失败") : r.error);

    const QJsonObject o    = QJsonDocument::fromJson(r.body).object();
    const QString     token = o.value(QStringLiteral("token")).toString();
    if (!token.isEmpty())
        m_token = token;

    m_user = Json::userFromJson(o.value(QStringLiteral("user")).toObject());
    return Result<UserInfo>::success(m_user);
}

void HttpBackend::logout()
{
    request(Method::Post, QStringLiteral("/api/auth/logout"));
    m_token.clear();
    m_user = UserInfo();
}

UserInfo HttpBackend::currentUser() const { return m_user; }

// ------------------------------ 文件 ------------------------------

Result<QVector<FileItem>> HttpBackend::listFolder(const QString &parentId)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("parent"), parentId);

    const Response r = request(Method::Get, QStringLiteral("/api/files?") + q.toString());
    if (!r.ok())
        return Result<QVector<FileItem>>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    return Result<QVector<FileItem>>::success(
        Json::fromArray<FileItem>(o.value(QStringLiteral("items")), &Json::fileFromJson));
}

Result<QVector<FileItem>> HttpBackend::listUnderPath(const QString &remotePath)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("path"), remotePath);

    const Response r = request(Method::Get, QStringLiteral("/api/files/tree?") + q.toString());
    if (!r.ok())
        return Result<QVector<FileItem>>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    return Result<QVector<FileItem>>::success(
        Json::fromArray<FileItem>(o.value(QStringLiteral("items")), &Json::fileFromJson));
}

Result<QVector<FileItem>> HttpBackend::listTrash()
{
    const Response r = request(Method::Get, QStringLiteral("/api/trash"));
    if (!r.ok())
        return Result<QVector<FileItem>>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    return Result<QVector<FileItem>>::success(
        Json::fromArray<FileItem>(o.value(QStringLiteral("items")), &Json::fileFromJson));
}

Result<FileItem> HttpBackend::statById(const QString &id)
{
    const Response r = request(Method::Get, QStringLiteral("/api/files/") + id);
    if (!r.ok())
        return Result<FileItem>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    return Result<FileItem>::success(Json::fileFromJson(o.value(QStringLiteral("item")).toObject()));
}

Result<FileItem> HttpBackend::statByPath(const QString &remotePath)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("path"), remotePath);

    const Response r = request(Method::Get, QStringLiteral("/api/files/stat?") + q.toString());
    if (!r.ok())
        return Result<FileItem>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    return Result<FileItem>::success(Json::fileFromJson(o.value(QStringLiteral("item")).toObject()));
}

Result<FileItem> HttpBackend::createFolder(const QString &parentId, const QString &name)
{
    QJsonObject body;
    body.insert(QStringLiteral("parent"), parentId);
    body.insert(QStringLiteral("name"), name);

    const Response r = request(Method::Post, QStringLiteral("/api/folders"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!r.ok())
        return Result<FileItem>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    return Result<FileItem>::success(Json::fileFromJson(o.value(QStringLiteral("item")).toObject()));
}

Result<FileItem> HttpBackend::ensureFolderPath(const QString &remotePath)
{
    QJsonObject body;
    body.insert(QStringLiteral("path"), remotePath);

    const Response r = request(Method::Post, QStringLiteral("/api/folders/mkdirp"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!r.ok())
        return Result<FileItem>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    return Result<FileItem>::success(Json::fileFromJson(o.value(QStringLiteral("item")).toObject()));
}

Ok HttpBackend::rename(const QString &id, const QString &newName)
{
    QJsonObject body;
    body.insert(QStringLiteral("name"), newName);

    const Response r = request(Method::Post, QStringLiteral("/api/files/") + id + QStringLiteral("/rename"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    return r.ok() ? ok() : err(r.error);
}

Ok HttpBackend::moveToTrash(const QString &id)
{
    const Response r = request(Method::Post, QStringLiteral("/api/files/") + id + QStringLiteral("/trash"));
    return r.ok() ? ok() : err(r.error);
}

Ok HttpBackend::restore(const QString &id)
{
    const Response r = request(Method::Post, QStringLiteral("/api/files/") + id + QStringLiteral("/restore"));
    return r.ok() ? ok() : err(r.error);
}

Ok HttpBackend::purge(const QString &id)
{
    const Response r = request(Method::Delete, QStringLiteral("/api/files/") + id);
    return r.ok() ? ok() : err(r.error);
}

Ok HttpBackend::emptyTrash()
{
    const Response r = request(Method::Delete, QStringLiteral("/api/trash"));
    return r.ok() ? ok() : err(r.error);
}

Ok HttpBackend::setTags(const QString &id, const QStringList &tagIds)
{
    QJsonArray arr;
    for (const QString &t : tagIds)
        arr.append(t);

    QJsonObject body;
    body.insert(QStringLiteral("tags"), arr);

    const Response r = request(Method::Put, QStringLiteral("/api/files/") + id + QStringLiteral("/tags"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    return r.ok() ? ok() : err(r.error);
}

// ------------------------------ 传输 ------------------------------

Result<UploadTicket> HttpBackend::beginUpload(const QString &parentId, const QString &name,
                                              qint64 size, const QString &sha256)
{
    QJsonObject body;
    body.insert(QStringLiteral("parent"), parentId);
    body.insert(QStringLiteral("name"), name);
    body.insert(QStringLiteral("size"), double(size));
    body.insert(QStringLiteral("sha256"), sha256);

    const Response r = request(Method::Post, QStringLiteral("/api/uploads"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!r.ok())
        return Result<UploadTicket>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();

    UploadTicket ticket;
    ticket.instant  = o.value(QStringLiteral("instant")).toBool();
    ticket.uploadId = o.value(QStringLiteral("uploadId")).toString();
    ticket.received = qint64(o.value(QStringLiteral("received")).toDouble());
    ticket.fileId   = o.value(QStringLiteral("fileId")).toString();
    return Result<UploadTicket>::success(ticket);
}

Ok HttpBackend::putChunk(const QString &uploadId, qint64 offset, const QByteArray &data)
{
    const QString range = QStringLiteral("bytes %1-%2/*").arg(offset).arg(offset + data.size() - 1);

    QHash<QByteArray, QByteArray> headers;
    headers.insert("Content-Type", "application/octet-stream");
    headers.insert("Content-Range", range.toLatin1());

    const Response r = request(Method::Put,
                               QStringLiteral("/api/uploads/") + uploadId + QStringLiteral("/chunk"),
                               data, headers);
    return r.ok() ? ok() : err(r.error);
}

Result<FileItem> HttpBackend::finishUpload(const QString &uploadId)
{
    const Response r = request(Method::Post,
                               QStringLiteral("/api/uploads/") + uploadId + QStringLiteral("/finish"));
    if (!r.ok())
        return Result<FileItem>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    return Result<FileItem>::success(Json::fileFromJson(o.value(QStringLiteral("item")).toObject()));
}

Ok HttpBackend::cancelUpload(const QString &uploadId)
{
    const Response r = request(Method::Delete, QStringLiteral("/api/uploads/") + uploadId);
    return r.ok() ? ok() : err(r.error);
}

Result<QByteArray> HttpBackend::getRange(const QString &fileId, qint64 offset, qint64 length)
{
    const QString range = QStringLiteral("bytes=%1-%2").arg(offset).arg(offset + length - 1);

    QHash<QByteArray, QByteArray> headers;
    headers.insert("Range", range.toLatin1());

    const Response r = request(Method::Get,
                               QStringLiteral("/api/files/") + fileId + QStringLiteral("/content"),
                               QByteArray(), headers);
    if (!r.ok())
        return Result<QByteArray>::fail(r.error);
    return Result<QByteArray>::success(r.body);
}

// ------------------------------ 版本 ------------------------------

Result<QVector<FileVersion>> HttpBackend::versions(const QString &fileId)
{
    const Response r = request(Method::Get,
                               QStringLiteral("/api/files/") + fileId + QStringLiteral("/versions"));
    if (!r.ok())
        return Result<QVector<FileVersion>>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    return Result<QVector<FileVersion>>::success(
        Json::fromArray<FileVersion>(o.value(QStringLiteral("items")), &Json::versionFromJson));
}

Ok HttpBackend::restoreVersion(const QString &fileId, const QString &versionId)
{
    const Response r = request(Method::Post, QStringLiteral("/api/files/") + fileId
                                                 + QStringLiteral("/versions/") + versionId
                                                 + QStringLiteral("/restore"));
    return r.ok() ? ok() : err(r.error);
}

// ------------------------------ 分享 ------------------------------

Result<QVector<ShareLink>> HttpBackend::shares()
{
    const Response r = request(Method::Get, QStringLiteral("/api/shares"));
    if (!r.ok())
        return Result<QVector<ShareLink>>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    return Result<QVector<ShareLink>>::success(
        Json::fromArray<ShareLink>(o.value(QStringLiteral("items")), &Json::shareFromJson));
}

Result<ShareLink> HttpBackend::createShare(const QString &fileId, const QString &code,
                                           int expireDays, int maxDownloads)
{
    QJsonObject body;
    body.insert(QStringLiteral("fileId"), fileId);
    body.insert(QStringLiteral("code"), code);
    body.insert(QStringLiteral("expireDays"), expireDays);
    body.insert(QStringLiteral("maxDownloads"), maxDownloads);

    const Response r = request(Method::Post, QStringLiteral("/api/shares"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!r.ok())
        return Result<ShareLink>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    return Result<ShareLink>::success(Json::shareFromJson(o.value(QStringLiteral("item")).toObject()));
}

Ok HttpBackend::revokeShare(const QString &shareId)
{
    const Response r = request(Method::Post,
                               QStringLiteral("/api/shares/") + shareId + QStringLiteral("/revoke"));
    return r.ok() ? ok() : err(r.error);
}

Ok HttpBackend::touchShare(const QString &shareId)
{
    const Response r = request(Method::Post,
                               QStringLiteral("/api/shares/") + shareId + QStringLiteral("/touch"));
    return r.ok() ? ok() : err(r.error);
}

// ------------------------------ 标签与检索 ------------------------------

Result<QVector<Tag>> HttpBackend::tags()
{
    const Response r = request(Method::Get, QStringLiteral("/api/tags"));
    if (!r.ok())
        return Result<QVector<Tag>>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    return Result<QVector<Tag>>::success(
        Json::fromArray<Tag>(o.value(QStringLiteral("items")), &Json::tagFromJson));
}

Result<Tag> HttpBackend::createTag(const QString &name, const QString &color)
{
    QJsonObject body;
    body.insert(QStringLiteral("name"), name);
    body.insert(QStringLiteral("color"), color);

    const Response r = request(Method::Post, QStringLiteral("/api/tags"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!r.ok())
        return Result<Tag>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    return Result<Tag>::success(Json::tagFromJson(o.value(QStringLiteral("item")).toObject()));
}

Ok HttpBackend::deleteTag(const QString &tagId)
{
    const Response r = request(Method::Delete, QStringLiteral("/api/tags/") + tagId);
    return r.ok() ? ok() : err(r.error);
}

Result<QVector<FileItem>> HttpBackend::search(const QString &keyword, const QStringList &tagIds,
                                              int typeFilter)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("keyword"), keyword);
    q.addQueryItem(QStringLiteral("tags"), tagIds.join(QLatin1Char(',')));
    q.addQueryItem(QStringLiteral("type"), QString::number(typeFilter));

    const Response r = request(Method::Get, QStringLiteral("/api/search?") + q.toString());
    if (!r.ok())
        return Result<QVector<FileItem>>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    return Result<QVector<FileItem>>::success(
        Json::fromArray<FileItem>(o.value(QStringLiteral("items")), &Json::fileFromJson));
}

// ------------------------------ 统计 ------------------------------

Result<UsageStats> HttpBackend::usage()
{
    const Response r = request(Method::Get, QStringLiteral("/api/stats/usage"));
    if (!r.ok())
        return Result<UsageStats>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    return Result<UsageStats>::success(Json::usageFromJson(o.value(QStringLiteral("stats")).toObject()));
}

} // namespace cv

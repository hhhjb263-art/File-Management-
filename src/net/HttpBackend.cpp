/****************************************************************************
 * net/HttpBackend.cpp —— 远程私有云服务器（真实契约）
 *
 * 本文件对接**真实服务端**（server/src/app/main.cpp 的 server.route(...)）。
 * 端点清单（与路由一一对应，均以本类 baseUrl 为前缀）：
 *
 *   GET    /healthz                             探活（免鉴权）
 *   POST   /api/v1/files                        整文件上传；头 X-CV-Name / X-CV-Dir(百分号编码)；
 *                                               同名未覆盖 -> 409 {exists:true,...}
 *   GET    /api/v1/files                        列表 {total,items[{id,name,dir,size,hash,chunks,created_at}]}
 *   GET    /api/v1/files/:id                    元数据（扁平对象）
 *   GET    /api/v1/files/:id/content            下载；Range: bytes=start- / start-end / -N；206/416
 *   POST   /api/v1/files/new                    新建空文件 {dir,name}；同名 409
 *   POST   /api/v1/files/:id/rename             重命名 {name}；同名 409；同名幂等 200
 *   DELETE /api/v1/files/:id                    删除并回收空间 -> 200 {freed_bytes,...}
 *   POST   /api/v1/dirs                         创建目录 {path}（201 新建 / 200 幂等 / 400 / 403）
 *   GET    /api/v1/dirs                         已登记目录列表 {total,items:[path,...]}
 *   GET    /api/v1/tree                         嵌套文件树
 *   GET    /api/v1/download?path=dir/name       按路径下载（仅已记录文件）
 *   GET    /api/v1/storage                      磁盘空间 {free_bytes,total_bytes,...}
 *   POST   /api/v1/uploads/init                 分块会话 {name,size,chunk_size,hash,dir[,overwrite]}
 *                                               秒传 -> {done:true,file_id}；同名未覆盖 -> 409
 *   GET    /api/v1/uploads/:id                  续传查询 {uploaded:[seq...],received_bytes,chunk_size,...}
 *   PUT    /api/v1/uploads/:id/chunk/:seq       body=分块字节；头 X-Chunk-SHA256；幂等
 *   POST   /api/v1/uploads/:id/complete         合并；缺块 409{missing}；哈希不符 422{invalid}
 *   DELETE /api/v1/uploads/:id                  取消会话（204）
 *
 *   鉴权：Authorization: Bearer <token>（token 非空才加；服务端同时兼容 X-CV-Token）。
 *   除 /healthz 外全部接口强制鉴权，缺失/错误 -> 401。
 *
 * 与抽象接口的映射 / 取舍：
 *   1) parentId 语义 -> dir 路径字符串（服务端没有 ID 体系）：
 *        · parentId == "" / "root" / "/"  => dir = ""
 *        · 其它 parentId 直接当作 dir 相对路径（'/' 分隔、无前导 '/'）
 *        · 返回的 FileItem.parentId 反向填回同一约定（根 = kRootId，其余 = dir 路径）
 *        · 目录条目 id 采用合成标识 "dir:<相对路径>"（服务端目录无数字 id）
 *   2) 列表类接口：服务端 GET /api/v1/files 返回**全量**（最近 500 条），
 *      因此 listFolder / listUnderPath 在本地按 dir 过滤；目录条目由 GET /api/v1/dirs 合成。
 *   3) 服务端不支持的能力（回收站 / 版本 / 分享 / 标签 / 检索 / 用户体系）：
 *      一律 **不发网络请求**，返回带 [unsupported] 前缀的失败（isUnsupported 可识别）。
 *      ⚠ 唯一例外：「永久删除」语义的 purge() 映射到真实 DELETE /api/v1/files/:id
 *      （服务端 DELETE 即硬删除，等价 purge；软删除的 moveToTrash 归入 unsupported）。
 *   4) 同名冲突（409）统一返回带 [conflict] 前缀的失败，并附带 name/dir/file_id，
 *      供上层弹「是否覆盖」。覆盖开关：
 *        · 整文件上传：uploadWholeFile(..., overwrite=true) -> 头 X-CV-Overwrite: 1
 *        · 分块上传：   setUploadOverwrite(true)（Backend 加法式接口）-> init body "overwrite": true
 *      约定：只对随后的一次上传生效，用后由调用方复位（TransferManager 负责）。
 *      分块大小以 chunkSize() 为**唯一来源**（init 与上层 offset→seq 推导都用它），
 *      不读取 Settings::chunkSizeMB()，避免两端失配导致 putChunk 被服务端 400。
 *   5) TLS 自签名：TOFU 指纹固定（首次确认 -> 记住 SHA-256 -> 不一致则拒绝）。
 *      net 层不依赖 QtWidgets、不弹窗：确认动作由界面层通过 setTrustPrompt() 注入回调；
 *      未注入回调时一律 fail-closed（拒绝连接），绝不静默信任。指纹持久化在
 *      AppPaths::settingsFile()（settings.ini，IniFormat）的键 `pinnedFingerprint/<host:port>`；
 *      与 client/ 参考实现不共享该记录（存储文件不同），属预期行为。
 *   6) 服务端未提供的能力，一律不支持（见上），不做任何网络请求。
 ****************************************************************************/
#include "HttpBackend.h"

#include "Json.h"
#include "core/AppPaths.h"
#include "core/Crypto.h"
#include "core/Util.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutexLocker>
#include <QNetworkRequest>
#include <QSettings>
#include <QSet>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QThreadStorage>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <memory>

namespace cv {
namespace {

constexpr int kTimeoutMs = 60000; // 单请求超时（含大分块 PUT / Range 下载）

// 失败结果的可识别前缀（见 HttpBackend.h 的 static 判定函数）
const QString kUnsupportedPrefix = QStringLiteral("[unsupported] ");
const QString kConflictPrefix    = QStringLiteral("[conflict] ");
const QString kMissingPrefix     = QStringLiteral("[missing-chunks] ");
const QString kInvalidPrefix     = QStringLiteral("[invalid-chunks] ");
// 指纹与已固定记录不一致（疑似中间人）：控制器据此给用户可见警告（契约 §8.3）
const QString kPinMismatchPrefix = QStringLiteral("[pin-mismatch] ");

// 目录条目合成 id 前缀
const QString kDirIdPrefix = QStringLiteral("dir:");

// 分块上传块大小（字节）：以 HttpBackend::chunkSize() 为**唯一来源**。
// 5 MiB 落在服务端 init 允许区间 [64KiB, 64MiB] 内，两端一致、不会触发 400。
constexpr qint64 kUploadChunkBytes = 5 * 1024 * 1024;

// 服务端未提供该能力时的统一失败文案
QString unsupportedMsg(const QString &feature)
{
    return kUnsupportedPrefix + QStringLiteral("服务端未提供「%1」能力，未发起网络请求").arg(feature);
}

// RFC 3986 百分号编码（文件名 / 目录名可能含中文、空格、'#'、'?'、'/' 等）
QString pct(const QString &s)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(s));
}

// 把十六进制指纹按 2 字符一组用冒号分隔，便于人眼核对（AA:BB:CC:…）
QString formatFingerprint(const QByteArray &digest)
{
    const QString hex = QString::fromLatin1(digest.toHex()).toUpper();
    QString       out;
    out.reserve(hex.size() * 3 / 2);
    for (int i = 0; i < hex.size(); i += 2) {
        if (!out.isEmpty())
            out.append(QLatin1Char(':'));
        out.append(hex.mid(i, 2));
    }
    return out;
}

// parentId -> 服务端 dir 相对路径（'/' 分隔、无前导 '/'；根 = 空串）
QString dirFromParentId(const QString &parentId)
{
    QString p = parentId.trimmed();
    if (p.isEmpty() || p == kRootId || p == kRootPath)
        return QString();
    while (p.startsWith(QLatin1Char('/')))
        p.remove(0, 1);
    while (p.endsWith(QLatin1Char('/')))
        p.chop(1);
    return p;
}

// 规范成相对路径（去掉前导 / 尾随 '/'）
QString normalizeRelPath(const QString &path)
{
    QString s = path.trimmed();
    while (s.startsWith(QLatin1Char('/')))
        s.remove(0, 1);
    while (s.endsWith(QLatin1Char('/')))
        s.chop(1);
    return s;
}

// 相对 dir + 单段 name -> 规范相对路径
QString joinRel(const QString &dir, const QString &name)
{
    if (dir.isEmpty())
        return name;
    if (name.isEmpty())
        return dir;
    return dir + QLatin1Char('/') + name;
}

// 由 dir 反推 parentId（根 = kRootId）
QString parentIdForDir(const QString &dir)
{
    return dir.isEmpty() ? kRootId : dir;
}

// 由「文件 id 字符串」判断是否为合成目录 id
bool isDirId(const QString &id)
{
    return id.startsWith(kDirIdPrefix);
}

// 服务端文件对象 -> FileItem（GET /api/v1/files 的 items 元素）
FileItem fileItemFromServer(const QJsonObject &o)
{
    FileItem f;
    f.id       = QString::number(o.value(QStringLiteral("id")).toVariant().toLongLong());
    const QString dir = o.value(QStringLiteral("dir")).toString();
    f.parentId = parentIdForDir(dir);
    f.name     = o.value(QStringLiteral("name")).toString();
    f.path     = QLatin1Char('/') + joinRel(dir, f.name);
    f.isDir    = false;
    f.size     = o.value(QStringLiteral("size")).toVariant().toLongLong();
    f.hash     = o.value(QStringLiteral("hash")).toString();
    const qint64 createdMs = o.value(QStringLiteral("created_at")).toVariant().toLongLong();
    if (createdMs > 0) {
        QDateTime dt = QDateTime::fromMSecsSinceEpoch(createdMs); // 本地时间
        f.modified   = dt.toUTC();                                // 契约：UTC
    }
    f.version  = 1;
    f.type     = Util::fileTypeOf(f.name, false);
    return f;
}

// 上传/合并结果对象 -> FileItem（POST /api/v1/files、/uploads/:id/complete）
FileItem fileItemFromWriteResult(const QJsonObject &o)
{
    FileItem f;
    f.id       = QString::number(o.value(QStringLiteral("id")).toVariant().toLongLong());
    if (f.id == QStringLiteral("0"))
        f.id = QString::number(o.value(QStringLiteral("file_id")).toVariant().toLongLong());
    const QString dir = o.value(QStringLiteral("dir")).toString();
    f.parentId = parentIdForDir(dir);
    f.name     = o.value(QStringLiteral("name")).toString();
    f.path     = QLatin1Char('/') + joinRel(dir, f.name);
    f.isDir    = false;
    f.size     = o.value(QStringLiteral("size")).toVariant().toLongLong();
    f.hash     = o.value(QStringLiteral("hash")).toString();
    f.modified = QDateTime::currentDateTimeUtc();
    f.version  = 1;
    f.type     = Util::fileTypeOf(f.name, false);
    return f;
}

// 目录相对路径 -> FileItem（服务端目录无数字 id / 时间，采用合成标识）
FileItem folderItemFor(const QString &relDir)
{
    FileItem f;
    f.id      = kDirIdPrefix + relDir;
    const int slash = relDir.lastIndexOf(QLatin1Char('/'));
    f.name    = slash < 0 ? relDir : relDir.mid(slash + 1);
    f.parentId = parentIdForDir(slash < 0 ? QString() : relDir.left(slash));
    f.path    = QLatin1Char('/') + relDir;
    f.isDir   = true;
    f.type    = FileType::Folder;
    return f;
}

// HTTP 状态码 + 服务端 JSON 错误字段 -> 可识别失败文案
QString errorTextForStatus(int status, const QByteArray &body)
{
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    const QString     e = o.value(QStringLiteral("error")).toString();

    if (status == 409) {
        if (o.contains(QStringLiteral("missing"))) {
            QStringList l;
            const QJsonArray arr = o.value(QStringLiteral("missing")).toArray();
            for (const QJsonValue &v : arr)
                l << QString::number(v.toInt());
            return kMissingPrefix
                   + QStringLiteral("HTTP 409: 缺失分块 missing=[%1]").arg(l.join(QLatin1Char(',')));
        }
        if (o.value(QStringLiteral("exists")).toBool()) {
            return kConflictPrefix
                   + QStringLiteral("HTTP 409: %1; name=%2; dir=%3; file_id=%4")
                         .arg(e.isEmpty() ? QStringLiteral("name exists in target directory") : e,
                              o.value(QStringLiteral("name")).toString(),
                              o.value(QStringLiteral("dir")).toString(),
                              QString::number(
                                  o.value(QStringLiteral("file_id")).toVariant().toLongLong()));
        }
    }
    if (status == 422) {
        QStringList l;
        const QJsonArray arr = o.value(QStringLiteral("invalid")).toArray();
        for (const QJsonValue &v : arr)
            l << QString::number(v.toInt());
        const QString reason = o.value(QStringLiteral("reason")).toString();
        return kInvalidPrefix
               + QStringLiteral("HTTP 422: invalid=[%1]%2")
                     .arg(l.join(QLatin1Char(',')),
                          reason.isEmpty() ? QString() : QStringLiteral("; ") + reason);
    }

    const QString shown = e.isEmpty() ? QStringLiteral("请求失败") : e;
    return QStringLiteral("HTTP %1: %2").arg(status).arg(shown);
}

} // namespace

// =========================================================================
// 构造与基础配置
// =========================================================================

HttpBackend::HttpBackend(const QString &baseUrl, QObject *parent)
    : Backend(parent)
    , m_baseUrl(baseUrl.trimmed())
{
    while (m_baseUrl.endsWith(QLatin1Char('/')))
        m_baseUrl.chop(1);
}

void HttpBackend::setBaseUrl(const QString &url)
{
    m_baseUrl = url.trimmed();
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

// =========================================================================
// 底层 HTTP
// =========================================================================

HttpBackend::Response HttpBackend::request(Method method, const QString &path,
                                           const QByteArray &body,
                                           const QHash<QByteArray, QByteArray> &headers)
{
    // 同步语义（既有调用方 / 非 GUI 线程）：用**局部**事件循环把异步实现包成阻塞返回。
    // 传输与解析只有一份 —— 全在 requestAsync()，避免两条路径行为漂移。
    // ⚠️ 同步阻塞仅限调用方自己的线程；GUI 主线程的刷新类查询请改用 *Async 变体。
    Response   out;
    QEventLoop loop;
    requestAsync(method, path, body, headers, [&out, &loop](Response r) {
        out = r;
        loop.quit();
    });
    loop.exec();
    return out;
}

void HttpBackend::requestAsync(Method method, const QString &path, const QByteArray &body,
                               const QHash<QByteArray, QByteArray> &headers,
                               std::function<void(Response)> done)
{
    QNetworkRequest req((QUrl(m_baseUrl + path)));
    req.setRawHeader("Accept", "application/json");
    if (!m_token.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());

    bool hasContentType = false;
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        if (it.key().compare("Content-Type", Qt::CaseInsensitive) == 0)
            hasContentType = true;
    }
    if (!body.isEmpty() && !hasContentType)
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

    // 超时用 QTimer 驱动「回调失败 + abort reply」，绝不阻塞调用方。
    auto *timer = new QTimer(reply); // 父子归 reply：reply 删除即回收
    timer->setSingleShot(true);

    // 保证 done **恰好一次**（finished 与 timeout 竞争时只取先到者）。
    auto fired  = std::make_shared<bool>(false);
    auto finish = [reply, timer, fired, done](bool timedOut) {
        if (*fired)
            return;
        *fired = true;
        timer->stop();

        Response out;
        if (timedOut && !reply->isFinished()) {
            reply->abort();
            out.error = QStringLiteral("请求超时（%1 ms）").arg(kTimeoutMs);
        } else {
            out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            out.body   = reply->readAll();
            if (out.status >= 400) {
                out.error = errorTextForStatus(out.status, out.body);
            } else if (out.status <= 0) {
                const QVariant pin = reply->property("cvPinMismatch");
                if (pin.isValid()) {
                    // 指纹不一致：给出可识别原因（控制器会翻成中文警告，且不提供"继续"）
                    out.error = pin.toString();
                } else {
                    out.error = reply->error() != QNetworkReply::NoError
                                    ? reply->errorString()
                                    : QStringLiteral("网络错误（无响应）");
                }
            }
        }
        reply->deleteLater();
        done(out);
    };

    QObject::connect(reply, &QNetworkReply::finished, reply, [finish]() { finish(false); });
    QObject::connect(timer, &QTimer::timeout, reply, [finish]() { finish(true); });
    // HTTPS 自签名证书：在 sslErrors 里做 TOFU 指纹固定（纯 HTTP 不受影响）
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [this, reply](const QList<QSslError> &errors) {
                         applyCertPinning(reply, errors);
                     });

    timer->start(kTimeoutMs);
}

// =========================================================================
// TLS 自签名证书（TOFU 指纹固定）
// =========================================================================

void HttpBackend::applyCertPinning(QNetworkReply *reply, const QList<QSslError> &errors)
{
    const QUrl u = reply->url();
    if (u.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0)
        return; // 纯 HTTP 不受影响
    if (!m_trustSelfSigned)
        return; // 未开启：交给 Qt 标准证书链校验（自签名会被拒）

    // 证书来源：优先 sslConfiguration 的对端证书；为空则从错误列表取第一个非空证书
    QSslCertificate cert = reply->sslConfiguration().peerCertificate();
    if (cert.isNull()) {
        for (const QSslError &e : errors) {
            if (!e.certificate().isNull()) {
                cert = e.certificate();
                break;
            }
        }
    }
    if (cert.isNull()) {
        return; // fail-closed：拿不到证书就不放行（不调用 ignoreSslErrors）
    }

    const QByteArray digest = cert.digest(QCryptographicHash::Sha256);
    const QString    fp     = QString::fromLatin1(digest.toHex());

    int port = u.port();
    if (port < 0)
        port = 443;
    const QString key = QStringLiteral("%1:%2").arg(u.host()).arg(port);

    QSettings s(AppPaths::settingsFile(), QSettings::IniFormat);
    const QString recorded = s.value(QStringLiteral("pinnedFingerprint/") + key).toString();

    if (!recorded.isEmpty()) {
        if (recorded.compare(fp, Qt::CaseInsensitive) == 0)
            reply->ignoreSslErrors(); // 指纹一致：静默放行
        else
            // 指纹不一致：疑似中间人 -> 不 ignore（按证书错误失败），
            // 并把可识别原因挂到该 reply 上，最终错误文本会带 [pin-mismatch] 前缀
            reply->setProperty("cvPinMismatch",
                               kPinMismatchPrefix +
                                   QStringLiteral("服务器证书指纹与已固定记录不一致（%1）").arg(key));
        return;
    }

    // 首次连接该主机：防同主机并发请求重复弹确认框（待决策期间一律 fail-closed）
    static QMutex         pendingMutex;
    static QSet<QString>  pendingHosts;
    {
        QMutexLocker locker(&pendingMutex);
        if (pendingHosts.contains(key))
            return;
        pendingHosts.insert(key);
    }

    bool trust = false;
    if (m_trustPrompt) {
        const QString subject = cert.subjectDisplayName();
        const QString issuer  = cert.issuerDisplayName();
        const QString valid   = QStringLiteral("%1 ~ %2").arg(
            cert.effectiveDate().toString(Qt::ISODate),
            cert.expiryDate().toString(Qt::ISODate));
        trust = m_trustPrompt(key, formatFingerprint(digest), subject, issuer, valid);
    }

    {
        QMutexLocker locker(&pendingMutex);
        pendingHosts.remove(key);
    }

    if (trust) {
        s.setValue(QStringLiteral("pinnedFingerprint/") + key, fp);
        s.sync();
        reply->ignoreSslErrors(); // 信任并记住指纹
    }
    // trust==false（含无确认回调）：不 ignore -> 本次连接失败
}

QString HttpBackend::pinnedFingerprint(const QString &hostPort) const
{
    QSettings s(AppPaths::settingsFile(), QSettings::IniFormat);
    return s.value(QStringLiteral("pinnedFingerprint/") + hostPort).toString();
}

void HttpBackend::clearPinnedFingerprint(const QString &hostPort)
{
    QSettings s(AppPaths::settingsFile(), QSettings::IniFormat);
    s.remove(QStringLiteral("pinnedFingerprint/") + hostPort);
    s.sync();
}

// =========================================================================
// 失败结果可识别性
// =========================================================================

bool HttpBackend::isUnsupported(const QString &error) { return error.startsWith(kUnsupportedPrefix); }
bool HttpBackend::isNameConflict(const QString &error) { return error.startsWith(kConflictPrefix); }
bool HttpBackend::isMissingChunks(const QString &error) { return error.startsWith(kMissingPrefix); }
bool HttpBackend::isInvalidChunks(const QString &error) { return error.startsWith(kInvalidPrefix); }
bool HttpBackend::isPinMismatch(const QString &error) { return error.startsWith(kPinMismatchPrefix); }

// =========================================================================
// 分块大小记忆（offset -> seq 需要）
// =========================================================================

void HttpBackend::rememberChunkSize(const QString &uploadId, qint64 chunkSize)
{
    if (uploadId.isEmpty() || chunkSize <= 0)
        return;
    QMutexLocker locker(&m_chunkSizeMutex);
    m_uploadChunkSize.insert(uploadId, chunkSize);
}

void HttpBackend::forgetChunkSize(const QString &uploadId)
{
    QMutexLocker locker(&m_chunkSizeMutex);
    m_uploadChunkSize.remove(uploadId);
}

qint64 HttpBackend::chunkSizeFor(const QString &uploadId)
{
    {
        QMutexLocker locker(&m_chunkSizeMutex);
        auto it = m_uploadChunkSize.constFind(uploadId);
        if (it != m_uploadChunkSize.constEnd())
            return it.value();
    }
    // 未记住（如跨进程续传）：回查服务端会话
    const Response r = request(Method::Get, QStringLiteral("/api/v1/uploads/") + uploadId);
    if (!r.ok())
        return 0;
    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    const qint64 cs = o.value(QStringLiteral("chunk_size")).toVariant().toLongLong();
    rememberChunkSize(uploadId, cs);
    return cs;
}

// =========================================================================
// 账户（服务端无用户体系 -> 不支持；令牌由 setToken 直接设置）
// =========================================================================

Result<UserInfo> HttpBackend::login(const QString &, const QString &)
{
    return Result<UserInfo>::fail(unsupportedMsg(QStringLiteral("登录/用户体系")));
}

void HttpBackend::logout()
{
    // 服务端无会话，登出即清除本地令牌与用户信息（不发网络请求）
    m_token.clear();
    m_user = UserInfo();
}

UserInfo HttpBackend::currentUser() const { return m_user; }

// =========================================================================
// 列表（GET /api/v1/files 全量 + GET /api/v1/dirs 合成目录）
// =========================================================================

// 解析：由 files/dirs 两响应合成当前目录条目（同步与异步共用，单一来源）。
Result<QVector<FileItem>> HttpBackend::buildFolderItems(const QString &parentId,
                                                        const Response &files,
                                                        const Response &dirs)
{
    if (!files.ok())
        return Result<QVector<FileItem>>::fail(files.error);

    const QString dir = dirFromParentId(parentId);

    // 目录列表失败不致命：至少把文件返回（无目录条目）
    QSet<QString> dirsSet;
    if (dirs.ok()) {
        const QJsonArray arr =
            QJsonDocument::fromJson(dirs.body).object().value(QStringLiteral("items")).toArray();
        for (const QJsonValue &v : arr)
            dirsSet.insert(v.toString());
    }

    QVector<FileItem> out;
    const QJsonArray items =
        QJsonDocument::fromJson(files.body).object().value(QStringLiteral("items")).toArray();
    for (const QJsonValue &v : items) {
        const FileItem f = fileItemFromServer(v.toObject());
        if (f.parentId == parentIdForDir(dir))
            out.append(f);
    }
    // 目录条目：dir 的**直接**子目录
    for (const QString &d : dirsSet) {
        if (d.isEmpty() || d == dir)
            continue;
        const bool directChild = dir.isEmpty()
                                     ? d.indexOf(QLatin1Char('/')) < 0
                                     : (d.startsWith(dir + QLatin1Char('/'))
                                        && d.indexOf(QLatin1Char('/'), dir.size() + 1) < 0);
        if (directChild)
            out.append(folderItemFor(d));
    }

    std::sort(out.begin(), out.end(), Json::fileLess);
    return Result<QVector<FileItem>>::success(out);
}

Result<QVector<FileItem>> HttpBackend::listFolder(const QString &parentId)
{
    const Response fr = request(Method::Get, QStringLiteral("/api/v1/files"));
    if (!fr.ok())
        return Result<QVector<FileItem>>::fail(fr.error);

    const Response dr = request(Method::Get, QStringLiteral("/api/v1/dirs"));
    return buildFolderItems(parentId, fr, dr);
}

void HttpBackend::listFolderAsync(const QString &parentId,
                                  std::function<void(Result<QVector<FileItem>>)> done)
{
    // 与同步版同序：先取 files，成功后再取 dirs（目录失败不致命，交由 buildFolderItems 处理）。
    requestAsync(Method::Get, QStringLiteral("/api/v1/files"), {}, {},
                 [this, parentId, done](Response fr) {
                     if (!fr.ok()) {
                         done(Result<QVector<FileItem>>::fail(fr.error));
                         return;
                     }
                     requestAsync(Method::Get, QStringLiteral("/api/v1/dirs"), {}, {},
                                  [this, parentId, fr, done](Response dr) {
                                      done(buildFolderItems(parentId, fr, dr));
                                  });
                 });
}

Result<QVector<FileItem>> HttpBackend::listUnderPath(const QString &remotePath)
{
    const QString base = normalizeRelPath(remotePath);

    const Response fr = request(Method::Get, QStringLiteral("/api/v1/files"));
    if (!fr.ok())
        return Result<QVector<FileItem>>::fail(fr.error);

    const Response dr = request(Method::Get, QStringLiteral("/api/v1/dirs"));
    QSet<QString> dirs;
    if (dr.ok()) {
        const QJsonArray arr =
            QJsonDocument::fromJson(dr.body).object().value(QStringLiteral("items")).toArray();
        for (const QJsonValue &v : arr)
            dirs.insert(v.toString());
    }

    const auto underBase = [&base](const QString &rel) {
        if (base.isEmpty())
            return true;
        return rel == base || rel.startsWith(base + QLatin1Char('/'));
    };

    QVector<FileItem> out;
    const QJsonArray items =
        QJsonDocument::fromJson(fr.body).object().value(QStringLiteral("items")).toArray();
    for (const QJsonValue &v : items) {
        const QJsonObject o    = v.toObject();
        const QString     dir  = o.value(QStringLiteral("dir")).toString();
        if (underBase(dir))
            out.append(fileItemFromServer(o));
    }
    for (const QString &d : dirs) {
        if (d.isEmpty() || d == base)
            continue;
        if (underBase(d))
            out.append(folderItemFor(d));
    }

    std::sort(out.begin(), out.end(), Json::fileLess);
    return Result<QVector<FileItem>>::success(out);
}

Result<QVector<FileItem>> HttpBackend::listTrash()
{
    return Result<QVector<FileItem>>::fail(unsupportedMsg(QStringLiteral("回收站列表")));
}

// =========================================================================
// 元数据
// =========================================================================

Result<FileItem> HttpBackend::statById(const QString &id)
{
    if (isDirId(id)) {
        const QString rel = id.mid(kDirIdPrefix.size());
        const Response dr = request(Method::Get, QStringLiteral("/api/v1/dirs"));
        if (!dr.ok())
            return Result<FileItem>::fail(dr.error);
        const QJsonArray arr =
            QJsonDocument::fromJson(dr.body).object().value(QStringLiteral("items")).toArray();
        for (const QJsonValue &v : arr) {
            if (v.toString() == rel)
                return Result<FileItem>::success(folderItemFor(rel));
        }
        return Result<FileItem>::fail(QStringLiteral("目录不存在：%1").arg(rel));
    }

    const Response r = request(Method::Get, QStringLiteral("/api/v1/files/") + pct(id));
    if (!r.ok())
        return Result<FileItem>::fail(r.error);
    return Result<FileItem>::success(
        fileItemFromServer(QJsonDocument::fromJson(r.body).object()));
}

Result<FileItem> HttpBackend::statByPath(const QString &remotePath)
{
    const QString rel = normalizeRelPath(remotePath);
    if (rel.isEmpty())
        return Result<FileItem>::success(folderItemFor(QString())); // 根目录

    const Response fr = request(Method::Get, QStringLiteral("/api/v1/files"));
    if (!fr.ok())
        return Result<FileItem>::fail(fr.error);

    const QJsonArray items =
        QJsonDocument::fromJson(fr.body).object().value(QStringLiteral("items")).toArray();
    for (const QJsonValue &v : items) {
        const QJsonObject o   = v.toObject();
        const QString     dir = o.value(QStringLiteral("dir")).toString();
        if (joinRel(dir, o.value(QStringLiteral("name")).toString()) == rel)
            return Result<FileItem>::success(fileItemFromServer(o));
    }

    // 不是文件 -> 查目录
    const Response dr = request(Method::Get, QStringLiteral("/api/v1/dirs"));
    if (dr.ok()) {
        const QJsonArray arr =
            QJsonDocument::fromJson(dr.body).object().value(QStringLiteral("items")).toArray();
        for (const QJsonValue &dv : arr) {
            if (dv.toString() == rel)
                return Result<FileItem>::success(folderItemFor(rel));
        }
    }

    return Result<FileItem>::fail(QStringLiteral("路径不存在：%1").arg(remotePath));
}

// =========================================================================
// 目录
// =========================================================================

Result<FileItem> HttpBackend::createFolder(const QString &parentId, const QString &name)
{
    const QString dir  = dirFromParentId(parentId);
    const QString path = joinRel(dir, name);

    QJsonObject body;
    body.insert(QStringLiteral("path"), path);
    const Response r = request(Method::Post, QStringLiteral("/api/v1/dirs"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!r.ok())
        return Result<FileItem>::fail(r.error);

    // 服务端幂等：已存在返回 200 {exists:true}。显式 createFolder 语义下视作同名冲突。
    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    if (o.value(QStringLiteral("exists")).toBool())
        return Result<FileItem>::fail(QStringLiteral("同名文件夹已存在：%1").arg(path));

    return Result<FileItem>::success(folderItemFor(path));
}

Result<FileItem> HttpBackend::ensureFolderPath(const QString &remotePath)
{
    const QString rel = normalizeRelPath(remotePath);
    if (rel.isEmpty())
        return Result<FileItem>::success(folderItemFor(QString())); // 根目录恒定存在

    QJsonObject body;
    body.insert(QStringLiteral("path"), rel);
    const Response r = request(Method::Post, QStringLiteral("/api/v1/dirs"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!r.ok())
        return Result<FileItem>::fail(r.error);

    // 201 新建 / 200 幂等，均视为成功
    return Result<FileItem>::success(folderItemFor(rel));
}

Ok HttpBackend::rename(const QString &id, const QString &newName)
{
    if (isDirId(id))
        return err(unsupportedMsg(QStringLiteral("目录重命名")));

    QJsonObject body;
    body.insert(QStringLiteral("name"), newName);
    const Response r = request(Method::Post,
                               QStringLiteral("/api/v1/files/") + pct(id) + QStringLiteral("/rename"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    return r.ok() ? ok() : err(r.error);
}

Ok HttpBackend::moveToTrash(const QString &)
{
    return err(unsupportedMsg(QStringLiteral("回收站（软删除）")));
}

Ok HttpBackend::restore(const QString &)
{
    return err(unsupportedMsg(QStringLiteral("回收站恢复")));
}

Ok HttpBackend::purge(const QString &id)
{
    // 服务端 DELETE 即永久删除（等价 purge 语义）；仅「回收站」相关软删除不支持。
    if (isDirId(id))
        return err(unsupportedMsg(QStringLiteral("目录删除")));

    const Response r = request(Method::Delete, QStringLiteral("/api/v1/files/") + pct(id));
    return r.ok() ? ok() : err(r.error);
}

Ok HttpBackend::emptyTrash()
{
    return err(unsupportedMsg(QStringLiteral("回收站清空")));
}

Ok HttpBackend::setTags(const QString &, const QStringList &)
{
    return err(unsupportedMsg(QStringLiteral("标签")));
}

// =========================================================================
// 传输（分块 / 秒传 / 断点续传 / Range）
// =========================================================================

Result<UploadTicket> HttpBackend::beginUpload(const QString &parentId, const QString &name,
                                              qint64 size, const QString &sha256)
{
    const QString dir       = dirFromParentId(parentId);
    const qint64  chunkSize = this->chunkSize(); // 唯一来源（不再读 Settings）

    QJsonObject body;
    body.insert(QStringLiteral("name"), name);
    body.insert(QStringLiteral("size"), double(size));
    body.insert(QStringLiteral("chunk_size"), double(chunkSize));
    body.insert(QStringLiteral("hash"), sha256);
    body.insert(QStringLiteral("dir"), dir);
    // 同名覆盖：由调用方通过 setUploadOverwrite(true) 置真（只对本次生效）
    body.insert(QStringLiteral("overwrite"), m_uploadOverwrite.load());

    const Response r = request(Method::Post, QStringLiteral("/api/v1/uploads/init"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!r.ok())
        return Result<UploadTicket>::fail(r.error);
    if (r.status != 200)
        return Result<UploadTicket>::fail(
            QStringLiteral("HTTP %1: 会话初始化失败").arg(r.status));

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();

    // 秒传命中：服务端已有相同内容，无需再传分块
    if (o.value(QStringLiteral("done")).toBool()) {
        UploadTicket t;
        t.instant = true;
        t.uploadId.clear();
        t.received = o.value(QStringLiteral("received_bytes")).toVariant().toLongLong();
        t.fileId   = QString::number(o.value(QStringLiteral("file_id")).toVariant().toLongLong());
        return Result<UploadTicket>::success(t);
    }

    const QString uploadId = QString::number(o.value(QStringLiteral("upload_id")).toVariant().toLongLong());
    const qint64  srvChunk = o.value(QStringLiteral("chunk_size")).toVariant().toLongLong();
    rememberChunkSize(uploadId, srvChunk > 0 ? srvChunk : chunkSize);

    UploadTicket t;
    t.instant  = false;
    t.uploadId = uploadId;
    t.received = o.value(QStringLiteral("received_bytes")).toVariant().toLongLong();
    return Result<UploadTicket>::success(t);
}

Ok HttpBackend::putChunk(const QString &uploadId, qint64 offset, const QByteArray &data)
{
    if (uploadId.isEmpty())
        return err(QStringLiteral("无效的上传会话 id"));
    if (offset < 0)
        return err(QStringLiteral("offset 不能为负"));

    const qint64 chunkSize = chunkSizeFor(uploadId);
    if (chunkSize <= 0)
        return err(QStringLiteral("无法确定分块大小（upload_id=%1）").arg(uploadId));
    if (offset % chunkSize != 0)
        return err(QStringLiteral("offset(%1) 未按分块大小(%2)对齐").arg(offset).arg(chunkSize));

    const qint64 seq = offset / chunkSize;
    const QString sha = Crypto::sha256Hex(data); // 小写十六进制 SHA-256

    QHash<QByteArray, QByteArray> headers;
    headers.insert("Content-Type", "application/octet-stream");
    headers.insert("X-Chunk-SHA256", sha.toUtf8());

    const Response r =
        request(Method::Put,
                QStringLiteral("/api/v1/uploads/") + uploadId + QStringLiteral("/chunk/")
                    + QString::number(seq),
                data, headers);
    return r.ok() ? ok() : err(r.error);
}

Result<FileItem> HttpBackend::finishUpload(const QString &uploadId)
{
    const Response r =
        request(Method::Post, QStringLiteral("/api/v1/uploads/") + uploadId
                                  + QStringLiteral("/complete"));
    if (!r.ok())
        return Result<FileItem>::fail(r.error); // 409 -> [missing-chunks]；422 -> [invalid-chunks]

    forgetChunkSize(uploadId);
    return Result<FileItem>::success(
        fileItemFromWriteResult(QJsonDocument::fromJson(r.body).object()));
}

Ok HttpBackend::cancelUpload(const QString &uploadId)
{
    const Response r = request(Method::Delete, QStringLiteral("/api/v1/uploads/") + uploadId);
    forgetChunkSize(uploadId);
    return r.ok() ? ok() : err(r.error);
}

Result<QByteArray> HttpBackend::getRange(const QString &fileId, qint64 offset, qint64 length)
{
    if (isDirId(fileId))
        return Result<QByteArray>::fail(QStringLiteral("目录不支持按区间下载：%1").arg(fileId));
    if (offset < 0)
        return Result<QByteArray>::fail(QStringLiteral("offset 不能为负"));

    const QString range = length > 0
                              ? QStringLiteral("bytes=%1-%2").arg(offset).arg(offset + length - 1)
                              : QStringLiteral("bytes=%1-").arg(offset);

    QHash<QByteArray, QByteArray> headers;
    headers.insert("Range", range.toLatin1());

    const Response r = request(Method::Get,
                               QStringLiteral("/api/v1/files/") + pct(fileId)
                                   + QStringLiteral("/content"),
                               QByteArray(), headers);

    if (r.status == 416)
        return Result<QByteArray>::fail(QStringLiteral("请求范围无效（HTTP 416）：%1").arg(range));
    if (!r.ok())
        return Result<QByteArray>::fail(r.error);

    if (r.status == 200) {
        // 服务端未按 Range 返回（整文件）：本地切出所需区间
        const qint64 total = r.body.size();
        if (offset >= total)
            return Result<QByteArray>::success(QByteArray());
        if (length <= 0)
            return Result<QByteArray>::success(r.body.mid(qsizetype(offset)));
        return Result<QByteArray>::success(
            r.body.mid(qsizetype(offset), qsizetype(length)));
    }

    // 206：服务端已按区间返回
    return Result<QByteArray>::success(r.body);
}

Result<HttpBackend::UploadSessionInfo> HttpBackend::uploadSession(const QString &uploadId)
{
    const Response r = request(Method::Get, QStringLiteral("/api/v1/uploads/") + uploadId);
    if (!r.ok())
        return Result<UploadSessionInfo>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    UploadSessionInfo info;
    info.uploadId  = QString::number(o.value(QStringLiteral("upload_id")).toVariant().toLongLong());
    info.size      = o.value(QStringLiteral("size")).toVariant().toLongLong();
    info.chunkSize = o.value(QStringLiteral("chunk_size")).toVariant().toLongLong();
    info.received  = o.value(QStringLiteral("received_bytes")).toVariant().toLongLong();
    info.status    = o.value(QStringLiteral("status")).toString();
    const QJsonArray arr = o.value(QStringLiteral("uploaded")).toArray();
    for (const QJsonValue &v : arr)
        info.uploaded.append(v.toInt());

    if (info.chunkSize > 0)
        rememberChunkSize(uploadId, info.chunkSize);

    return Result<UploadSessionInfo>::success(info);
}

// =========================================================================
// 整文件上传 / 探活（附加能力）
// =========================================================================

Result<FileItem> HttpBackend::uploadWholeFile(const QString &parentId, const QString &name,
                                              const QByteArray &data, bool overwrite)
{
    const QString dir = dirFromParentId(parentId);

    // 覆盖：显式参数 或 setUploadOverwrite(true) 均生效
    const bool effectiveOverwrite = overwrite || m_uploadOverwrite.load();

    QHash<QByteArray, QByteArray> headers;
    headers.insert("Content-Type", "application/octet-stream");
    headers.insert("X-CV-Name", pct(name).toUtf8());
    if (!dir.isEmpty())
        headers.insert("X-CV-Dir", pct(dir).toUtf8());
    if (effectiveOverwrite)
        headers.insert("X-CV-Overwrite", "1");

    const Response r = request(Method::Post, QStringLiteral("/api/v1/files"), data, headers);
    if (!r.ok())
        return Result<FileItem>::fail(r.error); // 409 -> [conflict]

    return Result<FileItem>::success(
        fileItemFromWriteResult(QJsonDocument::fromJson(r.body).object()));
}

void HttpBackend::setUploadOverwrite(bool enabled) { m_uploadOverwrite.store(enabled); }

// 分块大小唯一来源：init 与上层 offset→seq 推导都以此为准
qint64 HttpBackend::chunkSize() const { return kUploadChunkBytes; }

Result<bool> HttpBackend::health()
{
    const Response r = request(Method::Get, QStringLiteral("/healthz"));
    if (!r.ok())
        return Result<bool>::fail(r.error.isEmpty() ? QStringLiteral("健康检查失败") : r.error);
    // 记录服务端版本（/healthz 返回 {"status":"ok","version":"x.y.z",...}），供 UI 展示
    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    m_serverVersion = o.value(QStringLiteral("version")).toString();
    return Result<bool>::success(true);
}

// =========================================================================
// 版本（服务端未实现 -> 不支持）
// =========================================================================

Result<QVector<FileVersion>> HttpBackend::versions(const QString &)
{
    return Result<QVector<FileVersion>>::fail(unsupportedMsg(QStringLiteral("文件版本")));
}

Ok HttpBackend::restoreVersion(const QString &, const QString &)
{
    return err(unsupportedMsg(QStringLiteral("版本回溯")));
}

// =========================================================================
// 分享（服务端未实现 -> 不支持）
// =========================================================================

Result<QVector<ShareLink>> HttpBackend::shares()
{
    return Result<QVector<ShareLink>>::fail(unsupportedMsg(QStringLiteral("分享链接")));
}

Result<ShareLink> HttpBackend::createShare(const QString &, const QString &, int, int)
{
    return Result<ShareLink>::fail(unsupportedMsg(QStringLiteral("分享链接")));
}

Ok HttpBackend::revokeShare(const QString &)
{
    return err(unsupportedMsg(QStringLiteral("分享链接")));
}

Ok HttpBackend::touchShare(const QString &)
{
    return err(unsupportedMsg(QStringLiteral("分享链接")));
}

// =========================================================================
// 标签与检索（服务端未实现 -> 不支持）
// =========================================================================

Result<QVector<Tag>> HttpBackend::tags()
{
    return Result<QVector<Tag>>::fail(unsupportedMsg(QStringLiteral("标签")));
}

Result<Tag> HttpBackend::createTag(const QString &, const QString &)
{
    return Result<Tag>::fail(unsupportedMsg(QStringLiteral("标签")));
}

Ok HttpBackend::deleteTag(const QString &)
{
    return err(unsupportedMsg(QStringLiteral("标签")));
}

Result<QVector<FileItem>> HttpBackend::search(const QString &, const QStringList &, int)
{
    return Result<QVector<FileItem>>::fail(unsupportedMsg(QStringLiteral("全文检索")));
}

// =========================================================================
// 统计（GET /api/v1/storage + 计数）
// =========================================================================

// 解析：由 storage/files/dirs 三响应合成 UsageStats（同步与异步共用，单一来源）。
Result<UsageStats> HttpBackend::buildUsage(const Response &storage, const Response &files,
                                           const Response &dirs)
{
    if (!storage.ok())
        return Result<UsageStats>::fail(storage.error);

    const QJsonObject so = QJsonDocument::fromJson(storage.body).object();
    UsageStats        s;
    s.total = so.value(QStringLiteral("total_bytes")).toVariant().toLongLong();
    const qint64 free = so.value(QStringLiteral("free_bytes")).toVariant().toLongLong();
    s.used = s.total > 0 && free >= 0 ? qMax<qint64>(0, s.total - free) : 0;

    // 计数（best-effort）：文件数 / 目录数
    if (files.ok())
        s.fileCount =
            QJsonDocument::fromJson(files.body).object().value(QStringLiteral("total")).toVariant().toLongLong();
    if (dirs.ok())
        s.folderCount =
            QJsonDocument::fromJson(dirs.body).object().value(QStringLiteral("total")).toVariant().toLongLong();

    // 服务端不提供 版本 / 回收站 / 分享 占用与分类明细，保持默认 0 / 空
    return Result<UsageStats>::success(s);
}

Result<UsageStats> HttpBackend::usage()
{
    const Response sr = request(Method::Get, QStringLiteral("/api/v1/storage"));
    if (!sr.ok())
        return Result<UsageStats>::fail(sr.error);

    const Response fr = request(Method::Get, QStringLiteral("/api/v1/files"));
    const Response dr = request(Method::Get, QStringLiteral("/api/v1/dirs"));
    return buildUsage(sr, fr, dr);
}

void HttpBackend::usageAsync(std::function<void(Result<UsageStats>)> done)
{
    // 与同步版同序：storage 失败即止；其后 files/dirs 为 best-effort 计数。
    requestAsync(Method::Get, QStringLiteral("/api/v1/storage"), {}, {}, [this, done](Response sr) {
        if (!sr.ok()) {
            done(Result<UsageStats>::fail(sr.error));
            return;
        }
        requestAsync(Method::Get, QStringLiteral("/api/v1/files"), {}, {},
                     [this, done, sr](Response fr) {
                         requestAsync(Method::Get, QStringLiteral("/api/v1/dirs"), {}, {},
                                      [this, done, sr, fr](Response dr) {
                                          done(buildUsage(sr, fr, dr));
                                      });
                     });
    });
}

} // namespace cv

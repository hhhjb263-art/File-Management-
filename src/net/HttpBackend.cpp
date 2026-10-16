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
#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QPointer>
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
constexpr int kPreviewTimeoutMs = 15000; // 预览取内容区间专用超时（短请求；不改 kTimeoutMs 语义）

// 失败结果的可识别前缀（见 HttpBackend.h 的 static 判定函数）
const QString kUnsupportedPrefix = QStringLiteral("[unsupported] ");
const QString kConflictPrefix    = QStringLiteral("[conflict] ");
const QString kMissingPrefix     = QStringLiteral("[missing-chunks] ");
const QString kInvalidPrefix     = QStringLiteral("[invalid-chunks] ");
// 指纹与已固定记录不一致（疑似中间人）：控制器据此给用户可见警告（契约 §8.3）
const QString kPinMismatchPrefix = QStringLiteral("[pin-mismatch] ");

// 鉴权 / 输入 / 限流错误的可识别前缀（供 AuthController 区分错误种类；加法式，不改 Result 签名）：
//   [bad-request](400) / [unauthorized](401) / [forbidden](403) / [too-many-requests](429)
// 其余状态码（含 409/422/5xx）保持既有风格；网络错误由 requestAsync 统一加 [network] 前缀。
const QString kBadRequestPrefix      = QStringLiteral("[bad-request] ");
const QString kUnauthorizedPrefix    = QStringLiteral("[unauthorized] ");
const QString kForbiddenPrefix       = QStringLiteral("[forbidden] ");
const QString kTooManyRequestsPrefix = QStringLiteral("[too-many-requests] ");
const QString kNetworkPrefix         = QStringLiteral("[network] ");

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
    // ⚠️ 必须先剥掉目录 id 的前缀（"dir:"）。调用方传进来的往往是**列表里的目录 id**
    //    （ensureFolderPath() 返回的正是 folderItemFor(rel) 的 id），而不是裸路径。
    //    漏这一步的后果极隐蔽：服务端 sanitizeRelPath 见到 ':' 会判成**盘符/绝对路径**，
    //    直接回 403 "path escapes allowed root: absolute path/drive letter not allowed"
    //    —— 现象是"双向同步传不上去"，而 MockBackend 用自己的内部 id 所以完全复现不出来。
    if (p.startsWith(kDirIdPrefix))
        p = p.mid(kDirIdPrefix.size());
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

// 回收站位点 body：id 是合成目录标识 "dir:<rel>" 时传 dir，否则传数字 file_id。
// 服务端刻意不用 URL 路径参数承接目录（http_server 的路径段不做百分号解码，
// 含中文/冒号的 "dir:照片/2026" 塞进路径永远比对不上）。
QByteArray trashTargetBody(const QString &id)
{
    QJsonObject o;
    if (isDirId(id)) {
        o.insert(QStringLiteral("dir"), id.mid(kDirIdPrefix.size()));
    } else {
        o.insert(QStringLiteral("file_id"), id.toLongLong());
    }
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// 服务端版本对象 -> FileVersion（GET /api/v1/files/:id/versions 的 items 元素）
FileVersion fileVersionFromServer(const QJsonObject &o)
{
    FileVersion v;
    v.id      = QString::number(o.value(QStringLiteral("id")).toVariant().toLongLong());
    v.fileId  = QString::number(o.value(QStringLiteral("file_id")).toVariant().toLongLong());
    v.version = o.value(QStringLiteral("version")).toInt(1);
    v.size    = o.value(QStringLiteral("size")).toVariant().toLongLong();
    const qint64 ms = o.value(QStringLiteral("created_at")).toVariant().toLongLong();
    if (ms > 0)
        v.created = QDateTime::fromMSecsSinceEpoch(ms).toUTC();
    v.device  = o.value(QStringLiteral("device")).toString();
    v.note    = o.value(QStringLiteral("note")).toString();
    v.current = o.value(QStringLiteral("current")).toBool(false);
    return v;
}

// 服务端标签对象 -> Tag
Tag tagFromServer(const QJsonObject &o)
{
    Tag t;
    t.id    = QString::number(o.value(QStringLiteral("id")).toVariant().toLongLong());
    t.name  = o.value(QStringLiteral("name")).toString();
    t.color = o.value(QStringLiteral("color")).toString();
    t.count = o.value(QStringLiteral("count")).toInt(0);
    return t;
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

    // 鉴权 / 输入 / 限流错误带可识别前缀（供上层区分错误种类，不改 Result 签名）。
    QString prefix;
    switch (status) {
    case 400: prefix = kBadRequestPrefix; break;
    case 401: prefix = kUnauthorizedPrefix; break;
    case 403: prefix = kForbiddenPrefix; break;
    case 429: prefix = kTooManyRequestsPrefix; break;
    default:  break;
    }

    const QString shown = e.isEmpty() ? QStringLiteral("请求失败") : e;
    const QString base  = QStringLiteral("HTTP %1: %2").arg(status).arg(shown);
    return prefix.isEmpty() ? base : (prefix + base);
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
    // 兼容重载：默认 60s（既有调用方语义不变）
    requestAsync(method, path, body, headers, std::move(done), kTimeoutMs);
}

void HttpBackend::requestAsync(Method method, const QString &path, const QByteArray &body,
                               const QHash<QByteArray, QByteArray> &headers,
                               std::function<void(Response)> done, int timeoutMs)
{
    // 幂等方法留 1 次重试余地（详见 requestAsyncOnce 里"陈旧连接"的说明）
    requestAsyncOnce(method, path, body, headers, std::move(done), timeoutMs, 1);
}

void HttpBackend::requestAsyncOnce(Method method, const QString &path, const QByteArray &body,
                                   const QHash<QByteArray, QByteArray> &headers,
                                   std::function<void(Response)> done, int timeoutMs,
                                   int retriesLeft)
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
    // 需要 this：内部要读 m_token 并在 401 时 emit unauthorized()（会话过期通知上层）。
    // this 的生命周期由本对象保证；reply 是它的子对象，回调不会晚于对象析构。
    // 是否允许在"连接被对端关闭"时重试一次：只对 GET / PUT。
    //   · GET 无副作用；PUT 在本 API 里只有分块上传（按 seq 定位），重复写同一块幂等；
    //   · POST / DELETE 可能非幂等（新建、删除），**不重试** —— 宁可报错也不能重复执行。
    const bool mayRetry = (method == Method::Get || method == Method::Put);
    auto finish = [this, method, path, body, headers, mayRetry, retriesLeft, reply, timer, fired,
                   done, timeoutMs](bool timedOut) {
        if (*fired)
            return;
        *fired = true;
        timer->stop();

        Response out;
        if (timedOut && !reply->isFinished()) {
            reply->abort();
            out.error = kNetworkPrefix + QStringLiteral("请求超时（%1 ms）").arg(timeoutMs);
        } else {
            out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            out.body   = reply->readAll();
            if (out.status >= 400) {
                out.error = errorTextForStatus(out.status, out.body);
                // 解析 Retry-After（仅 429 账号锁定时有意义；其余忽略）
                const QByteArray ra = reply->rawHeader("Retry-After");
                if (!ra.isEmpty()) {
                    bool ok = false;
                    const int v = ra.toInt(&ok);
                    if (ok)
                        out.retryAfter = v;
                }
            } else if (out.status <= 0) {
                // 陈旧 keep-alive：服务端空闲超时（30s）关掉了连接，而我们复用了那个刚被关掉的
                // socket ⇒ 本次请求以 RemoteHostClosedError 失败。**这不是真的断网**，
                // 重发一次即可（用户看到的现象正是"老是断开连接"）。
                if (!timedOut && reply->error() == QNetworkReply::RemoteHostClosedError &&
                    mayRetry && retriesLeft > 0) {
                    reply->deleteLater();
                    // 这里**不**调 done、**不** emit unauthorized：直接以少一次额度重发。
                    // done 仍保证恰好被调用一次（由重发那次的 finish 负责）。
                    requestAsyncOnce(method, path, body, headers, done, timeoutMs,
                                     retriesLeft - 1);
                    return;
                }
                const QVariant pin = reply->property("cvPinMismatch");
                if (pin.isValid()) {
                    // 指纹不一致：给出可识别原因（控制器会翻成中文警告，且不提供"继续"）
                    out.error = pin.toString();
                } else {
                    out.error = kNetworkPrefix
                                + (reply->error() != QNetworkReply::NoError
                                       ? reply->errorString()
                                       : QStringLiteral("网络错误（无响应）"));
                }
            }
        }
        // 会话过期：持有令牌却收到 401 → 通知上层清登录态（登录自身无令牌，不会触发）。
        if (out.status == 401 && !m_token.isEmpty())
            emit unauthorized();
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

    timer->start(timeoutMs);
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
        else {
            // 指纹不一致：疑似中间人 -> 不 ignore（按证书错误失败），
            // 并把可识别原因挂到该 reply 上，最终错误文本会带 [pin-mismatch] 前缀
            reply->setProperty("cvPinMismatch",
                               kPinMismatchPrefix +
                                   QStringLiteral("服务器证书指纹与已固定记录不一致（%1）").arg(key));
            // 额外：把这件事**主动通知界面**（QML 只能经由控制器收到）。
            // 此前这里是静默失败——用户既不知道发生了什么，也不知道可以「清除指纹」恢复，
            // 表现为"换了服务端证书后永久连不上"。展示串取法与下方首次确认分支保持一致。
            // ⚠️ 仍然不放行：不调用 ignoreSslErrors()，本函数随后 return。
            emit certPinMismatch(key, recorded, fp, cert.subjectDisplayName(),
                                 cert.issuerDisplayName(),
                                 QStringLiteral("%1 ~ %2").arg(
                                     cert.effectiveDate().toString(Qt::ISODate),
                                     cert.expiryDate().toString(Qt::ISODate)));
        }
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
// 账户（契约 POST/GET /api/v1/auth/*）
// =========================================================================

namespace {
// 把登录 / 注册 / me 的响应 JSON 解析为 AuthUser
cv::HttpBackend::AuthUser authUserFromObject(const QJsonObject &o)
{
    cv::HttpBackend::AuthUser u;
    u.id          = QString::number(o.value(QStringLiteral("id")).toVariant().toLongLong());
    u.username    = o.value(QStringLiteral("username")).toString();
    u.displayName = o.value(QStringLiteral("display_name")).toString();
    u.createdAt   = o.value(QStringLiteral("created_at")).toString();
    u.lastLoginAt = o.value(QStringLiteral("last_login_at")).toString();
    return u;
}
} // namespace

int HttpBackend::retryAfterSeconds(const QByteArray &header)
{
    bool ok = false;
    const int v = QByteArray(header).trimmed().toInt(&ok);
    return ok ? v : -1;
}

HttpBackend::AuthResult HttpBackend::authLogin(const QString &user, const QString &password,
                                               const QString &userAgent)
{
    QJsonObject body;
    body.insert(QStringLiteral("username"), user);
    body.insert(QStringLiteral("password"), password);
    if (!userAgent.isEmpty())
        body.insert(QStringLiteral("user_agent"), userAgent);

    const Response r = request(Method::Post, QStringLiteral("/api/v1/auth/login"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    AuthResult res;
    res.retryAfter = r.retryAfter;
    if (!r.ok()) {
        res.error = r.error;
        return res;
    }
    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    res.session.token     = o.value(QStringLiteral("token")).toString();
    res.session.expiresAt = o.value(QStringLiteral("expires_at")).toString();
    res.session.user      = authUserFromObject(o.value(QStringLiteral("user")).toObject());
    res.ok                = true;
    // 同步更新后端内存态（currentUser / 令牌统一来源）
    m_user  = res.session.user.toUserInfo();
    m_token = res.session.token;
    return res;
}

HttpBackend::AuthResult HttpBackend::authRegister(const QString &user, const QString &password,
                                                  const QString &displayName)
{
    QJsonObject body;
    body.insert(QStringLiteral("username"), user);
    body.insert(QStringLiteral("password"), password);
    if (!displayName.isEmpty())
        body.insert(QStringLiteral("display_name"), displayName);

    const Response r = request(Method::Post, QStringLiteral("/api/v1/auth/register"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    AuthResult res;
    res.retryAfter = r.retryAfter;
    if (!r.ok()) {
        res.error = r.error;
        return res;
    }
    // 201 {id,username,display_name}：注册成功，不直接登录（注册无令牌返回）。
    res.ok = true;
    return res;
}

HttpBackend::AuthResult HttpBackend::authMe()
{
    const Response r = request(Method::Get, QStringLiteral("/api/v1/auth/me"));
    AuthResult res;
    res.retryAfter = r.retryAfter;
    if (!r.ok()) {
        res.error = r.error;
        return res;
    }
    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    res.session.user = authUserFromObject(o);
    res.ok           = true;
    m_user           = res.session.user.toUserInfo();
    return res;
}

void HttpBackend::authLogout()
{
    // best-effort：带上当前令牌发登出请求，忽略结果（失败也不影响本地清态）。
    if (!m_token.isEmpty())
        request(Method::Post, QStringLiteral("/api/v1/auth/logout"));
}

Result<UserInfo> HttpBackend::login(const QString &user, const QString &password)
{
    const AuthResult r = authLogin(user, password);
    if (!r.ok)
        return Result<UserInfo>::fail(r.error);
    return Result<UserInfo>::success(r.session.user.toUserInfo());
}

void HttpBackend::logout()
{
    authLogout();
    m_user = UserInfo(); // 仅清内存用户；令牌清空由 AppController/AuthController 统一负责
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

// 文件列表分页常量：与服务端 kDefaultFileListLimit/kMaxFileListLimit 对应。
constexpr int kFileListPageSize = 1000;
// 安全上限：防止服务端 hasMore 恒真时无限翻页（1000*200 = 20 万条，够覆盖 PRD 的 10 万文件目标）
constexpr int kMaxFileListPages = 200;

// 把累加好的 items 重新包成一个 Response，复用既有的 buildFolderItems
HttpBackend::Response HttpBackend::mergedFilesResponse(const QJsonArray &items)
{
    QJsonObject o;
    o.insert(QStringLiteral("total"), items.size());
    o.insert(QStringLiteral("items"), items);
    Response r;
    r.status = 200;
    r.body   = QJsonDocument(o).toJson(QJsonDocument::Compact);
    return r;
}

HttpBackend::Response HttpBackend::fetchFilePage(const QString &extraQuery, int limit, int offset)
{
    QString path = QStringLiteral("/api/v1/files");
    QString q    = extraQuery;
    if (!q.isEmpty())
        q += QLatin1Char('&');
    q += QStringLiteral("limit=%1&offset=%2").arg(limit).arg(offset);
    return request(Method::Get, path + QLatin1Char('?') + q);
}

Result<QJsonArray> HttpBackend::fetchAllFilePages(const QString &extraQuery)
{
    QJsonArray all;
    int        offset = 0;
    for (int page = 0; page < kMaxFileListPages; ++page) {
        const Response r = fetchFilePage(extraQuery, kFileListPageSize, offset);
        if (!r.ok())
            return Result<QJsonArray>::fail(r.error);
        const QJsonObject o     = QJsonDocument::fromJson(r.body).object();
        const QJsonArray  items = o.value(QStringLiteral("items")).toArray();
        for (const QJsonValue &v : items)
            all.append(v);
        // 老服务端没有 hasMore 字段 → toBool(false) → 视为"只有一页"，保持向后兼容
        if (!o.value(QStringLiteral("hasMore")).toBool(false) || items.isEmpty())
            break;
        offset += items.size();
    }
    return Result<QJsonArray>::success(all);
}

Result<QVector<FileItem>> HttpBackend::listFolder(const QString &parentId)
{
    // 改为**服务端按目录过滤 + 翻页**，不再"拉全表回本地筛"。
    // dir= 的**空值就是根目录**，所以必须显式带上这个参数（不能靠省略它来表达根目录）。
    const QString dir = dirFromParentId(parentId);
    const Result<QJsonArray> all =
        fetchAllFilePages(QStringLiteral("dir=%1").arg(pct(dir)));
    if (!all.ok)
        return Result<QVector<FileItem>>::fail(all.error);

    const Response dr = request(Method::Get, QStringLiteral("/api/v1/dirs"));
    return buildFolderItems(parentId, mergedFilesResponse(all.value), dr);
}

void HttpBackend::listFolderAsync(const QString &parentId,
                                  std::function<void(Result<QVector<FileItem>>)> done)
{
    // 本变体是界面实际走的路径（避免同步调用阻塞 GUI），所以它同样必须翻页 ——
    // 只修同步版等于什么都没修。
    //
    // 实现上用"首包拿到 total → 并发取其余页 → 按序拼装"，而不是递归回调：
    // 递归需要让闭包捕获它自己（shared_ptr 自引用成环），每次调用都会泄漏整份累加结果，
    // 也要在"执行中销毁自身"的边界上走钢丝。分页并发则没有这些问题。
    const QString dir   = dirFromParentId(parentId);
    const QString query = QStringLiteral("dir=%1").arg(pct(dir));

    struct Acc
    {
        QJsonArray items;
    };
    auto                  acc = std::make_shared<Acc>();
    QPointer<HttpBackend> self(this);

    const auto finish = [self, parentId, acc, done]() {
        if (!self) {
            done(Result<QVector<FileItem>>::fail(QStringLiteral("连接已释放")));
            return;
        }
        self->requestAsync(Method::Get, QStringLiteral("/api/v1/dirs"), {}, {},
                           [self, parentId, acc, done](Response dr) {
            if (!self) {
                done(Result<QVector<FileItem>>::fail(QStringLiteral("连接已释放")));
                return;
            }
            done(self->buildFolderItems(parentId, mergedFilesResponse(acc->items), dr));
        });
    };

    const QString first = QStringLiteral("/api/v1/files?%1&limit=%2&offset=0")
                              .arg(query)
                              .arg(kFileListPageSize);
    requestAsync(Method::Get, first, {}, {}, [self, query, acc, finish, done](Response fr) {
        if (!self)
            return;   // done 由 finish 负责；连接已释放时不再回调（调用方同期已销毁）
        if (!fr.ok()) {
            done(Result<QVector<FileItem>>::fail(fr.error));
            return;
        }
        const QJsonObject o     = QJsonDocument::fromJson(fr.body).object();
        const QJsonArray  items = o.value(QStringLiteral("items")).toArray();
        for (const QJsonValue &v : items)
            acc->items.append(v);

        const qint64 total = static_cast<qint64>(o.value(QStringLiteral("total")).toDouble());
        int          pages = 1;
        if (total > 0)
            pages = static_cast<int>((total + kFileListPageSize - 1) / kFileListPageSize);
        pages = qBound(1, pages, kMaxFileListPages);

        if (pages <= 1) {
            finish();
            return;
        }

        // 其余页并发取；按索引落位，最后按序拼装（顺序不能乱：界面/同步都依赖稳定顺序）
        // 变量名不能叫 slots：它是 Qt 的关键字宏（展开为空），会直接造成语法错误
        auto pageItems = std::make_shared<QVector<QJsonArray>>(pages);
        auto remaining = std::make_shared<int>(pages - 1);
        auto failed    = std::make_shared<bool>(false);
        (*pageItems)[0] = items;

        for (int p = 1; p < pages; ++p) {
            const QString path = QStringLiteral("/api/v1/files?%1&limit=%2&offset=%3")
                                     .arg(query)
                                     .arg(kFileListPageSize)
                                     .arg(p * kFileListPageSize);
            // 本 lambda 捕获的是 self（QPointer）而不是 this，必须显式走 self->
            self->requestAsync(Method::Get, path, {}, {},
                               [self, pageItems, remaining, failed, p, finish, done](Response r) {
                if (!self)
                    return;
                if (r.ok()) {
                    (*pageItems)[p] = QJsonDocument::fromJson(r.body).object()
                                      .value(QStringLiteral("items"))
                                      .toArray();
                } else {
                    // 任何一页失败都算整体失败：宁可报错，也不要"少几个文件"的静默残缺
                    *failed = true;
                }
                if (--(*remaining) == 0) {
                    if (*failed) {
                        done(Result<QVector<FileItem>>::fail(
                            QStringLiteral("分页拉取文件列表时有页失败")));
                        return;
                    }
                    QJsonArray all;
                    for (const QJsonArray &a : *pageItems) {
                        for (const QJsonValue &v : a)
                            all.append(v);
                    }
                    pageItems->clear();
                    // 交给 finish 去拉目录并组装
                    finish();
                }
            });
        }
    });
}

Result<QVector<FileItem>> HttpBackend::listUnderPath(const QString &remotePath)
{
    const QString base = normalizeRelPath(remotePath);

    // 用服务端 under= 前缀过滤 + 翻页，替代"拉全表再本地按前缀筛"。
    // ⚠️ 同步模块靠本函数拿**全量**云端树，漏一页就等于漏同步 —— 所以务必翻完所有页。
    const Result<QJsonArray> all =
        base.isEmpty() ? fetchAllFilePages(QString())
                       : fetchAllFilePages(QStringLiteral("under=%1").arg(pct(base)));
    if (!all.ok)
        return Result<QVector<FileItem>>::fail(all.error);

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
    for (const QJsonValue &v : all.value) {
        const QJsonObject o    = v.toObject();
        const QString     dir  = o.value(QStringLiteral("dir")).toString();
        if (underBase(dir))   // 服务端已过滤；本地再校验一遍，防老服务端/异常返回
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
    const Response r = request(Method::Get, QStringLiteral("/api/v1/trash"));
    if (!r.ok())
        return Result<QVector<FileItem>>::fail(r.error);

    const QJsonObject root = QJsonDocument::fromJson(r.body).object();
    QVector<FileItem> out;
    // 文件条目（顶层：所属目录不在回收站）
    for (const QJsonValue &v : root.value(QStringLiteral("items")).toArray()) {
        const QJsonObject o = v.toObject();
        FileItem f = fileItemFromServer(o);
        f.trashed  = true;
        const qint64 ms = o.value(QStringLiteral("deleted_at")).toVariant().toLongLong();
        if (ms > 0)
            f.trashedAt = QDateTime::fromMSecsSinceEpoch(ms).toUTC();
        out.append(f);
    }
    // 目录条目（顶层：父目录不在回收站）—— 服务端只回顶层，界面不该看到"散开的 N 个文件"
    for (const QJsonValue &v : root.value(QStringLiteral("dirs")).toArray()) {
        const QJsonObject o = v.toObject();
        FileItem f = folderItemFor(o.value(QStringLiteral("path")).toString());
        f.trashed  = true;
        f.size     = o.value(QStringLiteral("size")).toVariant().toLongLong();
        const qint64 ms = o.value(QStringLiteral("deleted_at")).toVariant().toLongLong();
        if (ms > 0)
            f.trashedAt = QDateTime::fromMSecsSinceEpoch(ms).toUTC();
        out.append(f);
    }
    return Result<QVector<FileItem>>::success(out);
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

Ok HttpBackend::moveFile(const QString &id, const QString &newParentId)
{
    // 服务端按**目录路径**接收（与上传 init 的 dir 同一套口径）。
    // ⚠️ 这里要同时接受两种传法：
    //   · 目录在列表里的 id（folderItemFor 生成的 <前缀>+<路径>）—— 界面自然拿到的是这个；
    //   · 裸相对路径（'' 或 'docs/2026'）—— 直接调 API 时更顺手。
    //   两个后端对"目录 id"的口径本来就不一样（Mock 是内部 id），统一到"传列表里的 id 即可"
    //   才算对外一致；否则调用方得知道自己接的是哪个后端，那等于没抽象。
    QJsonObject body;
    body.insert(QStringLiteral("dir"), dirFromParentId(newParentId));
    const Response r = request(Method::Post,
                               QStringLiteral("/api/v1/files/") + id + QStringLiteral("/move"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    return r.ok() ? ok() : err(r.error);
}

Ok HttpBackend::moveDir(const QString &from, const QString &to)
{
    QJsonObject body;
    body.insert(QStringLiteral("from"), normalizeRelPath(from));
    body.insert(QStringLiteral("to"), normalizeRelPath(to));
    const Response r = request(Method::Post, QStringLiteral("/api/v1/dirs/move"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    return r.ok() ? ok() : err(r.error);
}

Result<QPair<int, QString>> HttpBackend::moveToTrashBatch(const QStringList &ids)
{
    // 一次 POST 处理 N 个 id：N 次往返压成 1 次（多选删除慢的根因就是 N 次串行 DELETE）。
    QJsonArray arr;
    for (const QString &id : ids)
        arr.append(id.toDouble());
    QJsonObject body;
    body.insert(QStringLiteral("ids"), arr);

    const Response r = request(Method::Post, QStringLiteral("/api/v1/files/trash"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!r.ok())
        return Result<QPair<int, QString>>::fail(r.error);

    const QJsonObject o   = QJsonDocument::fromJson(r.body).object();
    const int         ok  = o.value(QStringLiteral("ok_count")).toInt();
    const int         bad = o.value(QStringLiteral("failed_count")).toInt();
    QString           err;
    if (bad > 0)
        err = QStringLiteral("%1 项失败（可能已被删除或无权限）").arg(bad);
    return Result<QPair<int, QString>>::success(qMakePair(ok, err));
}

Result<QPair<int, QString>> HttpBackend::moveFileBatch(const QStringList &ids,
                                                       const QString &targetDir)
{
    QJsonArray arr;
    for (const QString &id : ids)
        arr.append(id.toDouble());
    QJsonObject body;
    body.insert(QStringLiteral("ids"), arr);
    // 目标目录用**相对路径**（与单条接口一致；空串 = 根）
    body.insert(QStringLiteral("dir"), targetDir);

    const Response r = request(Method::Post, QStringLiteral("/api/v1/files/move"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!r.ok())
        return Result<QPair<int, QString>>::fail(r.error);

    const QJsonObject o  = QJsonDocument::fromJson(r.body).object();
    const int         ok = o.value(QStringLiteral("ok_count")).toInt();
    const int         bad = o.value(QStringLiteral("failed_count")).toInt();
    QString           err;
    if (bad > 0)
        err = QStringLiteral("%1 项失败（可能是同名冲突或已被移走）").arg(bad);
    return Result<QPair<int, QString>>::success(qMakePair(ok, err));
}

Result<ScanVerdict> HttpBackend::scanFile(const QString &id)
{
    // 服务端是**同步**查远端（VT 每次最多等 scan-timeout-ms），所以这个请求可能比普通请求慢；
    // 走同一个 request()（60s 超时）即可，调用方已用异步壳包起来，不会卡界面。
    const Response r = request(Method::Post,
                               QStringLiteral("/api/v1/files/") + id + QStringLiteral("/scan"),
                               QByteArray("{}"));
    if (!r.ok())
        return Result<ScanVerdict>::fail(r.error);

    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    ScanVerdict       v;
    v.verdict           = o.value(QStringLiteral("verdict")).toString();
    v.detail            = o.value(QStringLiteral("detail")).toString();
    v.enginesMalicious  = int(o.value(QStringLiteral("engines_malicious")).toDouble());
    v.enginesTotal      = int(o.value(QStringLiteral("engines_total")).toDouble());
    v.fromCache         = o.value(QStringLiteral("from_cache")).toBool();
    v.remoteFailed      = o.value(QStringLiteral("remote_failed")).toBool();
    v.blocked           = o.value(QStringLiteral("blocked")).toBool();
    return Result<ScanVerdict>::success(v);
}

Ok HttpBackend::moveToTrash(const QString &id)
{
    // 软删除（进回收站）：POST /api/v1/trash，body 二选一 —— {file_id} 或 {dir}
    const Response r = request(Method::Post, QStringLiteral("/api/v1/trash"), trashTargetBody(id));
    return r.ok() ? ok() : err(r.error);
}

Ok HttpBackend::restore(const QString &id)
{
    // 恢复：原目录若也在回收站，服务端会顺带把目录链恢复（否则"恢复成功却看不见"）
    const Response r =
        request(Method::Post, QStringLiteral("/api/v1/trash/restore"), trashTargetBody(id));
    return r.ok() ? ok() : err(r.error);
}

Ok HttpBackend::purge(const QString &id)
{
    // 彻底删除（不可恢复）：POST /api/v1/trash/purge。
    // 注意与旧语义的区别：早先本方法映射到 DELETE /api/v1/files/:id，而那个端点现在
    // 已改为"移入回收站"（配合回收站落地）。真正的永久删除只有这一条路径。
    const Response r =
        request(Method::Post, QStringLiteral("/api/v1/trash/purge"), trashTargetBody(id));
    return r.ok() ? ok() : err(r.error);
}

Ok HttpBackend::emptyTrash()
{
    const Response r = request(Method::Post, QStringLiteral("/api/v1/trash/empty"),
                               QByteArrayLiteral("{}"));
    return r.ok() ? ok() : err(r.error);
}

Ok HttpBackend::setTags(const QString &id, const QStringList &tagIds)
{
    if (isDirId(id))
        return err(QStringLiteral("目录暂不支持标签"));

    // 全量替换语义：服务端先清空该文件的全部关联，再按 tag_ids 重建
    QJsonArray arr;
    for (const QString &t : tagIds)
        arr.append(t.toLongLong());
    QJsonObject body;
    body.insert(QStringLiteral("tag_ids"), arr);

    const Response r = request(Method::Put,
                               QStringLiteral("/api/v1/files/") + pct(id) + QStringLiteral("/tags"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    return r.ok() ? ok() : err(r.error);
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

QByteArray HttpBackend::rangeHeader(qint64 offset, qint64 length)
{
    return (length > 0 ? QStringLiteral("bytes=%1-%2").arg(offset).arg(offset + length - 1)
                       : QStringLiteral("bytes=%1-").arg(offset))
        .toLatin1();
}

Result<QByteArray> HttpBackend::parseRangeBody(const Response &r, qint64 offset, qint64 length)
{
    if (r.status == 416)
        return Result<QByteArray>::fail(QStringLiteral("请求范围无效（HTTP 416）：%1")
                                            .arg(QString::fromLatin1(rangeHeader(offset, length))));
    if (!r.ok())
        return Result<QByteArray>::fail(r.error);

    if (r.status == 200) {
        // 服务端未按 Range 返回（整文件）：本地切出所需区间
        const qint64 total = r.body.size();
        if (offset >= total)
            return Result<QByteArray>::success(QByteArray());
        if (length <= 0)
            return Result<QByteArray>::success(r.body.mid(qsizetype(offset)));
        return Result<QByteArray>::success(r.body.mid(qsizetype(offset), qsizetype(length)));
    }

    // 206：服务端已按区间返回
    return Result<QByteArray>::success(r.body);
}

Result<QByteArray> HttpBackend::getRange(const QString &fileId, qint64 offset, qint64 length)
{
    if (isDirId(fileId))
        return Result<QByteArray>::fail(QStringLiteral("目录不支持按区间下载：%1").arg(fileId));
    if (offset < 0)
        return Result<QByteArray>::fail(QStringLiteral("offset 不能为负"));

    QHash<QByteArray, QByteArray> headers;
    headers.insert("Range", rangeHeader(offset, length));

    const Response r = request(Method::Get,
                               QStringLiteral("/api/v1/files/") + pct(fileId)
                                   + QStringLiteral("/content"),
                               QByteArray(), headers);
    return parseRangeBody(r, offset, length);
}

void HttpBackend::getRangeAsync(const QString &fileId, qint64 offset, qint64 length,
                                std::function<void(Result<QByteArray>)> done)
{
    // 与同步版一致的入参校验（不发请求）
    if (isDirId(fileId)) {
        done(Result<QByteArray>::fail(QStringLiteral("目录不支持按区间下载：%1").arg(fileId)));
        return;
    }
    if (offset < 0) {
        done(Result<QByteArray>::fail(QStringLiteral("offset 不能为负")));
        return;
    }

    QHash<QByteArray, QByteArray> headers;
    headers.insert("Range", rangeHeader(offset, length));

    requestAsync(Method::Get,
                 QStringLiteral("/api/v1/files/") + pct(fileId) + QStringLiteral("/content"),
                 QByteArray(), headers,
                 [offset, length, done](Response r) { done(parseRangeBody(r, offset, length)); },
                 kPreviewTimeoutMs); // 预览短请求：15s，不改默认 60s 语义
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
// 版本（GET/POST /api/v1/files/:id/versions、POST .../versions/:vid/restore）
//   服务端 file_version 只存历史快照，"当前版本"由服务端合成后一并返回（current=true）。
// =========================================================================

Result<QVector<FileVersion>> HttpBackend::versions(const QString &fileId)
{
    if (isDirId(fileId))
        return Result<QVector<FileVersion>>::fail(QStringLiteral("目录没有历史版本"));

    const Response r = request(Method::Get, QStringLiteral("/api/v1/files/") + pct(fileId)
                                               + QStringLiteral("/versions"));
    if (!r.ok())
        return Result<QVector<FileVersion>>::fail(r.error);

    const QJsonArray arr =
        QJsonDocument::fromJson(r.body).object().value(QStringLiteral("items")).toArray();
    QVector<FileVersion> out;
    for (const QJsonValue &v : arr)
        out.append(fileVersionFromServer(v.toObject()));
    return Result<QVector<FileVersion>>::success(out);
}

Ok HttpBackend::restoreVersion(const QString &fileId, const QString &versionId)
{
    if (isDirId(fileId))
        return err(QStringLiteral("目录没有历史版本"));
    if (versionId.isEmpty() || versionId == QStringLiteral("0"))
        return err(QStringLiteral("「当前版本」无需回滚"));  // 合成条目，服务端没有这一行

    // 服务端会在回滚前自动把当前内容备份成一条历史版本 —— 回滚不是单向删除
    const Response r = request(Method::Post, QStringLiteral("/api/v1/files/") + pct(fileId)
                                                 + QStringLiteral("/versions/") + pct(versionId)
                                                 + QStringLiteral("/restore"),
                               QByteArrayLiteral("{}"));
    return r.ok() ? ok() : err(r.error);
}

Ok HttpBackend::snapshotVersion(const QString &fileId, const QString &note)
{
    if (isDirId(fileId))
        return err(QStringLiteral("目录没有历史版本"));

    QJsonObject body;
    if (!note.isEmpty())
        body.insert(QStringLiteral("note"), note);

    // 手动快照：把"当前内容"存成一条历史版本（服务端 version_no 单调递增，只增不减）
    const Response r = request(Method::Post, QStringLiteral("/api/v1/files/") + pct(fileId)
                                                 + QStringLiteral("/versions"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    return r.ok() ? ok() : err(r.error);
}

Ok HttpBackend::deleteVersion(const QString &fileId, const QString &versionId)
{
    if (isDirId(fileId))
        return err(QStringLiteral("目录没有历史版本"));
    // id="0" 是服务端合成的「当前版本」条目，库里没有这一行，删它必然 404 —— 提前拦下
    if (versionId.isEmpty() || versionId == QStringLiteral("0"))
        return err(QStringLiteral("「当前版本」不是历史版本，无法删除"));

    const Response r = request(Method::Delete, QStringLiteral("/api/v1/files/") + pct(fileId)
                                                   + QStringLiteral("/versions/")
                                                   + pct(versionId));
    return r.ok() ? ok() : err(r.error);
}

// =========================================================================
// 分享（服务端已实现：POST/GET /api/v1/shares、DELETE /api/v1/shares/:id）
//   公开端点 /s/:token（免鉴权）由服务端提供，客户端不调用，仅展示完整 URL。
// =========================================================================

ShareLink HttpBackend::shareFromObject(const QJsonObject &o, bool includeCode) const
{
    ShareLink s;
    s.id       = QString::number(o.value(QStringLiteral("id")).toVariant().toLongLong());
    s.fileId   = QString::number(o.value(QStringLiteral("file_id")).toVariant().toLongLong());
    s.fileName = o.value(QStringLiteral("name")).toString();
    s.isDir    = false; // 服务端仅对文件提供分享
    s.size     = o.value(QStringLiteral("size")).toVariant().toLongLong();
    // 完整 URL = 当前基地址 + 服务端返回 path（如 "/s/<token>"）；基地址与预览/下载同源
    const QString path = o.value(QStringLiteral("path")).toString();
    s.url      = path.isEmpty() ? QString() : m_baseUrl + path;
    s.needCode = o.value(QStringLiteral("need_code")).toBool();
    s.code     = includeCode ? o.value(QStringLiteral("code")).toString() : QString();
    const qint64 createdMs = o.value(QStringLiteral("created_at")).toVariant().toLongLong();
    if (createdMs > 0)
        s.created = QDateTime::fromMSecsSinceEpoch(createdMs).toUTC();
    const qint64 expMs = o.value(QStringLiteral("expires_at")).toVariant().toLongLong();
    if (expMs > 0)
        s.expire = QDateTime::fromMSecsSinceEpoch(expMs).toUTC(); // 0 = 永久（expire 保持无效）
    s.maxDownloads = o.value(QStringLiteral("max_downloads")).toInt();
    s.downloads    = o.value(QStringLiteral("downloads")).toInt();
    s.revoked      = o.value(QStringLiteral("revoked")).toBool();  // 服务端软撤销标记
    // 服务端新增 state 字段（active/revoked/expired/exhausted）；缺省按 "active" 向后兼容
    s.state        = o.value(QStringLiteral("state")).toString();
    if (s.state.isEmpty())
        s.state = QStringLiteral("active");
    return s;
}

QVector<ShareLink> HttpBackend::sharesFromBody(const QByteArray &body) const
{
    QVector<ShareLink> out;
    const QJsonArray   arr =
        QJsonDocument::fromJson(body).object().value(QStringLiteral("items")).toArray();
    out.reserve(arr.size());
    for (const QJsonValue &v : arr)
        out.append(shareFromObject(v.toObject(), /*includeCode=*/false));
    return out;
}

Result<QVector<ShareLink>> HttpBackend::shares()
{
    const Response r = request(Method::Get, QStringLiteral("/api/v1/shares"));
    if (!r.ok())
        return Result<QVector<ShareLink>>::fail(r.error);
    return Result<QVector<ShareLink>>::success(sharesFromBody(r.body));
}

void HttpBackend::sharesAsync(std::function<void(Result<QVector<ShareLink>>)> done)
{
    requestAsync(Method::Get, QStringLiteral("/api/v1/shares"), {}, {},
                 [this, done](Response r) {
                     if (!r.ok()) {
                         done(Result<QVector<ShareLink>>::fail(r.error));
                         return;
                     }
                     done(Result<QVector<ShareLink>>::success(sharesFromBody(r.body)));
                 });
}

Result<ShareLink> HttpBackend::createShare(const QString &fileId, const QString &code,
                                           int expireDays, int maxDownloads)
{
    QJsonObject body;
    body.insert(QStringLiteral("file_id"), fileId);
    body.insert(QStringLiteral("code"), code);
    body.insert(QStringLiteral("expire_days"), expireDays);     // 0 = 永久
    body.insert(QStringLiteral("max_downloads"), maxDownloads); // 0 = 不限
    const Response r = request(Method::Post, QStringLiteral("/api/v1/shares"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!r.ok())
        return Result<ShareLink>::fail(r.error);
    return Result<ShareLink>::success(
        shareFromObject(QJsonDocument::fromJson(r.body).object(), /*includeCode=*/true));
}

void HttpBackend::createShareAsync(const QString &fileId, const QString &code, int expireDays,
                                   int maxDownloads,
                                   std::function<void(Result<ShareLink>)> done)
{
    QJsonObject body;
    body.insert(QStringLiteral("file_id"), fileId);
    body.insert(QStringLiteral("code"), code);
    body.insert(QStringLiteral("expire_days"), expireDays);
    body.insert(QStringLiteral("max_downloads"), maxDownloads);
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    requestAsync(Method::Post, QStringLiteral("/api/v1/shares"), payload, {},
                 [this, done](Response r) {
                     if (!r.ok()) {
                         done(Result<ShareLink>::fail(r.error));
                         return;
                     }
                     done(Result<ShareLink>::success(
                         shareFromObject(QJsonDocument::fromJson(r.body).object(), true)));
                 });
}

Ok HttpBackend::revokeShare(const QString &shareId)
{
    const Response r = request(Method::Delete, QStringLiteral("/api/v1/shares/") + pct(shareId));
    return r.ok() ? ok() : err(r.error);
}

void HttpBackend::revokeShareAsync(const QString &shareId, std::function<void(Ok)> done)
{
    requestAsync(Method::Delete, QStringLiteral("/api/v1/shares/") + pct(shareId), {}, {},
                 [done](Response r) { done(r.ok() ? ok() : err(r.error)); });
}

// 清除当前调用者名下所有无效分享（revoked/expired/exhausted），物理删除不可恢复。
Ok HttpBackend::cleanupInvalidShares(std::int64_t &removed)
{
    removed = 0;
    const Response r = request(Method::Post, QStringLiteral("/api/v1/shares/cleanup"));
    if (!r.ok())
        return err(r.error);
    const QJsonObject obj = QJsonDocument::fromJson(r.body).object();
    removed = obj.value(QStringLiteral("removed")).toVariant().toLongLong();
    return ok();
}

void HttpBackend::cleanupInvalidSharesAsync(std::function<void(Result<std::int64_t>)> done)
{
    requestAsync(Method::Post, QStringLiteral("/api/v1/shares/cleanup"), {}, {},
                 [done](Response r) {
                     if (!r.ok()) {
                         done(Result<std::int64_t>::fail(r.error));
                         return;
                     }
                     const QJsonObject obj = QJsonDocument::fromJson(r.body).object();
                     const std::int64_t removed =
                         obj.value(QStringLiteral("removed")).toVariant().toLongLong();
                     done(Result<std::int64_t>::success(removed));
                 });
}

Ok HttpBackend::touchShare(const QString &)
{
    return err(unsupportedMsg(QStringLiteral("分享链接")));
}

// =========================================================================
// 标签与检索（GET/POST /api/v1/tags、DELETE /api/v1/tags/:id、
//             PUT /api/v1/files/:id/tags、POST /api/v1/search）
// =========================================================================

Result<QVector<Tag>> HttpBackend::tags()
{
    const Response r = request(Method::Get, QStringLiteral("/api/v1/tags"));
    if (!r.ok())
        return Result<QVector<Tag>>::fail(r.error);

    const QJsonArray arr =
        QJsonDocument::fromJson(r.body).object().value(QStringLiteral("items")).toArray();
    QVector<Tag> out;
    for (const QJsonValue &v : arr)
        out.append(tagFromServer(v.toObject()));
    return Result<QVector<Tag>>::success(out);
}

Result<Tag> HttpBackend::createTag(const QString &name, const QString &color)
{
    QJsonObject body;
    body.insert(QStringLiteral("name"), name);
    if (!color.isEmpty())
        body.insert(QStringLiteral("color"), color);

    // 同名（NOCASE）服务端返回 200 + created=false（幂等），同样算成功
    const Response r = request(Method::Post, QStringLiteral("/api/v1/tags"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!r.ok())
        return Result<Tag>::fail(r.error);
    return Result<Tag>::success(tagFromServer(QJsonDocument::fromJson(r.body).object()));
}

Ok HttpBackend::deleteTag(const QString &tagId)
{
    // 服务端会级联清掉 file_tag 关联，不需要客户端补刀
    const Response r = request(Method::Delete, QStringLiteral("/api/v1/tags/") + pct(tagId));
    return r.ok() ? ok() : err(r.error);
}

// 附加能力：某文件当前关联的标签（GET /api/v1/files/:id/tags）。
// Backend 抽象接口未提供此查询（MockBackend 从本地 FileItem.tagIds 反查），
// 这里作为 HttpBackend 的加法式扩展暴露给 TagController。
Result<QVector<Tag>> HttpBackend::tagsOfFile(const QString &fileId)
{
    if (isDirId(fileId))
        return Result<QVector<Tag>>::success(QVector<Tag>());

    const Response r = request(Method::Get, QStringLiteral("/api/v1/files/") + pct(fileId)
                                               + QStringLiteral("/tags"));
    if (!r.ok())
        return Result<QVector<Tag>>::fail(r.error);

    const QJsonArray arr =
        QJsonDocument::fromJson(r.body).object().value(QStringLiteral("items")).toArray();
    QVector<Tag> out;
    for (const QJsonValue &v : arr)
        out.append(tagFromServer(v.toObject()));
    return Result<QVector<Tag>>::success(out);
}

Result<QVector<FileItem>> HttpBackend::search(const QString &keyword, const QStringList &tagIds,
                                              int typeFilter)
{
    QJsonObject body;
    if (!keyword.isEmpty())
        body.insert(QStringLiteral("keyword"), keyword);
    QJsonArray tags;
    for (const QString &t : tagIds)
        tags.append(t.toLongLong());
    if (!tags.isEmpty())
        body.insert(QStringLiteral("tag_ids"), tags);
    if (typeFilter >= 0)
        body.insert(QStringLiteral("type"), typeFilter);

    const Response r = request(Method::Post, QStringLiteral("/api/v1/search"),
                               QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!r.ok())
        return Result<QVector<FileItem>>::fail(r.error);

    const QJsonArray arr =
        QJsonDocument::fromJson(r.body).object().value(QStringLiteral("items")).toArray();
    QVector<FileItem> out;
    for (const QJsonValue &v : arr)
        out.append(fileItemFromServer(v.toObject()));
    return Result<QVector<FileItem>>::success(out);
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

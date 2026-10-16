/****************************************************************************
 * controllers/FileController.cpp —— 文件列表 / 文件操作控制器实现
 *
 * 行为对齐已验证母体 client/src/MainWindow.cpp：
 *   · 列表解析        -> handleListReply（字段 id/name/dir/size/hash/created_at）
 *   · 新建 / 重命名 / 删除 -> onCreateFile / onRenameFile / onDeleteFile
 *   · 同名冲突 409    -> 中文提示「该目录下已存在同名文件」
 ****************************************************************************/
#include "FileController.h"

#include "core/Util.h"          // Util::humanSize（预览元信息）
#include "core/Async.h"        // 病毒查杀异步执行（服务端可能等远端结论）
#include "data/FileListModel.h" // cv::FileListModel / cv::FileRow（契约 §4）
#include "net/Backend.h"
#include "net/HttpBackend.h"    // uploadWholeFile（新建空文件，POST /api/v1/files）

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QImage>
#include <QList>
#include <QStringConverter>
#include <QVariantMap>

#include <algorithm>

namespace cv {
namespace {

// 后端失败信息 -> 中文可读提示（含 401 的可操作引导），绝不吞错。
QString friendlyError(const QString &raw)
{
    if (raw.isEmpty())
        return QStringLiteral("未知错误");

    if (raw.startsWith(QStringLiteral("[unsupported]")))
        return QStringLiteral("服务端暂不支持此功能");

    if (raw.contains(QStringLiteral("HTTP 401")))
        return QStringLiteral("令牌无效，请检查访问令牌");
    if (raw.contains(QStringLiteral("HTTP 403")))
        return QStringLiteral("服务器拒绝访问（403）");
    if (raw.contains(QStringLiteral("HTTP 404")))
        return QStringLiteral("目标不存在（404）");
    if (raw.startsWith(QStringLiteral("[conflict]")) || raw.contains(QStringLiteral("HTTP 409")))
        return QStringLiteral("该目录下已存在同名文件，请换个名字");
    if (raw.contains(QStringLiteral("HTTP 507")))
        return QStringLiteral("服务器空间不足（507）");
    if (raw.contains(QStringLiteral("HTTP 413")))
        return QStringLiteral("文件超出服务器单次上限（413）");
    if (raw.contains(QStringLiteral("HTTP 422")))
        return QStringLiteral("文件内容校验失败（哈希不符）");

    return raw;
}

// 单段文件名 / 目录名合法性（对齐母体 validRelPathInput 的核心约束）
bool validSegment(const QString &name, QString *why)
{
    const auto fail = [why](const QString &m) {
        if (why) *why = m;
        return false;
    };
    if (name.isEmpty())
        return fail(QStringLiteral("名称不能为空"));
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return fail(QStringLiteral("名称不能包含 '/' 或 '\\'"));
    if (name == QStringLiteral(".") || name == QStringLiteral(".."))
        return fail(QStringLiteral("名称不能为 '.' 或 '..'"));
    return true;
}

} // namespace

FileController::FileController(Backend *backend, FileListModel *model, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_model(model)
{
    m_emptyText = m_backend ? QStringLiteral("尚未加载")
                            : QStringLiteral("未配置数据源");
}

// ---------------------------------------------------------------------------
//  属性
// ---------------------------------------------------------------------------

void FileController::setSelectedIds(const QStringList &ids)
{
    if (ids == m_selectedIds)
        return;
    m_selectedIds = ids;
    emit selectedIdsChanged();
}

void FileController::setLoading(bool on)
{
    if (on == m_loading)
        return;
    m_loading = on;
    emit loadingChanged();
}

void FileController::setEmptyText(const QString &text)
{
    if (text == m_emptyText)
        return;
    m_emptyText = text;
    emit emptyTextChanged();
}

void FileController::setCurrentDir(const QString &dir)
{
    if (dir == m_currentDir)
        return;
    m_currentDir = dir;
    emit currentDirChanged();
}

QString FileController::normalizeDir(const QString &dir)
{
    QString d = dir.trimmed();
    while (d.startsWith(QLatin1Char('/')))
        d.remove(0, 1);
    while (d.endsWith(QLatin1Char('/')))
        d.chop(1);
    return d;
}

// ---------------------------------------------------------------------------
//  列表
// ---------------------------------------------------------------------------

void FileController::refresh()
{
    beginLoad(/*pollOnly=*/false);
}

void FileController::refreshIfChanged()
{
    // 低频轮询入口：与 refresh() 共用同一套异步回包处理与 m_refreshSeq 序号，
    // 仅在**内容指纹变化**时才应用列表并 emit refreshFinished(true)。
    beginLoad(/*pollOnly=*/true);
}

void FileController::beginLoad(bool pollOnly)
{
    if (!m_backend) {
        setItems({});
        setEmptyText(QStringLiteral("未配置数据源"));
        emit errorOccurred(QStringLiteral("未配置数据源"));
        emit refreshFinished(false); // 退出路径①：无数据源 → 视为失败
        return;
    }

    // 仅"显式刷新"才置加载态 / 占位文案；低频轮询不打扰界面（未变化时更应零副作用）。
    if (!pollOnly) {
        setLoading(true);
        setEmptyText(QStringLiteral("正在加载…"));
    }

    // 非阻塞刷新：走异步变体（HttpBackend 真异步；本地引擎默认退化同步，行为不变）。
    // refresh() 与 refreshIfChanged() **共用** m_refreshSeq：连点 / 切目录 / 轮询混发时旧回包一律作废。
    const int     seq = ++m_refreshSeq;
    const QString dir = m_currentDir;
    m_backend->listFolderAsync(dir, [this, seq, dir, pollOnly](Result<QVector<FileItem>> r) {
        if (seq != m_refreshSeq) {
            return; // 乱序：已发起更新的刷新，本回包作废（不 emit、不改状态）
        }
        handleFolderReply(dir, r, pollOnly);
    });
}

void FileController::handleFolderReply(const QString &dir,
                                       const Result<QVector<FileItem>> &reply,
                                       bool pollOnly)
{
    // ---- 失败：与 refresh() 失败路径**完全一致**（恰好一次 refreshFinished(false)）----
    if (!reply.ok) {
        setLoading(false);
        setItems({});
        const QString msg = friendlyError(reply.error);
        setEmptyText(msg);
        emit errorOccurred(msg);
        emit logMessage(QStringLiteral("ERROR"),
                        QStringLiteral("列出目录「%1」失败：%2")
                            .arg(dir.isEmpty() ? QStringLiteral("根目录") : dir, reply.error));
        emit refreshFinished(false); // 退出路径②：列出失败
        return;
    }

    // ---- 轮询且内容未变：不动界面、不发任何信号（连 refreshFinished 也不发）----
    if (pollOnly && fingerprintOf(reply.value) == m_lastFingerprint) {
        emit logMessage(QStringLiteral("INFO"), QStringLiteral("轮询：内容未变化"));
        return;
    }

    // ---- 成功应用（非轮询恒走此处；轮询仅在"变了"时走此处）----
    setLoading(false);
    setItems(reply.value);
    setEmptyText(reply.value.isEmpty() ? QStringLiteral("此文件夹为空") : QString());
    m_lastFingerprint = fingerprintOf(reply.value); // 更新缓存指纹（紧随的轮询据此判"未变化"）
    emit logMessage(QStringLiteral("INFO"),
                    QStringLiteral("已列出「%1」，共 %2 项")
                        .arg(dir.isEmpty() ? QStringLiteral("根目录") : dir)
                        .arg(reply.value.size()));
    emit refreshFinished(true); // 退出路径③：成功收尾（恰好一次）
}

QString FileController::fingerprintOf(const QVector<FileItem> &items)
{
    QStringList parts;
    parts.reserve(items.size());
    for (const FileItem &it : items) {
        // 必须带 hash：服务端覆盖同名文件时**不改** created_at（modified 不变），只改内容，
        // 故仅凭 id/name/size/mtime 检不到"同 size 覆盖"。hash = 内容 SHA-256，会随之变化。
        // （字段取自 FileItem 真实定义；目录 hash 为空串，稳定、不误报。）
        parts << QStringLiteral("%1|%2|%3|%4|%5")
                     .arg(it.id, it.name)
                     .arg(it.size)
                     .arg(it.modified.isValid() ? it.modified.toMSecsSinceEpoch() : 0)
                     .arg(it.hash);
    }
    std::sort(parts.begin(), parts.end()); // 稳定：判定与后端返回顺序无关
    return QString::fromLatin1(QCryptographicHash::hash(parts.join(QLatin1Char('\n')).toUtf8(),
                                                        QCryptographicHash::Sha256)
                                   .toHex());
}

// ---------------------------------------------------------------------------
//  文件预览（异步；绝不阻塞主线程）
// ---------------------------------------------------------------------------

QString FileController::previewKindFor(const FileItem &item)
{
    if (item.isDir)
        return QStringLiteral("dir");
    const QString ext = QFileInfo(item.name).suffix().toLower();

    static const QStringList kImageExts = {QStringLiteral("png"),  QStringLiteral("jpg"),
                                           QStringLiteral("jpeg"), QStringLiteral("gif"),
                                           QStringLiteral("bmp"),  QStringLiteral("webp")};
    if (kImageExts.contains(ext))
        return QStringLiteral("image");

    static const QStringList kTextExts = {
        QStringLiteral("txt"),  QStringLiteral("md"),   QStringLiteral("markdown"),
        QStringLiteral("json"), QStringLiteral("xml"),  QStringLiteral("csv"),
        QStringLiteral("tsv"),  QStringLiteral("log"),  QStringLiteral("ini"),
        QStringLiteral("cfg"),  QStringLiteral("conf"), QStringLiteral("yaml"),
        QStringLiteral("yml"),  QStringLiteral("html"), QStringLiteral("htm"),
        QStringLiteral("css"),  QStringLiteral("js"),   QStringLiteral("ts"),
        QStringLiteral("cpp"),  QStringLiteral("c"),    QStringLiteral("h"),
        QStringLiteral("hpp"),  QStringLiteral("py"),   QStringLiteral("java"),
        QStringLiteral("sh"),   QStringLiteral("bat"),  QStringLiteral("sql"),
        QStringLiteral("go"),   QStringLiteral("rs"),   QStringLiteral("toml")};
    if (kTextExts.contains(ext))
        return QStringLiteral("text");

    return QStringLiteral("other");
}

void FileController::resetPreviewToIdle()
{
    m_previewMeta.clear();
    m_previewText.clear();
    m_previewImageUrl.clear();
    m_previewError.clear();
    m_previewState = QStringLiteral("idle");
}

void FileController::clearPreview()
{
    ++m_previewSeq; // 作废在途回包
    resetPreviewToIdle();
    emit previewChanged();
}

void FileController::requestPreview(const QString &fileId)
{
    if (fileId.isEmpty()) {
        clearPreview();
        return;
    }

    const int seq = ++m_previewSeq; // **独立**序号：与 m_refreshSeq 互不影响

    // 在当前列表里找该文件（预览入口来自列表右键菜单，必然在此）。拷贝一份，异步回包时列表可能已变。
    const FileItem *found = nullptr;
    for (const FileItem &it : m_items) {
        if (it.id == fileId) {
            found = &it;
            break;
        }
    }
    if (!found) {
        resetPreviewToIdle();
        m_previewState = QStringLiteral("error");
        m_previewError = QStringLiteral("文件不在当前列表中");
        emit previewChanged();
        return;
    }
    const FileItem item = *found;

    // 元信息（键名冻结：id/name/isDir/sizeText/timeText/path/hash/kind）
    const QString kind = previewKindFor(item);
    QVariantMap   meta;
    meta.insert(QStringLiteral("id"), item.id);
    meta.insert(QStringLiteral("name"), item.name);
    meta.insert(QStringLiteral("isDir"), item.isDir);
    meta.insert(QStringLiteral("sizeText"), Util::humanSize(item.size));
    meta.insert(QStringLiteral("timeText"),
                item.modified.isValid()
                    ? item.modified.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                    : QStringLiteral("-"));
    meta.insert(QStringLiteral("path"), item.path);
    meta.insert(QStringLiteral("hash"), item.hash);
    meta.insert(QStringLiteral("kind"), kind);
    m_previewMeta = meta;

    if (!m_backend) {
        m_previewState = QStringLiteral("error");
        m_previewText.clear();
        m_previewImageUrl.clear();
        m_previewError = QStringLiteral("未配置数据源");
        emit previewChanged();
        return;
    }

    // 目录 / 其它类型：不支持预览（**仍保留元信息**）
    if (kind == QStringLiteral("dir") || kind == QStringLiteral("other")) {
        m_previewState = QStringLiteral("unsupported");
        m_previewText.clear();
        m_previewImageUrl.clear();
        m_previewError.clear();
        emit previewChanged();
        return;
    }

    // 加载态 + 异步取片段（文本 64 KiB / 图片 4 MiB）
    m_previewState = QStringLiteral("loading");
    m_previewText.clear();
    m_previewImageUrl.clear();
    m_previewError.clear();
    emit previewChanged();

    const qint64 span =
        (kind == QStringLiteral("image")) ? qint64(4 * 1024 * 1024) : qint64(64 * 1024);
    m_backend->getRangeAsync(fileId, 0, span,
                             [this, seq, kind](Result<QByteArray> r) {
                                 handlePreviewReply(seq, kind, r);
                             });
}

void FileController::handlePreviewReply(int seq, const QString &kind,
                                        const Result<QByteArray> &reply)
{
    if (seq != m_previewSeq) {
        return; // 乱序 / 已切换预览目标：丢弃，不发任何信号
    }

    if (!reply.ok) {
        m_previewState = QStringLiteral("error");
        m_previewText.clear();
        m_previewImageUrl.clear();
        m_previewError = friendlyError(reply.error);
        emit previewChanged();
        return;
    }

    const QByteArray data = reply.value;

    if (kind == QStringLiteral("image")) {
        QImage img;
        if (!img.loadFromData(data)) {
            m_previewState = QStringLiteral("unsupported"); // 解码失败 → 不支持
            m_previewText.clear();
            m_previewImageUrl.clear();
            m_previewError.clear();
            emit previewChanged();
            return;
        }
        QByteArray png;
        QBuffer    buf(&png);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        buf.close();
        m_previewImageUrl = QStringLiteral("data:image/png;base64,")
                            + QString::fromLatin1(png.toBase64());
        m_previewText.clear();
        m_previewError.clear();
        m_previewState = QStringLiteral("image");
        emit previewChanged();
        return;
    }

    // 文本：含 NUL 或非合法 UTF-8 → 不支持
    bool   badEncoding = data.contains('\0');
    QString text;
    if (!badEncoding) {
        QStringDecoder dec(QStringDecoder::Utf8);
        text = dec(data);
        badEncoding = dec.hasError();
    }
    if (badEncoding) {
        m_previewState = QStringLiteral("unsupported");
        m_previewText.clear();
        m_previewImageUrl.clear();
        m_previewError.clear();
        emit previewChanged();
        return;
    }
    m_previewText = text;
    m_previewImageUrl.clear();
    m_previewError.clear();
    m_previewState = QStringLiteral("text");
    emit previewChanged();
}

void FileController::enterDir(const QString &dir)
{
    const QString d = normalizeDir(dir);
    setCurrentDir(d);
    refresh();
}

void FileController::goUp()
{
    if (m_currentDir.isEmpty())
        return; // 已在根目录
    const int slash = m_currentDir.lastIndexOf(QLatin1Char('/'));
    setCurrentDir(slash < 0 ? QString() : m_currentDir.left(slash));
    refresh();
}

void FileController::setItems(const QVector<FileItem> &items)
{
    m_items = items;

    // 填充 QML 数据模型（契约 §4 冻结角色名由 FileListModel 负责）
    if (m_model) {
        QList<FileRow> rows;
        rows.reserve(items.size());
        for (const FileItem &it : items) {
            FileRow row;
            row.id   = it.id;
            row.name = it.name;
            // FileItem.parentId 承载「所属目录」；根目录统一表示为 ''
            row.dir  = (it.parentId == kRootId) ? QString() : it.parentId;
            row.hash = it.hash;
            row.status = QStringLiteral("-"); // 列表接口无 instant 字段
            row.size = it.size;
            row.createdAt = it.modified.isValid() ? it.modified.toMSecsSinceEpoch() : 0;
            rows.append(row);
        }
        m_model->setRows(rows);
    }

    // 修剪选中项：仅保留仍存在于当前列表的 id
    QStringList keep;
    for (const QString &sid : m_selectedIds) {
        for (const FileItem &it : items) {
            if (it.id == sid) {
                keep << sid;
                break;
            }
        }
    }
    if (keep != m_selectedIds) {
        m_selectedIds = keep;
        emit selectedIdsChanged();
    }
}

// ---------------------------------------------------------------------------
//  文件操作（对齐母体 onCreateFile / onRenameFile / onDeleteFile）
// ---------------------------------------------------------------------------

void FileController::createFile(const QString &name)
{
    if (!m_backend) {
        emit errorOccurred(QStringLiteral("未配置数据源"));
        return;
    }

    const QString clean = name.trimmed();
    QString why;
    if (!validSegment(clean, &why)) {
        emit errorOccurred(why);
        return;
    }

    QString err;
    if (auto *hb = qobject_cast<HttpBackend *>(m_backend)) {
        // 远程服务：整文件上传（空体 → 0 字节文件），同目录同名 → 409 [conflict]
        const Result<FileItem> r =
            hb->uploadWholeFile(m_currentDir, clean, QByteArray(), /*overwrite=*/false);
        if (!r.ok)
            err = r.error;
    } else {
        // 其它后端（本地引擎）：用抽象接口建一个 0 字节文件
        const QByteArray emptyHash =
            QCryptographicHash::hash(QByteArray(), QCryptographicHash::Sha256).toHex();
        const Result<UploadTicket> t = m_backend->beginUpload(
            m_currentDir, clean, 0, QString::fromLatin1(emptyHash));
        if (!t.ok) {
            err = t.error;
        } else if (!t.value.instant) {
            const Result<FileItem> f = m_backend->finishUpload(t.value.uploadId);
            if (!f.ok)
                err = f.error;
        }
    }

    if (!err.isEmpty()) {
        const QString msg = friendlyError(err);
        emit errorOccurred(msg);
        emit logMessage(QStringLiteral("ERROR"), QStringLiteral("新建文件失败：%1").arg(err));
        return;
    }

    emit statusMessage(QStringLiteral("✓ 新建文件成功"), true);
    emit storageChanged(); // 成功 → 通知用例变更（Application 触发 Stats.refresh()）
    refresh();
}

QVariantList FileController::allFolders()
{
    QVariantList out;
    // 根目录永远可选：把文件移回根是很常见的需求，少了它就等于没法"移出去"
    {
        QVariantMap m;
        m.insert(QStringLiteral("id"), QStringLiteral("root"));
        m.insert(QStringLiteral("label"), QStringLiteral("根目录"));
        m.insert(QStringLiteral("depth"), 0);
        out.append(m);
    }
    if (!m_backend)
        return out;

    const Result<QVector<FileItem>> r = m_backend->listUnderPath(QString());
    if (!r.ok)
        return out;

    QStringList dirs;
    for (const FileItem &f : r.value) {
        if (f.isDir && !f.path.isEmpty())
            dirs << f.path;
    }
    dirs.sort();   // 按路径排序即树形顺序（父目录一定排在子目录前）

    for (const QString &d : dirs) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), d);        // 目录用**路径**作 id
        m.insert(QStringLiteral("label"), d);
        m.insert(QStringLiteral("depth"), d.count(QLatin1Char('/')) + 1);
        out.append(m);
    }
    return out;
}

void FileController::scanFile(const QString &fileId)
{
    if (!m_backend) {
        emit errorOccurred(QStringLiteral("未配置数据源"));
        return;
    }
    if (fileId.isEmpty()) {
        emit errorOccurred(QStringLiteral("请先选择要查杀的文件"));
        return;
    }
    emit statusMessage(QStringLiteral("正在查杀…"), true);
    // 异步：服务端可能要把哈希发给远端并等结论（默认 3s），同步调用会冻结界面
    Async::run(
        this, [this, fileId] { return m_backend->scanFile(fileId); },
        [this, fileId](Result<ScanVerdict> r) {
            if (!r.ok) {
                emit scanFinished(fileId, QString(), QString(), false, false, false,
                                  friendlyError(r.error));
                return;
            }
            emit scanFinished(fileId, r.value.verdict, r.value.detail, r.value.blocked,
                              r.value.remoteFailed, true, QString());
        });
}

void FileController::moveTo(const QString &id, const QString &targetParentId)
{
    if (!m_backend) {
        emit errorOccurred(QStringLiteral("未配置数据源"));
        return;
    }
    if (id.isEmpty()) {
        emit errorOccurred(QStringLiteral("请先选择要移动的文件"));
        return;
    }

    const Ok r = m_backend->moveFile(id, targetParentId);
    if (!r.ok) {
        emit errorOccurred(friendlyError(r.error));
        emit logMessage(QStringLiteral("ERROR"), QStringLiteral("移动失败：%1").arg(r.error));
        return;
    }
    emit statusMessage(QStringLiteral("✓ 已移动"), true);
    emit storageChanged();
    refresh();
}

void FileController::renameFolder(const QString &dirPath, const QString &newName)
{
    if (!m_backend) {
        emit errorOccurred(QStringLiteral("未配置数据源"));
        return;
    }
    if (dirPath.isEmpty()) {
        emit errorOccurred(QStringLiteral("请先选择要重命名的文件夹"));
        return;
    }
    const QString clean = newName.trimmed();
    QString       why;
    if (!validSegment(clean, &why)) {
        emit errorOccurred(why);
        return;
    }

    const Ok r = m_backend->renameDir(dirPath, clean);
    if (!r.ok) {
        emit errorOccurred(friendlyError(r.error));
        emit logMessage(QStringLiteral("ERROR"),
                        QStringLiteral("重命名文件夹失败：%1").arg(r.error));
        return;
    }
    emit statusMessage(QStringLiteral("✓ 文件夹已重命名"), true);
    emit storageChanged();
    refresh();
}

void FileController::rename(const QString &id, const QString &name)
{
    if (!m_backend) {
        emit errorOccurred(QStringLiteral("未配置数据源"));
        return;
    }
    if (id.isEmpty()) {
        emit errorOccurred(QStringLiteral("请先选择要重命名的文件"));
        return;
    }

    const QString clean = name.trimmed();
    QString why;
    if (!validSegment(clean, &why)) {
        emit errorOccurred(why);
        return;
    }

    const Ok r = m_backend->rename(id, clean);
    if (!r.ok) {
        const QString msg = friendlyError(r.error);
        emit errorOccurred(msg);
        emit logMessage(QStringLiteral("ERROR"), QStringLiteral("重命名失败：%1").arg(r.error));
        return;
    }

    emit statusMessage(QStringLiteral("✓ 重命名成功"), true);
    emit storageChanged();
    refresh();
}

void FileController::remove(const QString &id)
{
    if (!m_backend) {
        emit errorOccurred(QStringLiteral("未配置数据源"));
        return;
    }
    if (id.isEmpty()) {
        emit errorOccurred(QStringLiteral("请先选择要删除的文件"));
        return;
    }

    // 软删除（移入回收站）：POST /api/v1/trash，body 二选一 —— {file_id} 或 {dir}。
    // 语义变化说明（历史：此处曾走 purge 永久删除，因为当时服务端确实没有回收站）：
    //   · 现在「删除」= 移入回收站，可恢复；真正的永久删除收敛到回收站页的「彻底删除」。
    //   · 两个口径必须分开讲（实测于 harness A7/A8）：
    //       磁盘 —— 一个字节都没少（分块引用与 blob 原样保留，内容库不 GC）；
    //       「已用空间」—— sumSizeOfOwner 带 `WHERE deleted = 0`，进站后**立刻扣除**，
    //                      所以设置页的用量数字会当场下降。
    //     即"用量降了、磁盘没释放"。文案不能只说"仍占用空间"，那会和用量数字打架。
    //   · 目录 id 形如 "dir:照片/2026"，由 net 层 trashTargetBody 翻译成 {dir:"照片/2026"}
    //     （放 body 而不放路径：http_server 不做百分号解码，含冒号/中文的路径段永远比不中）。
    const Ok r = m_backend->moveToTrash(id);
    if (!r.ok) {
        const QString msg = friendlyError(r.error);
        emit errorOccurred(msg);
        emit logMessage(QStringLiteral("ERROR"), QStringLiteral("移入回收站失败：%1").arg(r.error));
        return;
    }

    emit statusMessage(QStringLiteral("✓ 已移入回收站，可在回收站中恢复"), true);
    emit storageChanged();
    refresh();
}

void FileController::createFolder(const QString &path)
{
    if (!m_backend) {
        emit errorOccurred(QStringLiteral("未配置数据源"));
        return;
    }

    const QString clean = normalizeDir(path);
    if (clean.isEmpty()) {
        emit errorOccurred(QStringLiteral("目录名不能为空"));
        return;
    }
    // 校验每一段（只允许单段或 "a/b" 形式，禁止 '.'/'..'/绝对路径）
    const QStringList segs = clean.split(QLatin1Char('/'));
    for (const QString &seg : segs) {
        QString why;
        if (!validSegment(seg, &why)) {
            emit errorOccurred(why);
            return;
        }
    }

    const Result<FileItem> r = m_backend->ensureFolderPath(clean);
    if (!r.ok) {
        const QString msg = friendlyError(r.error);
        emit errorOccurred(msg);
        emit logMessage(QStringLiteral("ERROR"), QStringLiteral("新建文件夹失败：%1").arg(r.error));
        return;
    }

    emit statusMessage(QStringLiteral("✓ 新建文件夹成功"), true);
    emit storageChanged();
    refresh();
}

// ---------------------------------------------------------------------------
//  上传 / 下载入口（文件选择框由 QML 承担，控制器只派发意图）
// ---------------------------------------------------------------------------

void FileController::uploadHere()
{
    // QML 监听 uploadRequested(dir) → 打开 FileDialog → Transfer.upload(paths, dir)
    emit uploadRequested(m_currentDir);
}

void FileController::removeSelected()
{
    if (!m_backend) {
        emit errorOccurred(QStringLiteral("未配置数据源"));
        return;
    }
    if (m_selectedIds.isEmpty()) {
        emit errorOccurred(QStringLiteral("请先选择要移入回收站的项目"));
        return;
    }
    const QStringList ids = m_selectedIds;
    // 逐个调用（不并发）：批量删除不是吞吐场景，串行更可控，也不会一次开太多连接。
    // 放到后台线程跑，避免 N 个同步请求卡住界面。
    // 走**批量接口**：一次请求删 N 个（原来逐个调用 ⇒ N 次往返，多选删除慢）
    Async::run(
        this, [this, ids] {
            const Result<QPair<int, QString>> r = m_backend->moveToTrashBatch(ids);
            if (!r.ok)
                return qMakePair(0, r.error);
            return r.value;
        },
        [this, ids](QPair<int, QString> r) {
            const int okCount = r.first;
            if (okCount == ids.size()) {
                emit statusMessage(QStringLiteral("已移入回收站 %1 项").arg(okCount), true);
            } else {
                emit errorOccurred(QStringLiteral("移入回收站：成功 %1 项，失败 %2 项%3")
                                       .arg(okCount)
                                       .arg(ids.size() - okCount)
                                       .arg(r.second.isEmpty() ? QString()
                                                               : QStringLiteral("（%1）").arg(r.second)));
            }
            m_selectedIds.clear();
            emit selectedIdsChanged();
            refresh();
        });
}

void FileController::moveSelectedTo(const QString &targetParentId)
{
    if (!m_backend) {
        emit errorOccurred(QStringLiteral("未配置数据源"));
        return;
    }
    if (m_selectedIds.isEmpty()) {
        emit errorOccurred(QStringLiteral("请先选择要移动的项目"));
        return;
    }
    const QStringList ids = m_selectedIds;
    const QString     target = targetParentId;
    // 走**批量接口**：一次请求移动 N 个（原来逐个调用 ⇒ N 次往返）
    Async::run(
        this, [this, ids, target] {
            const Result<QPair<int, QString>> r = m_backend->moveFileBatch(ids, target);
            if (!r.ok)
                return qMakePair(0, r.error);
            return r.value;
        },
        [this, ids](QPair<int, QString> r) {
            const int okCount = r.first;
            if (okCount == ids.size()) {
                emit statusMessage(QStringLiteral("已移动 %1 项").arg(okCount), true);
            } else {
                emit errorOccurred(QStringLiteral("移动：成功 %1 项，失败 %2 项%3")
                                       .arg(okCount)
                                       .arg(ids.size() - okCount)
                                       .arg(r.second.isEmpty() ? QString()
                                                               : QStringLiteral("（%1）").arg(r.second)));
            }
            m_selectedIds.clear();
            emit selectedIdsChanged();
            refresh();
        });
}

void FileController::downloadSelected()
{
    if (m_selectedIds.isEmpty()) {
        emit errorOccurred(QStringLiteral("请先选择要下载的文件"));
        return;
    }
    // QML / 上层监听 downloadRequested(ids) → 选择保存目录 → Transfer.download(ids, dest)
    emit downloadRequested(m_selectedIds);
}

} // namespace cv

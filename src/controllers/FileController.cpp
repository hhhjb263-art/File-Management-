/****************************************************************************
 * controllers/FileController.cpp —— 文件列表 / 文件操作控制器实现
 *
 * 行为对齐已验证母体 client/src/MainWindow.cpp：
 *   · 列表解析        -> handleListReply（字段 id/name/dir/size/hash/created_at）
 *   · 新建 / 重命名 / 删除 -> onCreateFile / onRenameFile / onDeleteFile
 *   · 同名冲突 409    -> 中文提示「该目录下已存在同名文件」
 ****************************************************************************/
#include "FileController.h"

#include "data/FileListModel.h" // cv::FileListModel / cv::FileRow（契约 §4）
#include "net/Backend.h"
#include "net/HttpBackend.h"    // uploadWholeFile（新建空文件，POST /api/v1/files）

#include <QCryptographicHash>
#include <QList>

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
    if (!m_backend) {
        setItems({});
        setEmptyText(QStringLiteral("未配置数据源"));
        emit errorOccurred(QStringLiteral("未配置数据源"));
        return;
    }

    setLoading(true);
    setEmptyText(QStringLiteral("正在加载…"));

    const Result<QVector<FileItem>> r = m_backend->listFolder(m_currentDir);

    setLoading(false);

    if (!r.ok) {
        setItems({});
        const QString msg = friendlyError(r.error);
        setEmptyText(msg);
        emit errorOccurred(msg);
        emit logMessage(QStringLiteral("ERROR"),
                        QStringLiteral("列出目录「%1」失败：%2")
                            .arg(m_currentDir.isEmpty() ? QStringLiteral("根目录") : m_currentDir,
                                 r.error));
        return;
    }

    setItems(r.value);
    setEmptyText(r.value.isEmpty() ? QStringLiteral("此文件夹为空") : QString());
    emit logMessage(QStringLiteral("INFO"),
                    QStringLiteral("已列出「%1」，共 %2 项")
                        .arg(m_currentDir.isEmpty() ? QStringLiteral("根目录") : m_currentDir)
                        .arg(r.value.size()));
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

    // 契约 §8.1：purge = 真实永久删除（服务端 DELETE /api/v1/files/{id}），无回收站
    const Ok r = m_backend->purge(id);
    if (!r.ok) {
        const QString msg = friendlyError(r.error);
        emit errorOccurred(msg);
        emit logMessage(QStringLiteral("ERROR"), QStringLiteral("删除失败：%1").arg(r.error));
        return;
    }

    emit statusMessage(QStringLiteral("✓ 已删除（不可恢复）"), true);
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

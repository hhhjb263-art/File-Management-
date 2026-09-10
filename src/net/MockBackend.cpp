#include "MockBackend.h"

#include "Json.h"

#include "core/AppPaths.h"
#include "core/Crypto.h"
#include "core/Settings.h"
#include "core/Util.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QStorageInfo>
#include <algorithm>

namespace cv {
namespace {

// 版本保留策略：最近 10 份，且不超过 90 天
constexpr int kMaxVersions    = 10;
constexpr int kVersionKeepDays = 90;

} // namespace

// =========================================================================
// 构造 / 持久化
// =========================================================================

MockBackend::MockBackend(QObject *parent)
    : Backend(parent)
    , m_dataDir(AppPaths::dataDir())
    , m_blobDir(m_dataDir + QStringLiteral("/blobs"))
    , m_shareBase(Settings::instance().serverUrl())
{
    while (m_shareBase.endsWith(QLatin1Char('/')))
        m_shareBase.chop(1);

    QDir().mkpath(m_blobDir);
    QDir().mkpath(AppPaths::cacheDir() + QStringLiteral("/uploads"));
    load();
}

MockBackend::~MockBackend()
{
    QMutexLocker locker(&m_mutex);
    save();
}

void MockBackend::load()
{
    const QString dbFile = AppPaths::localDbFile();
    QFile f(dbFile);
    if (!f.open(QIODevice::ReadOnly))
        return;

    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();

    const QJsonObject user = root.value(QStringLiteral("user")).toObject();
    if (!user.isEmpty()) {
        m_user.id    = user.value(QStringLiteral("id")).toString();
        m_user.name  = user.value(QStringLiteral("name")).toString();
        m_user.email = user.value(QStringLiteral("email")).toString();
        m_user.quota = qint64(user.value(QStringLiteral("quota")).toDouble());
    }

    for (const QJsonValue &v : root.value(QStringLiteral("files")).toArray()) {
        const FileItem item = Json::fileFromJson(v.toObject());
        if (item.isValid())
            m_files.insert(item.id, item);
    }
    for (const QJsonValue &v : root.value(QStringLiteral("versions")).toArray()) {
        const FileVersion ver = Json::versionFromJson(v.toObject());
        if (!ver.id.isEmpty())
            m_versions[ver.fileId].append(ver);
    }
    for (const QJsonValue &v : root.value(QStringLiteral("shares")).toArray()) {
        const ShareLink s = Json::shareFromJson(v.toObject());
        if (!s.id.isEmpty())
            m_shares.insert(s.id, s);
    }
    for (const QJsonValue &v : root.value(QStringLiteral("tags")).toArray()) {
        const Tag t = Json::tagFromJson(v.toObject());
        if (!t.id.isEmpty())
            m_tags.insert(t.id, t);
    }

    qInfo() << "本地引擎已载入：" << m_files.size() << "个条目，数据目录" << m_dataDir;
}

void MockBackend::save() const
{
    QJsonObject root;

    QJsonObject user;
    user.insert(QStringLiteral("id"), m_user.id);
    user.insert(QStringLiteral("name"), m_user.name);
    user.insert(QStringLiteral("email"), m_user.email);
    user.insert(QStringLiteral("quota"), double(m_user.quota));
    root.insert(QStringLiteral("user"), user);

    QJsonArray files;
    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it)
        files.append(Json::toJson(it.value()));
    root.insert(QStringLiteral("files"), files);

    QJsonArray versions;
    for (auto it = m_versions.constBegin(); it != m_versions.constEnd(); ++it)
        for (const FileVersion &v : it.value())
            versions.append(Json::toJson(v));
    root.insert(QStringLiteral("versions"), versions);

    QJsonArray shares;
    for (auto it = m_shares.constBegin(); it != m_shares.constEnd(); ++it)
        shares.append(Json::toJson(it.value()));
    root.insert(QStringLiteral("shares"), shares);

    QJsonArray tags;
    for (auto it = m_tags.constBegin(); it != m_tags.constEnd(); ++it)
        tags.append(Json::toJson(it.value()));
    root.insert(QStringLiteral("tags"), tags);

    QFile f(AppPaths::localDbFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "元数据写入失败:" << f.errorString();
        return;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString MockBackend::blobPath(const QString &fileId) const
{
    return m_blobDir + QLatin1Char('/') + fileId + QStringLiteral(".bin");
}

QString MockBackend::versionPath(const QString &fileId, int version) const
{
    return m_blobDir + QLatin1Char('/') + fileId + QStringLiteral(".v") + QString::number(version)
         + QStringLiteral(".bin");
}

// =========================================================================
// 账户
// =========================================================================

Result<UserInfo> MockBackend::login(const QString &user, const QString &password)
{
    QMutexLocker locker(&m_mutex);

    if (user.trimmed().isEmpty() || password.isEmpty())
        return Result<UserInfo>::fail(QStringLiteral("请输入账号和密码"));

    if (m_user.id.isEmpty()) {
        m_user.id    = Crypto::randomHex(8);
        m_user.name  = user.trimmed();
        m_user.email = user.trimmed() + QStringLiteral("@local");
        save();
    } else if (m_user.name.compare(user.trimmed(), Qt::CaseInsensitive) != 0) {
        // 本地引擎为单用户，首次创建后沿用
        qInfo() << "本地引擎以既有账户" << m_user.name << "登录";
    }

    const QStorageInfo storage(m_dataDir);
    if (m_user.quota <= 0) {
        m_user.quota = storage.bytesTotal() > 0 ? qint64(storage.bytesTotal())
                                                : qint64(512) * 1024 * 1024 * 1024;
        save();
    }

    return Result<UserInfo>::success(m_user);
}

void MockBackend::logout()
{
    // 本地引擎无需注销会话，仅记录日志
    qInfo() << "已退出本地引擎";
}

UserInfo MockBackend::currentUser() const
{
    QMutexLocker locker(&m_mutex);
    return m_user;
}

// =========================================================================
// 目录与文件
// =========================================================================

Result<QVector<FileItem>> MockBackend::listFolder(const QString &parentId)
{
    QMutexLocker locker(&m_mutex);

    QVector<FileItem> out;
    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
        const FileItem &f = it.value();
        if (!f.trashed && f.parentId == parentId)
            out.append(f);
    }
    std::sort(out.begin(), out.end(), Json::fileLess);
    return Result<QVector<FileItem>>::success(out);
}

Result<QVector<FileItem>> MockBackend::listUnderPath(const QString &remotePath)
{
    QMutexLocker locker(&m_mutex);

    const QString base = remotePath.endsWith(QLatin1Char('/')) ? remotePath.chopped(1) : remotePath;
    const QString prefix = (base.isEmpty() || base == kRootPath) ? QString() : base + QLatin1Char('/');

    QVector<FileItem> out;
    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
        const FileItem &f = it.value();
        if (f.trashed)
            continue;
        if (prefix.isEmpty() || f.path.startsWith(prefix) || f.path == base)
            out.append(f);
    }
    std::sort(out.begin(), out.end(), Json::fileLess);
    return Result<QVector<FileItem>>::success(out);
}

Result<QVector<FileItem>> MockBackend::listTrash()
{
    QMutexLocker locker(&m_mutex);

    // 只列出被直接删除的顶层条目，子条目随父一起恢复
    QVector<FileItem> out;
    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
        const FileItem &f = it.value();
        if (!f.trashed)
            continue;
        auto parent = m_files.constFind(f.parentId);
        if (parent == m_files.constEnd() || !parent.value().trashed)
            out.append(f);
    }
    std::sort(out.begin(), out.end(), Json::fileLess);
    return Result<QVector<FileItem>>::success(out);
}

Result<FileItem> MockBackend::statById(const QString &id)
{
    QMutexLocker locker(&m_mutex);
    auto it = m_files.constFind(id);
    if (it == m_files.constEnd())
        return Result<FileItem>::fail(QStringLiteral("文件不存在"));
    return Result<FileItem>::success(it.value());
}

Result<FileItem> MockBackend::statByPath(const QString &remotePath)
{
    QMutexLocker locker(&m_mutex);

    const QString target = remotePath.size() > 1 && remotePath.endsWith(QLatin1Char('/'))
                               ? remotePath.chopped(1)
                               : remotePath;
    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
        if (!it.value().trashed && it.value().path == target)
            return Result<FileItem>::success(it.value());
    }
    return Result<FileItem>::fail(QStringLiteral("路径不存在：%1").arg(remotePath));
}

Result<FileItem> MockBackend::createFolder(const QString &parentId, const QString &name)
{
    QMutexLocker locker(&m_mutex);

    const QString cleanName = Util::sanitizeName(name);

    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
        const FileItem &f = it.value();
        if (!f.trashed && f.parentId == parentId && f.name.compare(cleanName, Qt::CaseInsensitive) == 0)
            return Result<FileItem>::fail(QStringLiteral("同名文件已存在"));
    }

    QString parentPath = kRootPath;
    if (parentId != kRootId) {
        auto parent = m_files.constFind(parentId);
        if (parent == m_files.constEnd())
            return Result<FileItem>::fail(QStringLiteral("父目录不存在"));
        if (!parent.value().isDir)
            return Result<FileItem>::fail(QStringLiteral("父节点不是目录"));
        parentPath = parent.value().path;
    }

    FileItem folder;
    folder.id       = Crypto::randomHex(12);
    folder.parentId = parentId;
    folder.name     = cleanName;
    folder.isDir    = true;
    folder.path     = Util::joinPath(parentPath, cleanName);
    folder.modified = QDateTime::currentDateTimeUtc();
    folder.rev      = Crypto::randomHex(6);
    folder.version  = 1;
    folder.type     = FileType::Folder;
    folder.device   = Util::deviceName();

    m_files.insert(folder.id, folder);
    save();
    return Result<FileItem>::success(folder);
}

Result<FileItem> MockBackend::ensureFolderPath(const QString &remotePath)
{
    QMutexLocker locker(&m_mutex);

    const QString clean = (remotePath.size() > 1 && remotePath.endsWith(QLatin1Char('/')))
                              ? remotePath.chopped(1)
                              : remotePath;

    if (clean.isEmpty() || clean == kRootPath) {
        FileItem root;
        root.id     = kRootId;
        root.path   = kRootPath;
        root.name   = kRootPath;
        root.isDir  = true;
        root.type   = FileType::Folder;
        return Result<FileItem>::success(root);
    }

    // 逐级查找/创建目录（整个过程持锁，内部不再调用其它加锁方法）
    const QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString           parentId   = kRootId;
    QString           parentPath = kRootPath;
    FileItem          last;

    for (const QString &part : parts) {
        const QString path = Util::joinPath(parentPath, part);

        bool     found = false;
        FileItem existing;
        for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
            if (!it.value().trashed && it.value().path == path) {
                existing = it.value();
                found    = true;
                break;
            }
        }

        if (found) {
            if (!existing.isDir)
                return Result<FileItem>::fail(QStringLiteral("路径中存在同名文件：%1").arg(part));
            last       = existing;
            parentId   = last.id;
            parentPath = last.path;
            continue;
        }

        FileItem folder;
        folder.id       = Crypto::randomHex(12);
        folder.parentId = parentId;
        folder.name     = Util::sanitizeName(part);
        folder.isDir    = true;
        folder.path     = path;
        folder.modified = QDateTime::currentDateTimeUtc();
        folder.rev      = Crypto::randomHex(6);
        folder.version  = 1;
        folder.type     = FileType::Folder;
        folder.device   = Util::deviceName();

        m_files.insert(folder.id, folder);
        last       = folder;
        parentId   = folder.id;
        parentPath = folder.path;
    }

    if (last.isValid())
        save();
    return Result<FileItem>::success(last);
}

Ok MockBackend::rename(const QString &id, const QString &newName)
{
    QMutexLocker locker(&m_mutex);

    auto it = m_files.find(id);
    if (it == m_files.end())
        return err(QStringLiteral("文件不存在"));

    const QString clean = Util::sanitizeName(newName);
    FileItem      item  = it.value();

    const QString parentPath =
        item.parentId == kRootId ? kRootPath : m_files.value(item.parentId).path;
    const QString newPath = Util::joinPath(parentPath, clean);

    item.name     = clean;
    item.path     = newPath;
    item.modified = QDateTime::currentDateTimeUtc();
    item.rev      = Crypto::randomHex(6);
    item.device   = Util::deviceName();
    it.value()    = item;

    renameRecursive(id, newPath);
    save();
    return ok();
}

void MockBackend::renameRecursive(const QString &id, const QString &newPath)
{
    // 先收集子项，避免在遍历过程中修改容器
    QVector<QString> children;
    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
        if (it.value().parentId == id && it.value().id != id)
            children.append(it.key());
    }

    for (const QString &cid : children) {
        auto it = m_files.find(cid);
        if (it == m_files.end())
            continue;

        FileItem      child     = it.value();
        const QString childPath = Util::joinPath(newPath, child.name);
        child.path              = childPath;
        it.value()              = child;

        if (child.isDir)
            renameRecursive(child.id, childPath);
    }
}

Ok MockBackend::moveToTrash(const QString &id)
{
    QMutexLocker locker(&m_mutex);
    if (!m_files.contains(id))
        return err(QStringLiteral("文件不存在"));

    setTrashedRecursive(id, true);
    save();
    return ok();
}

Ok MockBackend::restore(const QString &id)
{
    QMutexLocker locker(&m_mutex);
    if (!m_files.contains(id))
        return err(QStringLiteral("文件不存在"));

    setTrashedRecursive(id, false);
    save();
    return ok();
}

void MockBackend::setTrashedRecursive(const QString &id, bool trashed)
{
    auto it = m_files.find(id);
    if (it == m_files.end())
        return;

    FileItem  item       = it.value();
    item.trashed         = trashed;
    item.trashedAt       = trashed ? QDateTime::currentDateTimeUtc() : QDateTime();
    it.value()           = item;

    QVector<QString> children;
    for (auto c = m_files.constBegin(); c != m_files.constEnd(); ++c) {
        if (c.value().parentId == id && c.value().id != id)
            children.append(c.key());
    }
    for (const QString &cid : children)
        setTrashedRecursive(cid, trashed);
}

Ok MockBackend::purge(const QString &id)
{
    QMutexLocker locker(&m_mutex);
    if (!m_files.contains(id))
        return err(QStringLiteral("文件不存在"));

    deleteRecursive(id);
    save();
    return ok();
}

void MockBackend::deleteRecursive(const QString &id)
{
    const FileItem item = m_files.value(id);

    // 先处理子项
    QVector<QString> children;
    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
        if (it.value().parentId == id && it.value().id != id)
            children.append(it.key());
    }
    for (const QString &c : children)
        deleteRecursive(c);

    if (!item.isDir) {
        QFile::remove(blobPath(item.id));
        const QVector<FileVersion> vers = m_versions.value(item.id);
        for (const FileVersion &v : vers)
            QFile::remove(versionPath(item.id, v.version));
    }
    m_versions.remove(item.id);

    // 关联的分享链接一并失效
    for (auto it = m_shares.begin(); it != m_shares.end(); ++it) {
        if (it.value().fileId == item.id)
            m_shares.erase(it);
    }

    m_files.remove(id);
}

Ok MockBackend::emptyTrash()
{
    QMutexLocker locker(&m_mutex);

    QVector<QString> roots;
    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
        if (!it.value().trashed)
            continue;
        auto parent = m_files.constFind(it.value().parentId);
        if (parent == m_files.constEnd() || !parent.value().trashed)
            roots.append(it.key());
    }
    for (const QString &r : roots)
        deleteRecursive(r);

    save();
    return ok();
}

Ok MockBackend::setTags(const QString &id, const QStringList &tagIds)
{
    QMutexLocker locker(&m_mutex);

    auto it = m_files.find(id);
    if (it == m_files.end())
        return err(QStringLiteral("文件不存在"));

    it.value().tagIds = tagIds;
    save();
    return ok();
}

// =========================================================================
// 传输
// =========================================================================

Result<UploadTicket> MockBackend::beginUpload(const QString &parentId, const QString &name,
                                              qint64 size, const QString &sha256)
{
    QMutexLocker locker(&m_mutex);

    if (parentId != kRootId && !m_files.contains(parentId))
        return Result<UploadTicket>::fail(QStringLiteral("目标目录不存在"));

    UploadTicket ticket;

    // 秒传：同目录下同名同内容，或库里已有相同指纹的文件
    if (!sha256.isEmpty()) {
        for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
            const FileItem &f = it.value();
            if (f.trashed || f.isDir)
                continue;
            if (f.hash == sha256 && f.size == size) {
                if (f.parentId == parentId && f.name == name) {
                    ticket.instant = true;
                    ticket.fileId  = f.id;
                    return Result<UploadTicket>::success(ticket);
                }
            }
        }
    }

    const QString uploadId = Crypto::randomHex(8);
    const QString tempPath = AppPaths::cacheDir() + QStringLiteral("/uploads/") + uploadId
                           + QStringLiteral(".part");

    UploadSession session;
    session.uploadId = uploadId;
    session.parentId = parentId;
    session.name     = Util::sanitizeName(name);
    session.sha256   = sha256;
    session.size     = size;
    session.tempPath = tempPath;

    // 断点续传：临时文件已存在且未超过目标大小时，从已有长度继续
    QFileInfo tmpInfo(tempPath);
    if (tmpInfo.exists() && tmpInfo.size() <= size)
        session.received = tmpInfo.size();
    else
        QFile::remove(tempPath);

    m_uploads.insert(uploadId, session);

    ticket.uploadId = uploadId;
    ticket.received = session.received;
    return Result<UploadTicket>::success(ticket);
}

Ok MockBackend::putChunk(const QString &uploadId, qint64 offset, const QByteArray &data)
{
    QMutexLocker locker(&m_mutex);

    auto it = m_uploads.find(uploadId);
    if (it == m_uploads.end())
        return err(QStringLiteral("上传会话不存在"));

    QFile file(it.value().tempPath);
    if (!file.open(QIODevice::ReadWrite)) {
        return err(QStringLiteral("无法写入临时文件：%1").arg(file.errorString()));
    }
    if (!file.seek(offset)) {
        file.close();
        return err(QStringLiteral("分片偏移量错误"));
    }
    if (file.write(data) != data.size()) {
        file.close();
        return err(QStringLiteral("分片写入失败"));
    }
    file.close();

    it.value().received = qMax(it.value().received, offset + data.size());
    return ok();
}

Result<FileItem> MockBackend::finishUpload(const QString &uploadId)
{
    QMutexLocker locker(&m_mutex);

    auto it = m_uploads.find(uploadId);
    if (it == m_uploads.end())
        return Result<FileItem>::fail(QStringLiteral("上传会话不存在"));

    const UploadSession session = it.value();
    m_uploads.erase(it);

    QFileInfo tmpInfo(session.tempPath);
    if (!tmpInfo.exists())
        return Result<FileItem>::fail(QStringLiteral("上传数据已丢失，请重试"));

    const QString parentPath = session.parentId == kRootId
                                   ? kRootPath
                                   : m_files.value(session.parentId).path;
    const QString targetPath = Util::joinPath(parentPath, session.name);

    // 已存在同名文件 -> 生成新版本
    FileItem item;
    bool     exists = false;
    for (auto f = m_files.begin(); f != m_files.end(); ++f) {
        if (!f.value().trashed && f.value().path == targetPath) {
            item   = f.value();
            exists = true;
            break;
        }
    }

    if (exists) {
        if (item.isDir)
            return Result<FileItem>::fail(QStringLiteral("已存在同名文件夹"));
        snapshotVersion(item, QStringLiteral("上传新版本"));
    } else {
        item.id       = Crypto::randomHex(12);
        item.parentId = session.parentId;
        item.name     = session.name;
        item.path     = targetPath;
        item.version  = 0; // 下面统一 +1
    }

    const QString dest = blobPath(item.id);
    QFile::remove(dest);
    if (!QFile::rename(session.tempPath, dest)) {
        // 跨分区时退化为复制
        QFile src(session.tempPath);
        QFile dst(dest);
        if (!src.open(QIODevice::ReadOnly) || !dst.open(QIODevice::WriteOnly)
            || dst.write(src.readAll()) != tmpInfo.size()) {
            return Result<FileItem>::fail(QStringLiteral("保存文件失败"));
        }
    }

    item.isDir    = false;
    item.size     = tmpInfo.size();
    item.modified = QDateTime::currentDateTimeUtc();
    item.hash     = session.sha256.isEmpty() ? QString() : session.sha256;
    item.rev      = Crypto::randomHex(6);
    item.version += 1;
    item.type     = Util::fileTypeOf(item.name, false);
    item.device   = Util::deviceName();
    item.trashed  = false;

    m_files.insert(item.id, item);
    pruneVersions(item.id);
    save();

    return Result<FileItem>::success(item);
}

Ok MockBackend::cancelUpload(const QString &uploadId)
{
    QMutexLocker locker(&m_mutex);

    auto it = m_uploads.find(uploadId);
    if (it == m_uploads.end())
        return ok();

    QFile::remove(it.value().tempPath);
    m_uploads.erase(it);
    return ok();
}

Result<QByteArray> MockBackend::getRange(const QString &fileId, qint64 offset, qint64 length)
{
    QMutexLocker locker(&m_mutex);

    if (!m_files.contains(fileId))
        return Result<QByteArray>::fail(QStringLiteral("文件不存在"));

    QFile file(blobPath(fileId));
    if (!file.open(QIODevice::ReadOnly))
        return Result<QByteArray>::fail(QStringLiteral("读取失败：%1").arg(file.errorString()));
    if (!file.seek(offset))
        return Result<QByteArray>::fail(QStringLiteral("偏移量超出范围"));

    return Result<QByteArray>::success(file.read(length));
}

// =========================================================================
// 版本
// =========================================================================

void MockBackend::snapshotVersion(const FileItem &item, const QString &note)
{
    const QString src = blobPath(item.id);
    if (!QFileInfo::exists(src))
        return;

    const QString dst = versionPath(item.id, item.version);
    QFile::remove(dst);
    QFile::copy(src, dst);

    FileVersion v;
    v.id      = Crypto::randomHex(8);
    v.fileId  = item.id;
    v.version = item.version;
    v.size    = item.size;
    v.created = QDateTime::currentDateTimeUtc();
    v.device  = item.device.isEmpty() ? Util::deviceName() : item.device;
    v.note    = note;

    m_versions[item.id].append(v);
}

void MockBackend::pruneVersions(const QString &fileId)
{
    auto it = m_versions.find(fileId);
    if (it == m_versions.end())
        return;

    QVector<FileVersion> &list = it.value();

    // 按版本号降序，只保留最近 kMaxVersions 份
    std::sort(list.begin(), list.end(),
              [](const FileVersion &a, const FileVersion &b) { return a.version > b.version; });

    const QDateTime deadline = QDateTime::currentDateTimeUtc().addDays(-kVersionKeepDays);

    QVector<FileVersion> kept;
    for (const FileVersion &v : list) {
        if (kept.size() >= kMaxVersions || v.created < deadline) {
            QFile::remove(versionPath(fileId, v.version));
            continue;
        }
        kept.append(v);
    }
    list = kept;
}

Result<QVector<FileVersion>> MockBackend::versions(const QString &fileId)
{
    QMutexLocker locker(&m_mutex);

    auto it = m_files.constFind(fileId);
    if (it == m_files.constEnd())
        return Result<QVector<FileVersion>>::fail(QStringLiteral("文件不存在"));

    QVector<FileVersion> out;

    // 当前版本排在最前
    FileVersion current;
    current.id      = QStringLiteral("current");
    current.fileId  = fileId;
    current.version = it.value().version;
    current.size    = it.value().size;
    current.created = it.value().modified;
    current.device  = it.value().device;
    current.note    = QStringLiteral("当前版本");
    current.current = true;
    out.append(current);

    const QVector<FileVersion> history = m_versions.value(fileId);
    QVector<FileVersion>       sorted  = history;
    std::sort(sorted.begin(), sorted.end(),
              [](const FileVersion &a, const FileVersion &b) { return a.version > b.version; });
    for (const FileVersion &v : sorted)
        out.append(v);

    return Result<QVector<FileVersion>>::success(out);
}

Ok MockBackend::restoreVersion(const QString &fileId, const QString &versionId)
{
    QMutexLocker locker(&m_mutex);

    auto file = m_files.find(fileId);
    if (file == m_files.end())
        return err(QStringLiteral("文件不存在"));

    if (versionId == QStringLiteral("current"))
        return err(QStringLiteral("该版本已是当前版本"));

    const QVector<FileVersion> list = m_versions.value(fileId);
    FileVersion                target;
    for (const FileVersion &v : list) {
        if (v.id == versionId) {
            target = v;
            break;
        }
    }
    if (target.id.isEmpty())
        return err(QStringLiteral("版本不存在"));

    const QString src = versionPath(fileId, target.version);
    if (!QFileInfo::exists(src))
        return err(QStringLiteral("版本数据缺失"));

    // 恢复前先给当前内容留一份快照，恢复动作本身也计入版本历史
    snapshotVersion(file.value(), QStringLiteral("恢复前备份"));

    const QString dest = blobPath(fileId);
    QFile::remove(dest);
    if (!QFile::copy(src, dest))
        return err(QStringLiteral("恢复失败"));

    FileItem item = file.value();
    item.size     = target.size;
    item.modified = QDateTime::currentDateTimeUtc();
    item.version += 1;
    item.rev      = Crypto::randomHex(6);
    item.device   = Util::deviceName();
    file.value()  = item;

    pruneVersions(fileId);
    save();
    return ok();
}

// =========================================================================
// 分享
// =========================================================================

QString MockBackend::shareUrlFor(const QString &shareId) const
{
    return m_shareBase + QStringLiteral("/s/") + shareId;
}

Result<QVector<ShareLink>> MockBackend::shares()
{
    QMutexLocker locker(&m_mutex);

    QVector<ShareLink> out;
    for (auto it = m_shares.constBegin(); it != m_shares.constEnd(); ++it)
        out.append(it.value());

    std::sort(out.begin(), out.end(), [](const ShareLink &a, const ShareLink &b) {
        return a.created > b.created;
    });
    return Result<QVector<ShareLink>>::success(out);
}

Result<ShareLink> MockBackend::createShare(const QString &fileId, const QString &code,
                                           int expireDays, int maxDownloads)
{
    QMutexLocker locker(&m_mutex);

    auto it = m_files.constFind(fileId);
    if (it == m_files.constEnd())
        return Result<ShareLink>::fail(QStringLiteral("文件不存在"));

    ShareLink s;
    s.id           = Crypto::randomHex(10);
    s.fileId       = fileId;
    s.fileName     = it.value().name;
    s.isDir        = it.value().isDir;
    s.code         = code;
    s.created      = QDateTime::currentDateTimeUtc();
    s.expire       = expireDays > 0 ? s.created.addDays(expireDays) : QDateTime();
    s.maxDownloads = qMax(0, maxDownloads);
    s.url          = shareUrlFor(s.id);

    m_shares.insert(s.id, s);
    save();
    return Result<ShareLink>::success(s);
}

Ok MockBackend::revokeShare(const QString &shareId)
{
    QMutexLocker locker(&m_mutex);

    auto it = m_shares.find(shareId);
    if (it == m_shares.end())
        return err(QStringLiteral("分享不存在"));

    it.value().revoked = true;
    save();
    return ok();
}

Ok MockBackend::touchShare(const QString &shareId)
{
    QMutexLocker locker(&m_mutex);

    auto it = m_shares.find(shareId);
    if (it == m_shares.end())
        return err(QStringLiteral("分享不存在"));

    it.value().downloads += 1;
    save();
    return ok();
}

// =========================================================================
// 标签与检索
// =========================================================================

Result<QVector<Tag>> MockBackend::tags()
{
    QMutexLocker locker(&m_mutex);

    // 统计每个标签被使用的次数
    QHash<QString, int> usage;
    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
        if (it.value().trashed)
            continue;
        for (const QString &t : it.value().tagIds)
            usage[t] += 1;
    }

    QVector<Tag> out;
    for (auto it = m_tags.constBegin(); it != m_tags.constEnd(); ++it) {
        Tag t = it.value();
        t.count = usage.value(t.id, 0);
        out.append(t);
    }
    std::sort(out.begin(), out.end(), [](const Tag &a, const Tag &b) { return a.name < b.name; });
    return Result<QVector<Tag>>::success(out);
}

Result<Tag> MockBackend::createTag(const QString &name, const QString &color)
{
    QMutexLocker locker(&m_mutex);

    const QString clean = name.trimmed();
    if (clean.isEmpty())
        return Result<Tag>::fail(QStringLiteral("标签名不能为空"));

    for (auto it = m_tags.constBegin(); it != m_tags.constEnd(); ++it) {
        if (it.value().name.compare(clean, Qt::CaseInsensitive) == 0)
            return Result<Tag>::fail(QStringLiteral("标签已存在"));
    }

    Tag t;
    t.id    = Crypto::randomHex(6);
    t.name  = clean;
    t.color = color.isEmpty() ? QStringLiteral("#4C6FFF") : color;

    m_tags.insert(t.id, t);
    save();
    return Result<Tag>::success(t);
}

Ok MockBackend::deleteTag(const QString &tagId)
{
    QMutexLocker locker(&m_mutex);
    m_tags.remove(tagId);

    for (auto it = m_files.begin(); it != m_files.end(); ++it)
        it.value().tagIds.removeAll(tagId);

    save();
    return ok();
}

Result<QVector<FileItem>> MockBackend::search(const QString &keyword, const QStringList &tagIds,
                                              int typeFilter)
{
    QMutexLocker locker(&m_mutex);

    const QString key = keyword.trimmed().toLower();

    QVector<FileItem> out;
    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
        const FileItem &f = it.value();
        if (f.trashed)
            continue;

        if (!key.isEmpty() && !f.name.toLower().contains(key)
            && !f.path.toLower().contains(key))
            continue;

        if (typeFilter >= 0 && int(f.type) != typeFilter)
            continue;

        if (!tagIds.isEmpty()) {
            bool hasAll = true;
            for (const QString &t : tagIds) {
                if (!f.tagIds.contains(t)) {
                    hasAll = false;
                    break;
                }
            }
            if (!hasAll)
                continue;
        }

        out.append(f);
    }

    std::sort(out.begin(), out.end(), [](const FileItem &a, const FileItem &b) {
        return a.modified > b.modified;
    });
    return Result<QVector<FileItem>>::success(out);
}

// =========================================================================
// 统计
// =========================================================================

Result<UsageStats> MockBackend::usage()
{
    QMutexLocker locker(&m_mutex);

    UsageStats stats;

    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
        const FileItem &f = it.value();

        if (f.trashed) {
            stats.trash += f.size;
            continue;
        }
        if (f.isDir) {
            stats.folderCount += 1;
            continue;
        }

        stats.used += f.size;
        stats.fileCount += 1;
    }

    for (auto it = m_versions.constBegin(); it != m_versions.constEnd(); ++it)
        for (const FileVersion &v : it.value())
            stats.versions += v.size;

    stats.shareCount = m_shares.size();

    // 按类型聚合
    QMap<int, qint64> byType;
    QVector<QPair<QString, qint64>> largest;
    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
        const FileItem &f = it.value();
        if (f.trashed || f.isDir)
            continue;
        byType[int(f.type)] += f.size;
        largest.append(qMakePair(f.name, f.size));
    }
    for (auto it = byType.constBegin(); it != byType.constEnd(); ++it)
        stats.byType.append(qMakePair(Util::typeName(it.key()), it.value()));

    std::sort(largest.begin(), largest.end(),
              [](const QPair<QString, qint64> &a, const QPair<QString, qint64> &b) {
                  return a.second > b.second;
              });
    stats.largest = largest.mid(0, 10);

    const QStorageInfo storage(m_dataDir);
    stats.total = storage.bytesTotal() > 0 ? qint64(storage.bytesTotal()) : m_user.quota;
    if (stats.total <= 0)
        stats.total = stats.used * 2 + qint64(1) * 1024 * 1024 * 1024; // 兜底，避免除零

    return Result<UsageStats>::success(stats);
}

} // namespace cv

/****************************************************************************
 * net/Backend.h —— 数据源抽象接口
 *
 * 上层（controllers / sync / transfer）只依赖这个接口：
 *   MockBackend  本地引擎，数据存在本机磁盘，开箱即用
 *   HttpBackend  远程私有云服务器，走 REST
 * 两者可无缝替换，界面层零改动。
 *
 * 所有方法都是同步阻塞的，调用方负责放到线程池执行。
 ****************************************************************************/
#pragma once

#include "core/Types.h"

#include <QByteArray>
#include <QObject>
#include <QStringList>

namespace cv {

class Backend : public QObject
{
    Q_OBJECT
public:
    explicit Backend(QObject *parent = nullptr) : QObject(parent) {}
    ~Backend() override = default;

    virtual QString displayName() const = 0; // 展示用："本地引擎" / "远程服务"
    virtual bool    isLocal() const = 0;

    // ---------------- 账户 ----------------
    virtual Result<UserInfo> login(const QString &user, const QString &password) = 0;
    virtual void             logout() = 0;
    virtual UserInfo         currentUser() const = 0;

    // ---------------- 目录与文件 ----------------
    virtual Result<QVector<FileItem>> listFolder(const QString &parentId) = 0;
    virtual Result<QVector<FileItem>> listUnderPath(const QString &remotePath) = 0; // 含子目录
    virtual Result<QVector<FileItem>> listTrash() = 0;
    virtual Result<FileItem>          statById(const QString &id) = 0;
    virtual Result<FileItem>          statByPath(const QString &remotePath) = 0;
    virtual Result<FileItem>          createFolder(const QString &parentId, const QString &name) = 0;
    virtual Result<FileItem>          ensureFolderPath(const QString &remotePath) = 0;
    virtual Ok                        rename(const QString &id, const QString &newName) = 0;
    virtual Ok                        moveToTrash(const QString &id) = 0;
    virtual Ok                        restore(const QString &id) = 0;
    virtual Ok                        purge(const QString &id) = 0;
    virtual Ok                        emptyTrash() = 0;
    virtual Ok                        setTags(const QString &id, const QStringList &tagIds) = 0;

    // ---------------- 传输（分块 / 秒传 / 断点续传）----------------
    virtual Result<UploadTicket> beginUpload(const QString &parentId, const QString &name,
                                             qint64 size, const QString &sha256) = 0;
    virtual Ok                   putChunk(const QString &uploadId, qint64 offset,
                                          const QByteArray &data) = 0;
    virtual Result<FileItem>     finishUpload(const QString &uploadId) = 0;
    virtual Ok                   cancelUpload(const QString &uploadId) = 0;
    virtual Result<QByteArray>   getRange(const QString &fileId, qint64 offset, qint64 length) = 0;

    // ---------------- 版本 ----------------
    virtual Result<QVector<FileVersion>> versions(const QString &fileId) = 0;
    virtual Ok                           restoreVersion(const QString &fileId,
                                                        const QString &versionId) = 0;

    // ---------------- 分享 ----------------
    virtual Result<QVector<ShareLink>> shares() = 0;
    virtual Result<ShareLink>          createShare(const QString &fileId, const QString &code,
                                                   int expireDays, int maxDownloads) = 0;
    virtual Ok                         revokeShare(const QString &shareId) = 0;
    virtual Ok                         touchShare(const QString &shareId) = 0; // 下载计数

    // ---------------- 标签与检索 ----------------
    virtual Result<QVector<Tag>> tags() = 0;
    virtual Result<Tag>          createTag(const QString &name, const QString &color) = 0;
    virtual Ok                   deleteTag(const QString &tagId) = 0;
    // typeFilter: -1 全部，其余见 FileType
    virtual Result<QVector<FileItem>> search(const QString &keyword, const QStringList &tagIds,
                                             int typeFilter) = 0;

    // ---------------- 统计 ----------------
    virtual Result<UsageStats> usage() = 0;
};

} // namespace cv

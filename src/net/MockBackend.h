/****************************************************************************
 * net/MockBackend.h —— 本地引擎
 *
 * 把"服务端"实现在客户端进程内：文件实体存在本机数据目录，元数据落在
 * db.json。它既是开发期的模拟后端，也是"单机私有云"模式的真实实现——
 * 装在自己电脑上就能直接用，后续接入远程服务器时替换为 HttpBackend 即可。
 ****************************************************************************/
#pragma once

#include "Backend.h"

#include <QHash>
#include <QMutex>

namespace cv {

class MockBackend : public Backend
{
    Q_OBJECT
public:
    explicit MockBackend(QObject *parent = nullptr);
    ~MockBackend() override;

    QString displayName() const override { return QStringLiteral("本地引擎"); }
    bool    isLocal() const override { return true; }

    Result<UserInfo> login(const QString &user, const QString &password) override;
    void             logout() override;
    UserInfo         currentUser() const override;

    Result<QVector<FileItem>> listFolder(const QString &parentId) override;
    Result<QVector<FileItem>> listUnderPath(const QString &remotePath) override;
    Result<QVector<FileItem>> listTrash() override;
    Result<FileItem>          statById(const QString &id) override;
    Result<FileItem>          statByPath(const QString &remotePath) override;
    Result<FileItem>          createFolder(const QString &parentId, const QString &name) override;
    Result<FileItem>          ensureFolderPath(const QString &remotePath) override;
    Ok                        rename(const QString &id, const QString &newName) override;
    Ok                        moveToTrash(const QString &id) override;
    Ok                        restore(const QString &id) override;
    Ok                        purge(const QString &id) override;
    Ok                        emptyTrash() override;
    Ok                        setTags(const QString &id, const QStringList &tagIds) override;

    Result<UploadTicket> beginUpload(const QString &parentId, const QString &name, qint64 size,
                                     const QString &sha256) override;
    Ok                   putChunk(const QString &uploadId, qint64 offset,
                                  const QByteArray &data) override;
    Result<FileItem>     finishUpload(const QString &uploadId) override;
    Ok                   cancelUpload(const QString &uploadId) override;
    Result<QByteArray>   getRange(const QString &fileId, qint64 offset, qint64 length) override;

    Result<QVector<FileVersion>> versions(const QString &fileId) override;
    Ok                           restoreVersion(const QString &fileId,
                                                const QString &versionId) override;

    Result<QVector<ShareLink>> shares() override;
    Result<ShareLink>          createShare(const QString &fileId, const QString &code,
                                           int expireDays, int maxDownloads) override;
    Ok                         revokeShare(const QString &shareId) override;
    Ok                         touchShare(const QString &shareId) override;

    Result<QVector<Tag>> tags() override;
    Result<Tag>          createTag(const QString &name, const QString &color) override;
    Ok                   deleteTag(const QString &tagId) override;
    Result<QVector<FileItem>> search(const QString &keyword, const QStringList &tagIds,
                                     int typeFilter) override;

    Result<UsageStats> usage() override;

private:
    struct UploadSession
    {
        QString uploadId;
        QString parentId;
        QString name;
        QString sha256;
        QString tempPath;
        qint64  size     = 0;
        qint64  received = 0;
    };

    void load();
    void save() const;

    QString blobPath(const QString &fileId) const;
    QString versionPath(const QString &fileId, int version) const;

    void renameRecursive(const QString &id, const QString &newPath);
    void collectDescendants(const QString &id, QVector<FileItem> &out);
    void setTrashedRecursive(const QString &id, bool trashed);
    void deleteRecursive(const QString &id);
    void snapshotVersion(const FileItem &item, const QString &note);
    void pruneVersions(const QString &fileId);
    QString shareUrlFor(const QString &shareId) const;

    QHash<QString, FileItem>              m_files;
    QHash<QString, QVector<FileVersion>>  m_versions;
    QHash<QString, ShareLink>             m_shares;
    QHash<QString, Tag>                   m_tags;
    QHash<QString, UploadSession>         m_uploads;
    UserInfo                              m_user;
    QString                               m_dataDir;
    QString                               m_blobDir;
    QString                               m_shareBase; // 生成分享链接用的服务地址
    mutable QMutex                        m_mutex;
};

} // namespace cv

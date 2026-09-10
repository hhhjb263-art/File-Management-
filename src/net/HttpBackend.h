/****************************************************************************
 * net/HttpBackend.h —— 远程私有云服务器（REST）
 *
 * 与 MockBackend 实现同一套 Backend 接口，界面与同步逻辑无需改动。
 * 接口约定见 HttpBackend.cpp 顶部的端点清单。
 ****************************************************************************/
#pragma once

#include "Backend.h"

#include <QByteArray>
#include <QHash>
#include <QNetworkAccessManager>

namespace cv {

class HttpBackend : public Backend
{
    Q_OBJECT
public:
    explicit HttpBackend(const QString &baseUrl, QObject *parent = nullptr);

    QString displayName() const override { return QStringLiteral("远程服务"); }
    bool    isLocal() const override { return false; }

    void    setBaseUrl(const QString &url);
    QString baseUrl() const { return m_baseUrl; }
    void    setToken(const QString &token);
    QString token() const { return m_token; }

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
    enum class Method { Get, Post, Put, Delete };

    struct Response
    {
        int        status = 0;
        QByteArray body;
        QString    error;

        bool ok() const { return error.isEmpty() && status >= 200 && status < 300; }
    };

    Response request(Method method, const QString &path, const QByteArray &body = {},
                     const QHash<QByteArray, QByteArray> &headers = {});
    QNetworkAccessManager *nam(); // 每线程一个实例

    QString m_baseUrl;
    QString m_token;
    UserInfo m_user;
};

} // namespace cv

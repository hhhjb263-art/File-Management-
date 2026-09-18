/****************************************************************************
 * net/HttpBackend.h —— 远程私有云服务器（真实 REST 契约，路径前缀 /api/v1）
 *
 * 与 MockBackend 实现同一套 Backend 抽象接口，界面与同步逻辑无需改动。
 * 具体端点清单见 HttpBackend.cpp 顶部注释（与 server/src/app/main.cpp 的
 * server.route(...) 一一对应）。
 *
 * 说明（保持 Backend.h 抽象接口不变的前提下）：
 *   · 服务端没有 ID 体系，只有 `dir` 相对路径；parentId 语义映射为 dir 路径。
 *   · 服务端暂不支持的能力（回收站 / 版本 / 分享 / 标签 / 检索 / 用户体系 …）
 *     一律 **不发网络请求**，直接返回带 [unsupported] 前缀的失败结果。
 *   · 提供少量附加能力（探活 / 整文件上传 / 分块会话查询 / TLS TOFU 挂钩），
 *     均为「新增」而非改动既有签名，既有调用方零影响。
 *   · TLS 自签名（TOFU）：本类**不依赖 QtWidgets、不弹窗**。首次连接的确认由界面层通过
 *     setTrustPrompt() 注入回调完成；未注入回调时一律 **fail-closed（拒绝连接）**，
 *     绝不静默信任（不存在"勾选即忽略一切证书错误"）。指纹持久化在
 *     AppPaths::settingsFile()（settings.ini，IniFormat）的键 `pinnedFingerprint/<host:port>`；
 *     与 client/ 参考实现**不共享**该记录（存储文件不同），属预期行为。
 *   · 同名覆盖：整文件上传用 uploadWholeFile(..., overwrite)；分块上传用
 *     setUploadOverwrite(true)（Backend 加法式接口）。
 *   · 分块大小：以 chunkSize() 为**唯一来源**（init 与上层推导 offset→seq 都用它），
 *     不再读取 Settings::chunkSizeMB()，避免两端失配导致 putChunk 被服务端 400。
 *     上层**不得硬编码**块大小。
 ****************************************************************************/
#pragma once

#include "Backend.h"

#include <QByteArray>
#include <atomic>

#include <QHash>
#include <QList>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSslError>
#include <QStringList>
#include <functional>
#include <utility>

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

    // ======================= 附加能力（非抽象接口） =======================

    // 探活：GET /healthz（免鉴权）。成功返回 success(true)。
    Result<bool> health();

    // 整文件上传：POST /api/v1/files（含同名覆盖）。
    // Backend 抽象接口未提供整文件上传，此处提供以供上层按需调用。
    // overwrite=false 且同目录同名 → 返回带 [conflict] 前缀的失败（可被 isNameConflict 识别）。
    Result<FileItem> uploadWholeFile(const QString &parentId, const QString &name,
                                     const QByteArray &data, bool overwrite = false);

    // 上传同名文件是否覆盖（Backend 加法式接口的实现）。
    // 为真时：整文件上传加头 X-CV-Overwrite: 1；分块 init body 带 "overwrite": true。
    // 只对随后的一次上传生效，用后由调用方复位（见 Backend.h 约定）。
    void setUploadOverwrite(bool enabled) override;
    bool uploadOverwrite() const { return m_uploadOverwrite.load(); }

    // 分块上传块大小（字节）——唯一来源。init 与上层推导 offset→seq 都以此为准，
    // 不再读取 Settings::chunkSizeMB()，避免两端失配导致 putChunk 被服务端 400。
    qint64 chunkSize() const override;

    // 分块会话状态：GET /api/v1/uploads/:id（供断点续传：拿到已传分块 seq 列表）。
    struct UploadSessionInfo
    {
        QString      uploadId;
        qint64       size      = 0;
        qint64       chunkSize = 0;
        qint64       received  = 0;
        QString      status;
        QVector<int> uploaded; // 已收分块序号（升序）
    };
    Result<UploadSessionInfo> uploadSession(const QString &uploadId);

    // ---- TLS 自签名证书（TOFU 指纹固定）----
    // 对应界面开关「允许使用自签名证书（首次需确认）」：关闭时交给 Qt 标准证书链校验。
    void setTrustSelfSigned(bool on) { m_trustSelfSigned = on; }
    bool trustSelfSigned() const { return m_trustSelfSigned; }

    // 首次连接需要人工确认：由界面层注入该回调（在发起请求的线程内同步调用）。
    // 返回 true = 信任并记住该主机证书指纹；false = 拒绝本次连接。
    // 未注入回调时，「首次连接」一律按拒绝处理（fail-closed，绝不静默信任）。
    using TrustPrompt = std::function<bool(const QString &hostPort,
                                           const QString &fingerprintView,
                                           const QString &subject,
                                           const QString &issuer,
                                           const QString &validity)>;
    void setTrustPrompt(TrustPrompt prompt) { m_trustPrompt = std::move(prompt); }

    QString pinnedFingerprint(const QString &hostPort) const;
    void    clearPinnedFingerprint(const QString &hostPort);

    // ---- 失败结果的可识别性（约定前缀）----
    // 上层据此给出「该功能暂不支持 / 同名冲突，是否覆盖」等精确提示。
    static bool isUnsupported(const QString &error);
    static bool isNameConflict(const QString &error);
    static bool isMissingChunks(const QString &error);
    static bool isInvalidChunks(const QString &error);
    static bool isPinMismatch(const QString &error);   // [pin-mismatch] 指纹与已固定记录不一致

    // ======================= Backend 抽象接口实现 =======================

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

    // TLS：在 QNetworkReply::sslErrors 上做 TOFU 指纹固定
    void applyCertPinning(QNetworkReply *reply, const QList<QSslError> &errors);

    // 分块 offset -> seq 需要知道分块大小：会话创建时记住，缺失时回查服务端
    qint64 chunkSizeFor(const QString &uploadId);
    void   rememberChunkSize(const QString &uploadId, qint64 chunkSize);
    void   forgetChunkSize(const QString &uploadId);

    QString m_baseUrl;
    QString m_token;
    UserInfo m_user;

    bool        m_trustSelfSigned = true;
    TrustPrompt m_trustPrompt;
    // 下次上传是否覆盖同名（由调用方置真/复位）。跨线程读写（传输 worker 写、主线程读）→ 原子
    std::atomic<bool> m_uploadOverwrite{false};

    QHash<QString, qint64> m_uploadChunkSize; // uploadId -> chunk_size
    mutable QMutex         m_chunkSizeMutex;
};

} // namespace cv

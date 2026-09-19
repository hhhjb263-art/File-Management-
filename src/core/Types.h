/****************************************************************************
 * core/Types.h —— 全局数据结构与枚举
 *
 * 这一层不依赖任何 Qt Widgets / QML，供 core / net / sync / transfer /
 * data / controllers 共同引用。
 ****************************************************************************/
#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>
#include <utility>

namespace cv {

// 云端根目录常量
inline const QString kRootId = QStringLiteral("root");
inline const QString kRootPath = QStringLiteral("/");

// ============================ 文件 ============================

enum class FileType {
    Folder = 0, // 文件夹
    Document,   // 文档（pdf/word/excel/ppt/txt/md...）
    Image,      // 图片
    Video,      // 视频
    Audio,      // 音频
    Archive,    // 压缩包
    Code,       // 代码 / 文本源
    Other       // 其它
};

struct FileItem {
    QString    id;        // 唯一 ID
    QString    parentId;  // 父目录 ID（根目录为 kRootId）
    QString    path;      // 云端完整路径，例如 "/照片/2026/a.jpg"
    QString    name;      // 文件或文件夹名
    bool       isDir = false;
    qint64     size = 0;             // 字节（目录为 0，聚合值由统计模块计算）
    QDateTime  modified;             // 最后修改时间（UTC）
    QString    hash;                 // SHA-256，目录为空
    QString    rev;                  // 服务端版本号，内容变化则递增
    int        version = 1;          // 当前版本号（第几版）
    QStringList tagIds;              // 标签 ID 列表
    FileType   type = FileType::Other;
    bool       trashed = false;      // 是否在回收站
    QDateTime  trashedAt;
    QString    device;               // 最后修改来源设备名

    bool isValid() const { return !id.isEmpty(); }
};

// ============================ 版本 ============================

struct FileVersion {
    QString   id;
    QString   fileId;
    int       version = 1;      // 1 为最初版本
    qint64    size = 0;
    QDateTime created;
    QString   device;
    QString   note;             // 备注：上传 / 同步 / 恢复
    bool      current = false;  // 是否当前版本
};

// ============================ 分享 ============================

struct ShareLink {
    QString   id;
    QString   fileId;
    QString   fileName;
    bool      isDir = false;
    QString   url;              // 完整访问地址
    QString   code;             // 提取码（空表示无密码）
    QDateTime created;
    QDateTime expire;           // 无效表示永久有效
    int       maxDownloads = 0; // 0 表示不限次数
    int       downloads = 0;
    bool      revoked = false;
    // 追加（加法式）：服务端列表返回的字段，供展示 / needCode 判定
    qint64    size = 0;         // 文件字节数（用于 sizeText）
    bool      needCode = false; // 是否启用提取码（列表不含明文 code，用此判定）

    bool expired() const { return expire.isValid() && expire < QDateTime::currentDateTimeUtc(); }
    bool usable() const
    {
        return !revoked && !expired() && (maxDownloads <= 0 || downloads < maxDownloads);
    }
};

// ============================ 标签 ============================

struct Tag {
    QString id;
    QString name;
    QString color; // #RRGGBB
    int     count = 0;
};

// ============================ 同步 ============================

enum class SyncMode {
    TwoWay = 0,   // 双向同步
    UploadOnly,   // 单向备份：本地 -> 云端
    DownloadOnly  // 单向镜像：云端 -> 本地
};

enum class SyncStatus {
    Idle = 0,   // 空闲（已同步）
    Watching,   // 监听中
    Syncing,    // 同步中
    Conflict,   // 有冲突副本产生
    Error,      // 出错
    Paused,     // 已暂停
    Disabled    // 未启用
};

struct SyncPair {
    QString    id;
    QString    localPath;   // 本机目录
    QString    remotePath;  // 云端目录
    SyncMode   mode = SyncMode::TwoWay;
    bool       enabled = true;
    SyncStatus status = SyncStatus::Idle;
    QString    lastError;
    QDateTime  lastSync;
    int        pendingCount = 0; // 上次同步处理的文件数
    int        conflictCount = 0;
};

// ============================ 传输 ============================

enum class TransferKind { Upload = 0, Download };

enum class TransferState {
    Queued = 0, // 排队中
    Hashing,    // 计算指纹（秒传判定）
    Running,    // 传输中
    Paused,     // 已暂停
    Completed,  // 完成
    Failed,     // 失败
    Canceled    // 已取消
};

struct TransferTask {
    QString       id;
    TransferKind  kind = TransferKind::Upload;
    QString       fileName;
    QString       localPath;   // 本机绝对路径
    QString       remoteId;    // 云端目录 ID（上传）或文件 ID（下载）
    QString       remotePath;  // 云端目标位置（展示用）
    qint64        total = 0;
    qint64        done = 0;
    TransferState state = TransferState::Queued;
    double        bytesPerSec = 0;
    QString       error;
    QDateTime     started;
    bool          instant = false; // 是否命中秒传

    int percent() const { return total > 0 ? int(done * 100 / total) : 0; }
    bool finished() const
    {
        return state == TransferState::Completed || state == TransferState::Failed
            || state == TransferState::Canceled;
    }
};

// 分块上传票据：beginUpload 的返回值
struct UploadTicket {
    bool    instant = false; // 命中秒传，无需再传数据
    QString uploadId;        // 本次上传会话 ID（断点续传依据）
    qint64  received = 0;    // 服务端已收到的字节数，续传从此处开始
    QString fileId;          // 秒传成功时的文件 ID
};

// ============================ 统计 ============================

struct UsageStats {
    qint64 total = 0;       // 总容量（字节）
    qint64 used = 0;        // 文件占用
    qint64 versions = 0;    // 历史版本占用
    qint64 trash = 0;       // 回收站占用
    qint64 fileCount = 0;
    qint64 folderCount = 0;
    qint64 shareCount = 0;
    QVector<QPair<QString, qint64>> byType;   // {类型名, 字节}
    QVector<QPair<QString, qint64>> largest;  // {文件名, 字节}
};

// ============================ 账户 ============================

struct UserInfo {
    QString id;
    QString name;
    QString email;
    qint64  quota = 0; // 配额字节数，0 表示不限
};

// ============================ 通用返回值 ============================

template <typename T>
struct Result {
    bool    ok = false;
    QString error;
    T       value{};

    static Result success(T v)
    {
        Result r;
        r.ok = true;
        r.value = std::move(v);
        return r;
    }
    static Result fail(const QString &e)
    {
        Result r;
        r.ok = false;
        r.error = e;
        return r;
    }
};

// 无返回值的操作统一用 Result<bool>
using Ok = Result<bool>;
inline Ok ok() { return Ok::success(true); }
inline Ok err(const QString &e) { return Ok::fail(e); }

} // namespace cv

Q_DECLARE_METATYPE(cv::FileItem)
Q_DECLARE_METATYPE(cv::FileVersion)
Q_DECLARE_METATYPE(cv::ShareLink)
Q_DECLARE_METATYPE(cv::Tag)
Q_DECLARE_METATYPE(cv::SyncPair)
Q_DECLARE_METATYPE(cv::TransferTask)
Q_DECLARE_METATYPE(cv::UsageStats)
Q_DECLARE_METATYPE(cv::UserInfo)
Q_DECLARE_METATYPE(QVector<cv::FileItem>)
Q_DECLARE_METATYPE(QVector<cv::FileVersion>)
Q_DECLARE_METATYPE(QVector<cv::ShareLink>)
Q_DECLARE_METATYPE(QVector<cv::Tag>)
Q_DECLARE_METATYPE(QVector<cv::SyncPair>)

#pragma once

#include <QObject>
#include <QString>

class QQmlApplicationEngine;

namespace cv {
class Backend;
class TransferManager;
class FileListModel;
class TransferModel;
class TagModel;
class VersionModel;
class SyncPairModel;
class LocalIndex;
class AppController;
class AuthController;
class FileController;
class SyncController;
class TransferController;
class ShareController;
class StatsController;
class TrashController;
class TagController;
class VersionController;
}

// 应用装配（依赖注入的唯一位置）：
//   Settings -> Backend(HttpBackend/MockBackend) -> TransferManager -> Controllers/Models -> QML 上下文
// 同时承担 net 层「同步 TOFU 回调」与 QML「异步确认框」之间的桥接。
class Application : public QObject
{
    Q_OBJECT
public:
    explicit Application(bool useMock = false, QObject *parent = nullptr);
    ~Application() override;

    void start();
    void registerContext(QQmlApplicationEngine *engine);

    // 加法式访问器：暴露传输数据模型，供 main.cpp 的自检钩子（CV_SELFTEST_SEED_HISTORY）
    // 种入假历史。仅返回指针，不改变任何既有装配 / 语义。
    cv::TransferModel *transferModel() const { return m_transferModel; }

    // 说明：TLS 证书确认（TOFU）由 AppController 全权负责（它持有 fail-closed 守卫，
    // 且 API 已冻结在 docs/整合实现契约.md §8.3）。装配层**不得**再次注入信任回调——
    // HttpBackend::setTrustPrompt 是赋值语义，重复注入会互相覆盖，导致 QML 只连到其中一条
    // 而另一条成为死路径（这正是曾经出现的缺陷）。

private:
    // 把默认下载目录从 Settings 注入 TransferManager（启动时 + 设置变更后）
    void applyDownloadDirSetting();

    cv::Backend          *m_backend = nullptr;
    cv::TransferManager  *m_transfer = nullptr;
    cv::FileListModel    *m_fileModel = nullptr;
    cv::TransferModel    *m_transferModel = nullptr;
    cv::TagModel         *m_tagModel = nullptr;
    cv::VersionModel     *m_versionModel = nullptr;
    cv::FileListModel    *m_trashModel = nullptr;  // 回收站专用（独立于文件页模型）
    cv::SyncPairModel    *m_syncPairs = nullptr;   // 同步对列表（同步页专用）
    cv::LocalIndex       *m_syncIndex = nullptr;   // 同步索引（SQLite，记上次两端一致状态）
    cv::AppController    *m_app = nullptr;
    cv::AuthController   *m_auth = nullptr;
    cv::FileController   *m_files = nullptr;
    cv::SyncController   *m_sync = nullptr;
    cv::TransferController *m_transfers = nullptr;
    cv::ShareController  *m_shares = nullptr;
    cv::StatsController  *m_stats = nullptr;
    cv::TrashController  *m_trash = nullptr;
    cv::TagController    *m_tags = nullptr;
    cv::VersionController *m_versions = nullptr;
};

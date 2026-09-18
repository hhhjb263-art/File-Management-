#include "Application.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "../controllers/AppController.h"
#include "../controllers/AuthController.h"
#include "../controllers/FileController.h"
#include "../controllers/ShareController.h"
#include "../controllers/StatsController.h"
#include "../controllers/SyncController.h"
#include "../controllers/TransferController.h"
#include "../core/Settings.h"
#include "../data/FileListModel.h"
#include "../data/TransferModel.h"
#include "../net/HttpBackend.h"
#include "../net/MockBackend.h"
#include "../transfer/TransferManager.h"

Application::Application(bool useMock, QObject *parent) : QObject(parent)
{
    if (useMock) {
        m_backend = new cv::MockBackend(this);
    } else {
        // 连接配置来自 Settings（与设置页共享同一份持久化）
        cv::Settings &s = cv::Settings::instance();
        auto *http = new cv::HttpBackend(s.serverUrl(), this);
        http->setToken(s.token());
        http->setTrustSelfSigned(true);
        // TOFU 回调由 AppController 在构造时注入（单一路径，见 Application.h 顶部说明）
        m_backend = http;
    }
}

Application::~Application() = default;

void Application::start()
{
    m_fileModel = new cv::FileListModel(this);
    m_transferModel = new cv::TransferModel(this);
    m_transfer = new cv::TransferManager(m_backend, this);

    m_app = new cv::AppController(m_backend, this);
    m_auth = new cv::AuthController(this);
    m_files = new cv::FileController(m_backend, m_fileModel, this);
    m_sync = new cv::SyncController(this);
    m_transfers = new cv::TransferController(m_transfer, this);
    // 把队列进度镜像到 QML 数据模型（否则传输页/侧栏永远空白）
    m_transfers->setTaskModel(m_transferModel);
    m_shares = new cv::ShareController(this);
    m_stats = new cv::StatsController(m_backend, this);
}

void Application::registerContext(QQmlApplicationEngine *engine)
{
    if (!engine) {
        return;
    }
    QQmlContext *ctx = engine->rootContext();
    ctx->setContextProperty(QStringLiteral("Core"), this);          // TOFU 桥 + 全局
    ctx->setContextProperty(QStringLiteral("App"), m_app);
    ctx->setContextProperty(QStringLiteral("Files"), m_files);
    ctx->setContextProperty(QStringLiteral("Transfers"), m_transfers);
    ctx->setContextProperty(QStringLiteral("Stats"), m_stats);
    ctx->setContextProperty(QStringLiteral("Auth"), m_auth);
    ctx->setContextProperty(QStringLiteral("Sync"), m_sync);
    ctx->setContextProperty(QStringLiteral("Shares"), m_shares);
    ctx->setContextProperty(QStringLiteral("FileModel"), m_fileModel);
    ctx->setContextProperty(QStringLiteral("TransferModel"), m_transferModel);
}


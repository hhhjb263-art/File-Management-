#include "Application.h"

#include <QDebug>
#include <QLoggingCategory>
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

namespace {

// 控制器的 logMessage(level, text) 信号 → 统一日志 sink。
// Logging::install() 已 qInstallMessageHandler，会把 qInfo/qWarning/qCritical 同时写
// stderr 与 cloudvault.log；level 映射：ERROR→qCritical，WARN→qWarning，其余→qInfo。
void logToSink(const char *tag, const QString &level, const QString &text)
{
    const QString l = level.trimmed().toUpper();
    const QString line = QStringLiteral("[%1] %2").arg(QString::fromLatin1(tag), text);
    if (l == QLatin1String("ERROR") || l == QLatin1String("ERR") || l == QLatin1String("CRITICAL"))
        qCritical().noquote() << line;
    else if (l == QLatin1String("WARN") || l == QLatin1String("WARNING"))
        qWarning().noquote() << line;
    else
        qInfo().noquote() << line;
}

} // namespace

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
    m_auth = new cv::AuthController(m_backend, m_app, this);
    m_files = new cv::FileController(m_backend, m_fileModel, this);
    m_sync = new cv::SyncController(this);
    m_transfers = new cv::TransferController(m_transfer, this);
    // 把队列进度镜像到 QML 数据模型（否则传输页/侧栏永远空白）
    m_transfers->setTaskModel(m_transferModel);
    m_shares = new cv::ShareController(m_backend, this);
    m_stats = new cv::StatsController(m_backend, this);

    // ---- 日志接线：把各控制器 logMessage(level,text) 接到统一日志 sink ----
    // 修复：此前 FileController/AppController/TransferController/StatsController 的 logMessage
    // **全无消费者**（TransferController 只是把 TransferManager 的信号转发成自己同样无人订阅的信号），
    // 导致"列出目录失败"等关键信息永远进不了 cloudvault.log。Logging 已 qInstallMessageHandler，
    // 这里把 qCritical/qWarning/qInfo 落到 stderr + 日志文件。
    // 不重复：每路信号只在此接一次；TransferManager 的日志经 TransferController 转发这一条路径进入。
    QObject::connect(m_files, &cv::FileController::logMessage, this,
                     [](const QString &lv, const QString &tx) { logToSink("Files", lv, tx); });
    QObject::connect(m_app, &cv::AppController::logMessage, this,
                     [](const QString &lv, const QString &tx) { logToSink("App", lv, tx); });
    QObject::connect(m_transfers, &cv::TransferController::logMessage, this,
                     [](const QString &lv, const QString &tx) { logToSink("Transfer", lv, tx); });
    QObject::connect(m_stats, &cv::StatsController::logMessage, this,
                     [](const QString &lv, const QString &tx) { logToSink("Stats", lv, tx); });
    QObject::connect(m_shares, &cv::ShareController::logMessage, this,
                     [](const QString &lv, const QString &tx) { logToSink("Shares", lv, tx); });
    QObject::connect(m_auth, &cv::AuthController::logMessage, this,
                     [](const QString &lv, const QString &tx) { logToSink("Auth", lv, tx); });

    // ---- 会话过期 / 切换服务器 → 重置登录态 ----
    // HttpBackend 收到 401 且持有令牌时发 unauthorized() → Auth 清令牌 + loggedOut("expired")。
    if (auto *hb = qobject_cast<cv::HttpBackend *>(m_backend))
        QObject::connect(hb, &cv::HttpBackend::unauthorized,
                         m_auth, &cv::AuthController::onUnauthorized);
    // 切换服务器地址 → 清空会话令牌与登录态（避免把 A 的令牌发给 B）。
    QObject::connect(m_app, &cv::AppController::serverUrlChanged,
                     m_auth, &cv::AuthController::onServerUrlChanged);

    // ---- 默认下载目录注入（启用 Settings::downloadDir()）+ 设置变更后重新注入 ----
    applyDownloadDirSetting();
    QObject::connect(m_app, &cv::AppController::downloadDirChanged, this,
                     &Application::applyDownloadDirSetting);

    // ---- 用量自动刷新：文件增 / 删 / 改名 / 新建文件夹成功 → 刷服务器用量 ----
    QObject::connect(m_files, &cv::FileController::storageChanged, m_stats,
                     &cv::StatsController::refresh);
    // 上传/下载任务**成功结束**后同样刷新用量。复用既有信号 TransferManager::taskFinished
    // （不新造语义重复的信号）；下载成功也刷一次（无害，且刷新走异步不阻塞）。
    QObject::connect(m_transfer, &cv::TransferManager::taskFinished, this,
                     [this](const QString &, bool ok, const QString &) {
                         if (ok && m_stats)
                             m_stats->refresh();
                     });
}

void Application::applyDownloadDirSetting()
{
    if (m_transfer)
        m_transfer->setDefaultDownloadDir(cv::Settings::instance().downloadDir());
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


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
#include "../controllers/TagController.h"
#include "../controllers/TransferController.h"
#include "../controllers/TrashController.h"
#include "../controllers/VersionController.h"
#include "../core/AppPaths.h"
#include "../core/Settings.h"
#include "../data/FileListModel.h"
#include "../data/SyncPairModel.h"
#include "../data/TagModel.h"
#include "../data/TransferModel.h"
#include "../data/VersionModel.h"
#include "../net/HttpBackend.h"
#include "../net/MockBackend.h"
#include "../sync/LocalIndex.h"
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
    m_tagModel = new cv::TagModel(this);
    m_versionModel = new cv::VersionModel(this);
    m_transfer = new cv::TransferManager(m_backend, this);

    m_app = new cv::AppController(m_backend, this);
    m_auth = new cv::AuthController(m_backend, m_app, this);
    m_files = new cv::FileController(m_backend, m_fileModel, this);
    // ---- 文件夹自动同步（F3）----
    // ⚠️ 顺序有讲究：LocalIndex 必须先 open() 再交给引擎 —— 引擎一被构造出来就可能
    //    立刻开始对账（Settings::autoSync 默认 true），那时索引若还没打开，
    //    上次的两端一致状态读不到 ⇒ 全部文件被当成"新增"，会白跑一轮全量上传。
    m_syncPairs = new cv::SyncPairModel(this);
    m_syncIndex = new cv::LocalIndex(this);
    m_syncIndex->open(cv::AppPaths::indexDbFile());
    m_sync = new cv::SyncController(m_backend, m_syncIndex, m_syncPairs, this);
    m_transfers = new cv::TransferController(m_transfer, this);
    // 把队列进度镜像到 QML 数据模型（否则传输页/侧栏永远空白）
    m_transfers->setTaskModel(m_transferModel);
    m_shares = new cv::ShareController(m_backend, this);
    m_stats = new cv::StatsController(m_backend, this);
    // ---- 回收站 / 标签 / 版本 ----
    // 回收站复用 FileListModel 作为展示模型（行结构与文件列表不同：dir 列放"原位置"、
    // status 列放"文件/文件夹"、time 列放"删除时间"），所以**必须**是独立实例 ——
    // 与文件页共用一个模型会让两个页面互相把对方的数据冲掉。
    m_trashModel = new cv::FileListModel(this);
    m_trash = new cv::TrashController(m_backend, m_trashModel, this);
    m_tags = new cv::TagController(m_backend, m_tagModel, this);
    m_versions = new cv::VersionController(m_backend, m_versionModel, this);

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
    QObject::connect(m_trash, &cv::TrashController::logMessage, this,
                     [](const QString &lv, const QString &tx) { logToSink("Trash", lv, tx); });
    QObject::connect(m_tags, &cv::TagController::logMessage, this,
                     [](const QString &lv, const QString &tx) { logToSink("Tags", lv, tx); });
    QObject::connect(m_versions, &cv::VersionController::logMessage, this,
                     [](const QString &lv, const QString &tx) { logToSink("Versions", lv, tx); });
    // SyncController 没有 logMessage(level,text)，只有给用户看的状态信号；一并落到文件日志，
    // 否则"同步启动/暂停/同步对增删"这些关键动作在 cloudvault.log 里查不到。
    QObject::connect(m_sync, &cv::SyncController::statusMessage, this,
                     [](const QString &tx, bool ok) {
                         logToSink("Sync", ok ? QStringLiteral("info") : QStringLiteral("warn"),
                                   tx);
                     });

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

    // ================= 三项功能的跨模块联动 =================
    // 说明：这三个功能各自"独立可用"，但共享同一份文件实体，所以状态必须互相通知 ——
    // 否则就会出现"回收站里恢复成功了，文件页还看不见"这类各自为政的错觉。

    // ① 回收站内容变动（恢复 / 彻底删除 / 清空）→ 文件列表 + 服务器用量。
    //    恢复会把文件（以及它的原目录链）重新变回可见，彻底删除则会释放空间；
    //    两个后果都超出回收站页自身，必须往外广播。
    QObject::connect(m_trash, &cv::TrashController::trashMutated, this, [this] {
        if (m_files)
            m_files->refresh();
        if (m_stats)
            m_stats->refresh();
    });

    // ② 文件列表里的增删改 → 回收站条目可能已失效（例如"彻底删除"后回收站少一条）。
    //    反向不订阅：FileController 的软删除由回收站页进入时自行刷新（见 TrashPage）。

    // ③ 版本回滚 → 文件内容被替换（size / mtime / hash 全变）→ 文件列表 + 用量必须刷新。
    //    预览面板的强制重取在 QML 侧（FilesPage 监听 versionRestored 后复位预览去抖键），
    //    因为"当前选中项"是界面状态，装配层不该代持。
    QObject::connect(m_versions, &cv::VersionController::versionRestored, this,
                     [this](const QString &) {
                         if (m_files)
                             m_files->refresh();
                         if (m_stats)
                             m_stats->refresh();
                     });

    // ④ 登录态变化 → 三个功能的数据都按 owner 隔离，换账号必须清干净，
    //    否则会把上一个用户的回收站/标签/版本留在界面上。
    //    登录成功后同理重取（登录前后 owner 不同，旧的空结果不能一直挂着）。
    // ⚠️ 必须用 qOverload 消歧：AuthController 同时有 Q_PROPERTY(bool loggedIn READ loggedIn)
    //    的属性读取函数与名为 loggedIn 的信号，直接取 &AuthController::loggedIn 编译不过
    //    （no matching function for call to connect —— 实际是"重载函数无法解析"）。
    QObject::connect(m_auth,
                     qOverload<const QString &, const QString &, const QString &>(
                         &cv::AuthController::loggedIn),
                     this, [this]() {
                         if (m_tags)
                             m_tags->refresh();
                         if (m_trash)
                             m_trash->refresh();
                         if (m_versions)
                             m_versions->close();
                     });
    QObject::connect(m_auth, &cv::AuthController::loggedOut, this,
                     [this](const QString &) {
                         if (m_tags)
                             m_tags->refresh();
                         if (m_trash)
                             m_trash->refresh();
                         if (m_versions)
                             m_versions->close();
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
    ctx->setContextProperty(QStringLiteral("Trash"), m_trash);
    ctx->setContextProperty(QStringLiteral("Tags"), m_tags);
    ctx->setContextProperty(QStringLiteral("Versions"), m_versions);
    ctx->setContextProperty(QStringLiteral("FileModel"), m_fileModel);
    ctx->setContextProperty(QStringLiteral("TransferModel"), m_transferModel);
    // 回收站用的独立行模型（与 FileModel 不同实例，勿合并）
    ctx->setContextProperty(QStringLiteral("SyncModel"), m_syncPairs);
    ctx->setContextProperty(QStringLiteral("TrashModel"), m_trashModel);
    ctx->setContextProperty(QStringLiteral("TagModel"), m_tagModel);
    ctx->setContextProperty(QStringLiteral("VersionModel"), m_versionModel);
}


/****************************************************************************
 * controllers/SyncController.cpp —— 同步控制器实现
 *
 * 分层：QML → SyncController（校验/落盘/编排）→ SyncEngine（调度/执行）→ Backend + LocalIndex
 * 本控制器**不直接操作文件**，也不直接发网络请求；它只把意图变成引擎的输入。
 ****************************************************************************/
#include "SyncController.h"

#include "core/AppPaths.h"
#include "core/Settings.h"
#include "data/SyncPairModel.h"
#include "net/Backend.h"
#include "sync/LocalIndex.h"
#include "sync/SyncEngine.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSysInfo>
#include <QUuid>

namespace cv {
namespace {

constexpr int kMaxLogLines = 400;   // 界面日志环形上限：只说明"最近发生了什么"，不做归档
constexpr qint64 kSyncLogMaxBytes = 2 * 1024 * 1024;   // 落盘日志 2MB 轮转为 sync.log.1

QString newPairId()
{
    return QStringLiteral("sp-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

// FR-3-9：同步日志落盘。
// 为什么内存环形不够：同步是**长期后台**行为，"昨天那次为什么没同步"只能靠文件日志回溯；
// 而 Logging 的 cloudvault.log 是通用日志，同步动作混在里面很难读。单独一份 sync.log。
// 轮转策略故意简单（超 2MB 就改名 .1 覆盖旧的）—— 同步日志不需要历史归档，只需要"最近一段"。
void appendToSyncLogFile(const QString &line)
{
    const QString dir = AppPaths::logDir();
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QStringLiteral("sync.log"));

    const QFileInfo fi(path);
    if (fi.exists() && fi.size() > kSyncLogMaxBytes) {
        QFile::remove(path + QStringLiteral(".1"));
        QFile::rename(path, path + QStringLiteral(".1"));
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;   // 落盘失败不影响同步本身
    f.write(line.toUtf8());
    f.write("\n");
}

} // namespace

SyncController::SyncController(Backend *backend, LocalIndex *index, SyncPairModel *model,
                               QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_index(index)
    , m_model(model)
{
    const QString host = QSysInfo::machineHostName();
    m_deviceName = host.isEmpty() ? QStringLiteral("本机") : host;

    m_engine = new SyncEngine(m_backend, m_index, this);
    m_engine->setDeviceName(m_deviceName);

    connect(m_engine, &SyncEngine::logAppended, this, &SyncController::appendLog);
    connect(m_engine, &SyncEngine::pairStatus, this,
            [this](const QString &pairId, int status, const QString &message) {
                if (SyncPair *p = findPair(pairId)) {
                    p->status = SyncStatus(status);
                    if (!message.isEmpty())
                        p->lastError = message;
                    if (m_model)
                        m_model->updatePair(*p);
                    emit pairsChanged();
                }
            });
    connect(m_engine, &SyncEngine::pairSettled, this, [this](const SyncPair &after) {
        // 引擎只给出"这次对账的结论"，持久化与展示由控制器统一负责
        if (SyncPair *p = findPair(after.id)) {
            *p = after;
            if (m_model)
                m_model->updatePair(*p);
            persistPairs();
            emit pairsChanged();
        }
    });

    loadPairs();

    // FR-3-1：默认 5 秒防抖 / 10 分钟兜底全量对账
    m_engine->setScanIntervalMin(Settings::instance().fullScanIntervalMin());
    m_engine->setDebounceMs(5000);

    if (Settings::instance().autoSync())
        start();
}

SyncController::~SyncController() = default;

// ============================ 状态访问 ============================

bool SyncController::running() const
{
    return m_engine && m_engine->isRunning();
}

bool SyncController::autoSync() const
{
    return Settings::instance().autoSync();
}

void SyncController::setAutoSync(bool on)
{
    if (Settings::instance().autoSync() == on)
        return;
    Settings::instance().setAutoSync(on);
    emit autoSyncChanged();
    if (on)
        start();
    else
        stop();
}

int SyncController::scanIntervalMin() const
{
    return Settings::instance().fullScanIntervalMin();
}

void SyncController::setScanIntervalMin(int minutes)
{
    const int v = qMax(1, minutes);
    if (Settings::instance().fullScanIntervalMin() == v)
        return;
    Settings::instance().setFullScanIntervalMin(v);
    if (m_engine)
        m_engine->setScanIntervalMin(v);
    emit scanIntervalChanged();
}

bool SyncController::hasPairs() const
{
    return !m_pairs.isEmpty();
}

int SyncController::pairCount() const
{
    return m_pairs.size();
}

QVariantList SyncController::logEntries() const
{
    return m_log;
}

QString SyncController::deviceName() const
{
    return m_deviceName;
}

QString SyncController::emptyText() const
{
    return QStringLiteral(
        "还没有同步对。新建一个同步对，把本机目录与云端目录绑定起来：\n"
        "· 双向同步：两端改动互相同步；冲突时两份都保留（较旧的那份另存为「冲突副本」）\n"
        "· 仅上传：本机目录单向备份到云端，云端改动不会拉回本地\n"
        "· 仅下载：把云端目录单向发布到本机\n\n"
        "删除是安全的：云端删除进云端回收站；本地删除会把文件**搬进本机的同步保险箱**"
        "（不是直接抹掉），位置见页面下方提示。");
}

QString SyncController::statusText(int status) const
{
    return SyncPairModel::statusText(status);
}

QString SyncController::modeText(int mode) const
{
    return SyncPairModel::modeText(mode);
}

QString SyncController::localTrashLocation() const
{
    return SyncEngine::localTrashRoot();
}

// ============================ 同步对 CRUD ============================

SyncPair *SyncController::findPair(const QString &pairId)
{
    for (SyncPair &p : m_pairs) {
        if (p.id == pairId)
            return &p;
    }
    return nullptr;
}

void SyncController::loadPairs()
{
    m_pairs = Settings::instance().syncPairs();
    for (SyncPair &p : m_pairs) {
        if (p.id.isEmpty())
            p.id = newPairId();
        // 上次退出时可能停在"同步中"，启动时状态归位
        p.status = p.enabled ? SyncStatus::Idle : SyncStatus::Disabled;
    }
    if (m_model)
        m_model->setPairs(m_pairs);
    emit pairsChanged();
}

void SyncController::persistPairs()
{
    Settings::instance().setSyncPairs(m_pairs);
    Settings::instance().sync();
}

void SyncController::pushToEngine()
{
    if (m_engine)
        m_engine->setPairs(m_pairs);
}

QString SyncController::addPair(const QString &localPath, const QString &remotePath, int mode)
{
    if (localPath.trimmed().isEmpty())
        return QStringLiteral("请填写本机目录");
    const QString local = QDir(localPath.trimmed()).absolutePath();
    if (!QFileInfo(local).isDir())
        return QStringLiteral("本机目录不存在或不是目录：%1").arg(local);

    QString remote = remotePath.trimmed();
    if (remote.isEmpty())
        return QStringLiteral("请填写云端目录");
    if (!remote.startsWith(QLatin1Char('/')))
        remote.prepend(QLatin1Char('/'));
    while (remote.size() > 1 && remote.endsWith(QLatin1Char('/')))
        remote.chop(1);

    for (const SyncPair &p : m_pairs) {
        // 完全重复：两个同步对写同一处必然互相打架
        if (QDir(p.localPath).absolutePath() == local && p.remotePath == remote)
            return QStringLiteral("这个同步对已经存在了");
        // 云端目录嵌套：同一批文件被两个策略同时管，结果不可预期
        if (remote.startsWith(p.remotePath + QLatin1Char('/')))
            return QStringLiteral("云端目录与已有同步对「%1」重叠").arg(p.remotePath);
        if (p.remotePath.startsWith(remote + QLatin1Char('/')))
            return QStringLiteral("云端目录包含了已有同步对「%1」").arg(p.remotePath);
    }

    SyncPair p;
    p.id         = newPairId();
    p.localPath  = local;
    p.remotePath = remote;
    p.mode       = SyncMode(mode);
    p.enabled    = true;
    p.status     = SyncStatus::Idle;

    m_pairs.append(p);
    persistPairs();
    pushToEngine();
    if (m_model)
        m_model->updatePair(p);
    emit pairsChanged();

    appendLog(QStringLiteral("新建同步对：%1 ⇄ %2（%3）").arg(local, remote, modeText(mode)));
    if (m_engine && m_engine->isRunning())
        m_engine->reconcileNow(p.id);
    return QString();
}

void SyncController::removePair(const QString &pairId)
{
    for (int i = 0; i < m_pairs.size(); ++i) {
        if (m_pairs[i].id != pairId)
            continue;
        const SyncPair gone = m_pairs.takeAt(i);
        // 索引必须一起清：留着的话，将来复用同 id 会误判"已同步"而漏掉全部改动
        if (m_index)
            m_index->removePair(pairId);
        if (m_model)
            m_model->removePair(pairId);
        persistPairs();
        pushToEngine();
        emit pairsChanged();
        appendLog(QStringLiteral("已移除同步对：%1 ⇄ %2（本机与云端的文件都没有被删除）")
                      .arg(gone.localPath, gone.remotePath));
        return;
    }
}

void SyncController::setPairEnabled(const QString &pairId, bool on)
{
    SyncPair *p = findPair(pairId);
    if (!p)
        return;
    p->enabled = on;
    p->status  = on ? SyncStatus::Idle : SyncStatus::Disabled;
    persistPairs();
    pushToEngine();
    if (m_model)
        m_model->updatePair(*p);
    emit pairsChanged();

    // 监听集合跟着 enabled 变，必须重开一次引擎
    if (m_engine && m_engine->isRunning()) {
        m_engine->stop();
        m_engine->start();
    }
}

void SyncController::setPairExcludes(const QString &pairId, const QString &text)
{
    SyncPair *p = findPair(pairId);
    if (!p)
        return;

    QStringList dirs;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        QString e = line.trimmed();
        if (e.isEmpty())
            continue;
        // 归一化：去掉首尾斜杠（与 SyncPlan::isExcluded 的归一化保持一致，
        // 否则界面上显示的和实际生效的可能不是同一个东西）
        while (e.startsWith(QLatin1Char('/')))
            e.remove(0, 1);
        while (e.endsWith(QLatin1Char('/')))
            e.chop(1);
        if (!e.isEmpty() && !dirs.contains(e))
            dirs << e;
    }

    if (p->excludeDirs == dirs)
        return;
    p->excludeDirs = dirs;
    persistPairs();
    pushToEngine();
    if (m_model)
        m_model->updatePair(*p);
    emit pairsChanged();
    appendLog(QStringLiteral("选择性同步已更新：%1（排除 %2 个云端目录）")
                  .arg(p->remotePath)
                  .arg(dirs.size()));
    if (m_engine && m_engine->isRunning())
        m_engine->reconcileNow(pairId);
}

void SyncController::setPairMode(const QString &pairId, int mode)
{
    SyncPair *p = findPair(pairId);
    if (!p || int(p->mode) == mode)
        return;
    p->mode = SyncMode(mode);
    persistPairs();
    pushToEngine();
    if (m_model)
        m_model->updatePair(*p);
    emit pairsChanged();
    appendLog(QStringLiteral("同步方向改为 %1：%2").arg(modeText(mode), p->localPath));
    // 换方向后"上次两端一致"的基准依然有效（索引记的是真实观察到的状态），
    // 直接重对账即可，不需要清空索引。
    if (m_engine && m_engine->isRunning())
        m_engine->reconcileNow(pairId);
}

// ============================ 运行控制 ============================

void SyncController::start()
{
    if (!m_engine)
        return;
    pushToEngine();
    m_engine->start();
    emit runningChanged();
    appendLog(QStringLiteral("同步已启动：%1 个同步对，每 %2 分钟兜底对账一次")
                  .arg(m_pairs.size())
                  .arg(scanIntervalMin()));
    emit statusMessage(QStringLiteral("同步已启动"), true);
}

void SyncController::stop()
{
    if (!m_engine)
        return;
    m_engine->stop();
    for (SyncPair &p : m_pairs) {
        if (p.enabled)
            p.status = SyncStatus::Paused;
        if (m_model)
            m_model->updatePair(p);
    }
    emit runningChanged();
    emit pairsChanged();
    appendLog(QStringLiteral("同步已暂停（目录监听与定时对账均已停止）"));
    emit statusMessage(QStringLiteral("同步已暂停"), true);
}

void SyncController::syncNow(const QString &pairId)
{
    if (!m_engine)
        return;
    if (!m_engine->isRunning()) {
        // 未启动就点"立即同步"：先把引擎起起来，否则请求会被静默丢掉
        start();
    }
    if (pairId.isEmpty()) {
        m_engine->reconcileNow();
        appendLog(QStringLiteral("手动触发：全部同步对"));
    } else {
        m_engine->reconcileNow(pairId);
        if (SyncPair *p = findPair(pairId))
            appendLog(QStringLiteral("手动触发：%1").arg(p->localPath));
    }
}

void SyncController::clearLog()
{
    m_log.clear();
    emit logChanged();
}

void SyncController::setTransferManager(TransferManager *tm)
{
    if (m_engine)
        m_engine->setTransferManager(tm);
}

void SyncController::appendLog(const QString &line)
{
    const QString stamped =
        QStringLiteral("[%1] %2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), line);
    m_log.prepend(stamped);
    while (m_log.size() > kMaxLogLines)
        m_log.removeLast();
    // FR-3-9：同时落盘（带日期，便于回溯"昨天那次为什么没同步"）
    appendToSyncLogFile(QStringLiteral("[%1] %2")
                            .arg(QDateTime::currentDateTime().toString(
                                     QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                                 line));
    emit logChanged();
}

} // namespace cv

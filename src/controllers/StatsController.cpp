/****************************************************************************
 * controllers/StatsController.cpp —— 存储统计控制器实现
 ****************************************************************************/
#include "StatsController.h"

#include "core/Settings.h"  // downloadDir()（判定 isDefault 所在卷）
#include "core/Util.h"      // Util::humanSize
#include "net/Backend.h"    // Backend / Result / UsageStats

#include <QDir>
#include <QStorageInfo>
#include <QVariantMap>

namespace cv {
namespace {

// 把后端 Result 的失败信息（含 [unsupported] 前缀、HTTP 状态码与服务端 error 文本）
// 转成中文可读、可操作的提示。不得吞错。
QString friendlyError(const QString &raw)
{
    if (raw.isEmpty())
        return QStringLiteral("未知错误");

    if (raw.startsWith(QStringLiteral("[unsupported]")))
        return QStringLiteral("服务端暂不支持此功能");

    if (raw.contains(QStringLiteral("HTTP 401")))
        return QStringLiteral("令牌无效，请检查访问令牌");
    if (raw.contains(QStringLiteral("HTTP 403")))
        return QStringLiteral("服务器拒绝访问（403）");
    if (raw.contains(QStringLiteral("HTTP 404")))
        return QStringLiteral("目标不存在（404）");
    if (raw.contains(QStringLiteral("HTTP 507")))
        return QStringLiteral("服务器空间不足（507）");
    if (raw.contains(QStringLiteral("HTTP 413")))
        return QStringLiteral("文件超出服务器单次上限（413）");

    return raw; // 其余（含超时 / 网络错误 / 服务端 error 原文）原样透出
}

} // namespace

StatsController::StatsController(Backend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
{
    m_supported = (m_backend != nullptr);
    m_freeText = m_supported ? QStringLiteral("空间信息未获取")
                             : QStringLiteral("未配置数据源");

    // 本机磁盘：纯本机查询、无网络、不阻塞。构造即填一次（QML 首帧即有数据）；
    // 之后由 QML 按需调用 refreshLocalDisks()。**不**放进 refresh() —— 避免每次刷新服务器
    // 用量时重复枚举本机卷（两者变化频率与数据源无关）。
    refreshLocalDisks();
}

void StatsController::refresh()
{
    if (!m_backend) {
        m_freeBytes = -1;
        m_totalBytes = 0;
        m_freeText = QStringLiteral("空间信息不可用：未配置数据源");
        emit statsChanged();
        return;
    }

    // 非阻塞：走异步变体（HttpBackend 真异步；本地引擎默认退化同步，行为不变）。
    // 用序号丢弃乱序回包（连点刷新 / 与启动查询竞争时只认最新一次）。
    const int seq = ++m_usageSeq;
    m_backend->usageAsync([this, seq](Result<UsageStats> r) {
        if (seq != m_usageSeq) {
            return; // 乱序回包：作废
        }

        if (!r.ok) {
            m_freeBytes = -1;
            m_totalBytes = 0;
            m_freeText = friendlyError(r.error);
            emit logMessage(QStringLiteral("ERROR"),
                            QStringLiteral("存储空间查询失败：%1").arg(r.error));
            emit statsChanged();
            return;
        }

        const UsageStats u = r.value;
        m_totalBytes = u.total;
        if (u.total > 0) {
            // used 由后端给出；free = total - used（下限 0，避免服务端越界数据出现负数）
            m_freeBytes = qMax<qint64>(0, u.total - u.used);
            m_freeText = QStringLiteral("剩余 %1 / 共 %2")
                             .arg(Util::humanSize(m_freeBytes), Util::humanSize(m_totalBytes));
        } else {
            // 服务端未返回总容量（total==0）：不臆造数字
            m_freeBytes = -1;
            m_freeText = QStringLiteral("服务器未返回容量信息");
        }
        emit statsChanged();
    });
}

void StatsController::refreshLocalDisks()
{
    QVariantList list;

    // 下载目录所在卷（用于 isDefault）：QStorageInfo 从路径构造最稳妥；无效则退回系统根卷。
    const QString dl = Settings::instance().downloadDir();
    QStorageInfo  dlVol;
    if (!dl.isEmpty())
        dlVol = QStorageInfo(dl);
    if (!dlVol.isValid() || !dlVol.isReady())
        dlVol = QStorageInfo::root();
    const QString dlRoot = dlVol.isValid() ? dlVol.rootPath() : QString();

    const QList<QStorageInfo> vols = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &st : vols) {
        if (!st.isValid() || !st.isReady() || st.bytesTotal() <= 0)
            continue; // 跳过无效 / 未就绪 / 无容量的伪设备

        const qint64 total = st.bytesTotal();
        const qint64 free  = st.bytesAvailable(); // 当前用户可用字节
        const qint64 used  = qMax<qint64>(0, total - free);

        // name：优先盘符形式（"C:"）；非盘符卷退回 displayName / 根路径
        const QString root = QDir::toNativeSeparators(st.rootPath());
        QString       name;
        if (root.size() >= 2 && root.at(1) == QLatin1Char(':'))
            name = root.left(2).toUpper();
        else
            name = st.displayName().isEmpty() ? root : st.displayName();

        // label：有卷标 → "卷标 (C:)"，否则就是 "C:"
        const QString volLabel = st.name();
        const QString label =
            volLabel.isEmpty() ? name : QStringLiteral("%1 (%2)").arg(volLabel, name);

        QVariantMap m;
        m.insert(QStringLiteral("name"), name);
        m.insert(QStringLiteral("label"), label);
        m.insert(QStringLiteral("totalText"), Util::humanSize(total));
        m.insert(QStringLiteral("freeText"), Util::humanSize(free));
        m.insert(QStringLiteral("usedText"), Util::humanSize(used));
        m.insert(QStringLiteral("usedRatio"), total > 0 ? double(used) / double(total) : 0.0);
        m.insert(QStringLiteral("isDefault"),
                 !dlRoot.isEmpty()
                     && st.rootPath().compare(dlRoot, Qt::CaseInsensitive) == 0);
        list.append(m);
    }

    m_localDisks = list;
    emit localDisksChanged();
}

} // namespace cv

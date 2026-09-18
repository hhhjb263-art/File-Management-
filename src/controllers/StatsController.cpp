/****************************************************************************
 * controllers/StatsController.cpp —— 存储统计控制器实现
 ****************************************************************************/
#include "StatsController.h"

#include "core/Util.h"      // Util::humanSize
#include "net/Backend.h"    // Backend / Result / UsageStats

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

} // namespace cv

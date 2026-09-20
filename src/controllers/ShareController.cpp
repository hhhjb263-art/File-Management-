/****************************************************************************
 * controllers/ShareController.cpp —— 分享链接控制器实现
 *
 * 全异步：走 Backend 的 sharesAsync / createShareAsync / revokeShareAsync
 * （HttpBackend 真异步，不阻塞主线程；其它后端默认退化同步）。
 ****************************************************************************/
#include "ShareController.h"

#include "core/Settings.h" // Settings::instance()：提取码本地缓存
#include "core/Util.h"      // Util::humanSize
#include "net/Backend.h"    // Backend / Result / ShareLink

#include <QClipboard>
#include <QGuiApplication>
#include <QVariantMap>

namespace cv {
namespace {

// 后端失败信息 -> 中文可读提示（含可识别前缀 / HTTP 状态码），绝不吞错。
QString friendlyShareError(const QString &raw)
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
        return QStringLiteral("分享不存在或已被删除（404）");

    return raw;
}

} // namespace

ShareController::ShareController(Backend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
{
    refresh(); // 构造即拉一次列表
}

bool ShareController::supported() const
{
    // 服务端已实现分享能力 → 界面入口可用（保留 supportedChanged 信号以兼容既有绑定）
    return true;
}

QString ShareController::friendlyError(const QString &raw)
{
    return friendlyShareError(raw);
}

void ShareController::setBusy(bool on)
{
    if (on == m_busy)
        return;
    m_busy = on;
    emit busyChanged();
}

void ShareController::refresh()
{
    if (!m_backend) {
        m_shares.clear();
        emit sharesChanged();
        return;
    }

    const int seq = ++m_seq;
    setBusy(true);
    m_backend->sharesAsync([this, seq](Result<QVector<ShareLink>> r) {
        if (seq != m_seq)
            return; // 乱序回包：作废
        setBusy(false);

        if (!r.ok) {
            const QString msg = friendlyError(r.error);
            emit errorOccurred(msg);
            emit logMessage(QStringLiteral("ERROR"),
                            QStringLiteral("获取分享列表失败：%1").arg(r.error));
            return;
        }
        m_shares = r.value;
        // 本地判定失效状态（revoked / 已过期 / 次数用尽 ⇒ 归入「已失效」栏）。
        // 不依赖服务端 state 字段——旧服务端没升级也能正确分栏（用户实测诉求）。
        for (ShareLink &s : m_shares) {
            if (s.revoked)
                s.state = QStringLiteral("revoked");
            else if (s.expired())
                s.state = QStringLiteral("expired");
            else if (s.maxDownloads > 0 && s.downloads >= s.maxDownloads)
                s.state = QStringLiteral("exhausted");
            else if (s.state.isEmpty())
                s.state = QStringLiteral("active");
        }
        emit sharesChanged();
    });
}

void ShareController::create(const QString &fileId, const QString &code, int expireDays,
                             int maxDownloads)
{
    if (!m_backend) {
        emit errorOccurred(QStringLiteral("未配置数据源"));
        return;
    }
    if (fileId.isEmpty()) {
        emit errorOccurred(QStringLiteral("请先选择要分享的文件"));
        return;
    }

    const int seq = ++m_seq;
    setBusy(true);
    m_backend->createShareAsync(fileId, code, expireDays, maxDownloads,
                                [this, seq](Result<ShareLink> r) {
                                    if (seq != m_seq)
                                        return; // 乱序：作废
                                    setBusy(false);

                                    if (!r.ok) {
                                        const QString msg = friendlyError(r.error);
                                        emit errorOccurred(msg);
                                        emit logMessage(
                                            QStringLiteral("ERROR"),
                                            QStringLiteral("创建分享失败：%1").arg(r.error));
                                        return;
                                    }

                                    // 成功：链接 + 明文提取码**仅此一次**回显给界面
                                    // 同时把「分享 id → 提取码」缓存在本机（settings.ini），
                                    // 分享列表按 id 展示明文（服务端只存哈希，列表永远拿不到明文）
                                    if (!r.value.id.isEmpty() && !r.value.code.isEmpty()) {
                                        Settings::instance().setShareCode(r.value.id, r.value.code);
                                        Settings::instance().sync();
                                    }
                                    emit created(r.value.url, r.value.code);
                                    emit statusMessage(QStringLiteral("✓ 分享链接已创建"), true);
                                    emit logMessage(QStringLiteral("INFO"),
                                                    QStringLiteral("已创建分享：%1")
                                                        .arg(r.value.fileName));
                                    refresh(); // 列表立即出现新项
                                });
}

void ShareController::revoke(const QString &shareId)
{
    if (!m_backend) {
        emit errorOccurred(QStringLiteral("未配置数据源"));
        return;
    }
    if (shareId.isEmpty()) {
        emit errorOccurred(QStringLiteral("请先选择要撤销的分享"));
        return;
    }

    const int seq = ++m_seq;
    setBusy(true);
    m_backend->revokeShareAsync(shareId, [this, seq](Ok r) {
        if (seq != m_seq)
            return; // 乱序：作废
        setBusy(false);

        if (!r.ok) {
            const QString msg = friendlyError(r.error);
            emit errorOccurred(msg);
            emit logMessage(QStringLiteral("ERROR"),
                            QStringLiteral("撤销分享失败：%1").arg(r.error));
            return;
        }
        emit statusMessage(QStringLiteral("✓ 已撤销分享"), true);
        refresh();
    });
}

void ShareController::copyLink(const QString &url)
{
    if (url.isEmpty()) {
        emit statusMessage(QStringLiteral("✗ 链接为空，无法复制"), false);
        return;
    }
    QClipboard *cb = QGuiApplication::clipboard();
    if (!cb) {
        emit statusMessage(QStringLiteral("✗ 无法访问剪贴板"), false);
        return;
    }
    cb->setText(url);
    emit statusMessage(QStringLiteral("✓ 链接已复制到剪贴板"), true);
}

QVariantMap ShareController::at(int i) const
{
    if (i < 0 || i >= m_shares.size())
        return QVariantMap(); // 越界：空 map（不崩、不报错）

    const ShareLink &s = m_shares.at(i);

    QVariantMap m;
    // 前 11 个键：与 ShareLinkModel 角色名一致
    m.insert(QStringLiteral("id"), s.id);
    m.insert(QStringLiteral("fileId"), s.fileId);
    m.insert(QStringLiteral("fileName"), s.fileName);
    m.insert(QStringLiteral("isDir"), s.isDir);
    m.insert(QStringLiteral("url"), s.url);
    m.insert(QStringLiteral("code"), s.code);
    m.insert(QStringLiteral("created"), s.created);
    m.insert(QStringLiteral("expire"), s.expire);
    m.insert(QStringLiteral("downloads"), s.downloads);
    m.insert(QStringLiteral("maxDownloads"), s.maxDownloads);
    m.insert(QStringLiteral("revoked"), s.revoked);
      m.insert(QStringLiteral("state"), s.state);
      // 提取码明文只在创建时的本机有缓存（服务端只存哈希）；无缓存显示空 ⇒ QML 显示「—」
      m.insert(QStringLiteral("codeText"),
               Settings::instance().shareCodes().value(s.id).toString());

    // 后 6 个键：展示用派生值
    m.insert(QStringLiteral("needCode"), s.needCode);
    m.insert(QStringLiteral("expired"), s.expired());
    m.insert(QStringLiteral("usable"), s.usable());
    m.insert(QStringLiteral("expireText"),
             s.expire.isValid()
                 ? QStringLiteral("%1 到期")
                       .arg(s.expire.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                 : QStringLiteral("永久有效"));
    const int remain = s.maxDownloads > 0 ? qMax(0, s.maxDownloads - s.downloads) : 0;
    m.insert(QStringLiteral("remainText"),
             s.maxDownloads > 0 ? QStringLiteral("剩余 %1 次").arg(remain)
                                : QStringLiteral("不限次数"));
    m.insert(QStringLiteral("sizeText"), Util::humanSize(s.size));
    return m;
}

// ---------------------------------------------------------------------------
//  两栏分桶计数 / 清除无效分享
// ---------------------------------------------------------------------------
int ShareController::activeCount() const
{
    int n = 0;
    for (const ShareLink &s : m_shares)
        if (s.state == QStringLiteral("active"))
            ++n;
    return n;
}

int ShareController::invalidCount() const
{
    return m_shares.size() - activeCount();
}

void ShareController::cleanupInvalid()
{
    if (!m_backend || m_busy)
        return;

    setBusy(true);
    m_backend->cleanupInvalidSharesAsync([this](Result<std::int64_t> r) {
        setBusy(false);
        if (!r.ok) {
            emit errorOccurred(friendlyShareError(r.error));
            emit logMessage(QStringLiteral("ERROR"),
                            QStringLiteral("清除无效分享失败：%1").arg(r.error));
            return;
        }
        // 同步清掉「已失效分享」的本机提取码缓存（进行中的保留）
        const QVariantMap codes = Settings::instance().shareCodes();
        QStringList dead;
        for (const ShareLink &s : m_shares) {
            if (s.state != QStringLiteral("active") && codes.contains(s.id))
                dead << s.id;
        }
        if (!dead.isEmpty())
            Settings::instance().dropShareCodes(dead);
        emit logMessage(QStringLiteral("INFO"),
                        QStringLiteral("已清除 %1 条无效分享").arg(r.value));
        emit statusMessage(QStringLiteral("已清除 %1 条无效分享").arg(r.value), true);
        refresh();  // 重新拉取，两栏计数随之更新
    });
}

} // namespace cv

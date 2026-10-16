#include "AppTheme.h"

#include "Settings.h"

#include <QColor>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>

namespace cv {
namespace {

// ===========================================================================
//  调色板色值 —— 与 qml/theme/Theme.qml 的令牌一一对应（唯一需人工同步处）
//  标注格式：[L] = 浅色令牌   [D] = 深色令牌
// ===========================================================================
struct Ramp
{
    QColor window;        // Theme.bg
    QColor base;          // Theme.card（输入框 / 列表底色）
    QColor altBase;       // Theme.surfaceSunken
    QColor text;          // Theme.textPrimary
    QColor textDim;       // Theme.textTertiary（placeholder）
    QColor button;        // Theme.card
    QColor buttonText;    // Theme.textPrimary
    QColor border;        // Theme.border
    QColor accent;        // Theme.primary
    QColor accentText;    // Theme.textOnPrimary
    QColor disabled;      // Theme.disabledText
};

const Ramp kLight = {
    QColor(0xF7, 0xF8, 0xFA), // [L] bg
    QColor(0xFF, 0xFF, 0xFF), // [L] card
    QColor(0xF1, 0xF3, 0xF6), // [L] surfaceSunken
    QColor(0x14, 0x18, 0x21), // [L] textPrimary
    QColor(0x8B, 0x94, 0xA3), // [L] textTertiary
    QColor(0xFF, 0xFF, 0xFF), // [L] card
    QColor(0x14, 0x18, 0x21), // [L] textPrimary
    QColor(0xE6, 0xE9, 0xEF), // [L] border
    QColor(0x3B, 0x5B, 0xDB), // [L] primary
    QColor(0xFF, 0xFF, 0xFF), // [L] textOnPrimary
    QColor(0xA3, 0xAB, 0xBA)  // [L] disabledText
};

const Ramp kDark = {
    QColor(0x0E, 0x11, 0x16), // [D] bg
    QColor(0x16, 0x1A, 0x21), // [D] card
    QColor(0x0A, 0x0D, 0x11), // [D] surfaceSunken
    QColor(0xE8, 0xEB, 0xF0), // [D] textPrimary
    QColor(0x6E, 0x76, 0x86), // [D] textTertiary
    QColor(0x16, 0x1A, 0x21), // [D] card
    QColor(0xE8, 0xEB, 0xF0), // [D] textPrimary
    QColor(0x26, 0x2C, 0x36), // [D] border
    QColor(0x8A, 0xA0, 0xF2), // [D] primary
    QColor(0x0E, 0x11, 0x16), // [D] textOnPrimary
    QColor(0x52, 0x5A, 0x68)  // [D] disabledText
};

QPalette buildPalette(const Ramp &r)
{
    QPalette pal;
    pal.setColor(QPalette::Window,          r.window);
    pal.setColor(QPalette::WindowText,      r.text);
    pal.setColor(QPalette::Base,            r.base);
    pal.setColor(QPalette::AlternateBase,   r.altBase);
    pal.setColor(QPalette::Text,            r.text);
    pal.setColor(QPalette::PlaceholderText, r.textDim);
    pal.setColor(QPalette::Button,          r.button);
    pal.setColor(QPalette::ButtonText,      r.buttonText);
    pal.setColor(QPalette::BrightText,      r.accentText);
    pal.setColor(QPalette::ToolTipBase,     r.base);
    pal.setColor(QPalette::ToolTipText,     r.text);
    pal.setColor(QPalette::Highlight,       r.accent);
    pal.setColor(QPalette::HighlightedText, r.accentText);
    pal.setColor(QPalette::Link,            r.accent);
    pal.setColor(QPalette::LinkVisited,     r.accent);

    for (auto group : { QPalette::Active, QPalette::Inactive, QPalette::Disabled }) {
        pal.setColor(group, QPalette::Mid,      r.border);
        pal.setColor(group, QPalette::Midlight, r.border);
        pal.setColor(group, QPalette::Light,    r.base);
        pal.setColor(group, QPalette::Dark,     r.border);
        pal.setColor(group, QPalette::Shadow,   r.border);
    }
    // 禁用态文字比常态更淡（仅 Disabled 组）
    pal.setColor(QPalette::Disabled, QPalette::WindowText, r.disabled);
    pal.setColor(QPalette::Disabled, QPalette::Text,       r.disabled);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, r.disabled);
    return pal;
}

} // namespace

// ===========================================================================
//  静态工具
// ===========================================================================

QString AppTheme::normalize(const QString &mode)
{
    const QString m = mode.trimmed().toLower();
    if (m == QLatin1String("light") || m == QLatin1String("dark")
        || m == QLatin1String("system"))
        return m;
    return QStringLiteral("system");
}

QStringList AppTheme::availableModes()
{
    return { QStringLiteral("light"), QStringLiteral("dark"), QStringLiteral("system") };
}

bool AppTheme::resolveDark(const QString &mode)
{
    const QString m = normalize(mode);
    if (m == QLatin1String("dark"))
        return true;
    if (m == QLatin1String("light"))
        return false;
    // system：读平台配色方案；平台未上报（Unknown）时按浅色处理
    if (auto *hints = QGuiApplication::styleHints())
        return hints->colorScheme() == Qt::ColorScheme::Dark;
    return false;
}

void AppTheme::applyToApplication(const QString &mode)
{
    const QString m = normalize(mode);

    auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    if (!app)
        return;

    // ---- ① ColorScheme：显式模式钉死；system 交还平台（Unknown = 跟随）----
    if (auto *hints = app->styleHints()) {
        const Qt::ColorScheme want = (m == QLatin1String("light")) ? Qt::ColorScheme::Light
                                   : (m == QLatin1String("dark"))  ? Qt::ColorScheme::Dark
                                                                   : Qt::ColorScheme::Unknown;
        // 仅在确有变化时写：避免 setColorScheme → colorSchemeChanged → 再进来 的递归
        if (hints->colorScheme() != want)
            hints->setColorScheme(want);
    }

    // ---- ② QPalette：不依赖平台主题，保证无头与真机一致 ----
    app->setPalette(buildPalette(resolveDark(m) ? kDark : kLight));
}

// ===========================================================================
//  实例
// ===========================================================================

AppTheme::AppTheme(QObject *parent) : QObject(parent)
{
    // 初始模式：CV_THEME 环境变量优先（自检 / 无头截图用），否则读持久化配置。
    // 与项目既有自检钩子（CV_START_PAGE / CV_SCREENSHOT）保持同一套约定。
    const QByteArray envMode = qgetenv("CV_THEME");
    m_mode = envMode.isEmpty() ? Settings::instance().themeMode()
                               : normalize(QString::fromLatin1(envMode));

    applyToApplication(m_mode);

    // 跟随系统：平台配色方案变化时重新应用（仅 system 模式会产生实际变化）
    if (auto *hints = QGuiApplication::styleHints()) {
        connect(hints, &QStyleHints::colorSchemeChanged, this,
                [this](Qt::ColorScheme) { onPlatformColorSchemeChanged(); });
    }
}

AppTheme::~AppTheme() = default;

void AppTheme::setThemeMode(const QString &mode)
{
    const QString m = normalize(mode);
    if (m == m_mode)
        return;

    m_mode = m;
    // 持久化（CV_THEME 只是启动期覆盖，不写回配置 —— 否则会污染用户设置）
    Settings::instance().setThemeMode(m);
    Settings::instance().sync();

    applyToApplication(m_mode);
    emit changed();
}

void AppTheme::onPlatformColorSchemeChanged()
{
    if (m_mode != QLatin1String("system"))
        return; // 显式模式下忽略平台变化

    applyToApplication(m_mode);
    emit changed();
}

} // namespace cv

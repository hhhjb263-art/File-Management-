// 云匣 CloudVault 正式客户端入口（Qt6 / QML，无 QtWidgets 依赖）
#include <QGuiApplication>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTimer>

#include "Application.h"

#include "../core/AppPaths.h"
#include "../core/Logging.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("cloudvault"));
    app.setOrganizationDomain(QStringLiteral("cloudvault.local"));
    app.setApplicationName(QStringLiteral("CloudVault"));
    app.setApplicationDisplayName(QStringLiteral("云匣 CloudVault"));
    app.setApplicationVersion(QStringLiteral(CV_APP_VERSION));

    // 日志落盘：装到应用日志目录，便于真机排障（此前从未调用，日志文件一直没生成）
    cv::Logging::install(cv::AppPaths::logDir());

    QQuickStyle::setStyle(QStringLiteral("FluentWinUI3"));

    // 配色方案必须与项目自身的 Theme 对齐（Theme.qml 固定浅色，qtquickcontrols2.conf 亦为 Light）。
    // 不设这一行时 FluentWinUI3 会跟随「系统配色方案」：系统为深色时控件调色板整体变深
    // （实测 colorScheme=Unknown/Dark、palette.text=#ffffff、palette.base=#2d2d2d），
    // 而本应用自己画的卡片是白色 —— 结果设置页的 TextField/ComboBox 变成「白字白底」，
    // 输入框视觉上消失（真机与 offscreen 均可复现）。裸用系统控件的页面都会有此问题。
    // ⚠️ 后续若实现深色模式，这里应改为依据 Theme.darkMode 切换 Light/Dark。
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);

    // 再显式钉一遍应用调色板：不依赖平台主题/配色方案提供正确的文字色。
    // （实测 offscreen 平台会忽略 setColorScheme，其默认调色板 text=#ffffff，导致无头截图失真；
    //   显式设置后无头渲染与真机一致，控件着色也不再看平台脸色。）
    // 色值来自 qml/theme/Theme.qml 的浅色令牌，两处需保持一致。
    {
        QPalette pal = app.palette();
        const QColor cBg      (0xF8, 0xFA, 0xFC); // Theme.bg
        const QColor cCard    (0xFF, 0xFF, 0xFF); // Theme.card
        const QColor cText    (0x0F, 0x17, 0x2A); // Theme.textPrimary
        const QColor cTextDim (0x64, 0x74, 0x8B); // Theme.textSecondary
        const QColor cBorder  (0xE2, 0xE8, 0xF0); // Theme.border
        const QColor cAccent  (0x25, 0x63, 0xEB); // Theme.primary
        pal.setColor(QPalette::Window,          cBg);
        pal.setColor(QPalette::WindowText,      cText);
        pal.setColor(QPalette::Base,            cCard);
        pal.setColor(QPalette::AlternateBase,   cBg);
        pal.setColor(QPalette::Text,            cText);
        pal.setColor(QPalette::PlaceholderText, cTextDim);
        pal.setColor(QPalette::Button,          cCard);
        pal.setColor(QPalette::ButtonText,      cText);
        pal.setColor(QPalette::BrightText,      cCard);
        pal.setColor(QPalette::ToolTipBase,     cCard);
        pal.setColor(QPalette::ToolTipText,     cText);
        pal.setColor(QPalette::Highlight,       cAccent);
        pal.setColor(QPalette::HighlightedText, cCard);
        for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            pal.setColor(group, QPalette::Mid,      cBorder);
            pal.setColor(group, QPalette::Midlight, cBorder);
            pal.setColor(group, QPalette::Light,    cCard);
            pal.setColor(group, QPalette::Dark,     cBorder);
        }
        // 禁用态文字需要比常态更淡（仅 Disabled 组）
        const QColor cDisabled(0x94, 0xA3, 0xB8); // Theme.disabledText（浅色）
        pal.setColor(QPalette::Disabled, QPalette::WindowText, cDisabled);
        pal.setColor(QPalette::Disabled, QPalette::Text,       cDisabled);
        pal.setColor(QPalette::Disabled, QPalette::ButtonText, cDisabled);
        app.setPalette(pal);
    }

    bool useMock = false;
    const QStringList args = app.arguments();
    for (const QString &a : args) {
        if (a == QStringLiteral("--mock")) {
            useMock = true;   // 本地引擎模式（离线开发），默认走 HTTP
        }
    }

    Application core(useMock);
    core.start();

    QQmlApplicationEngine engine;
    core.registerContext(&engine);

    // --- UI 自检钩子（无头环境下抓图/直达指定页；不设置环境变量时完全无副作用）---
    //   CV_START_PAGE=settings|files|transfers  启动即切到该页
    //   CV_SCREENSHOT=<png路径>                 窗口就绪后抓图保存并退出
    const QByteArray startPage = qgetenv("CV_START_PAGE");
    engine.rootContext()->setContextProperty(QStringLiteral("CvStartPage"),
                                             QString::fromUtf8(startPage));
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    const QByteArray shotPath = qgetenv("CV_SCREENSHOT");
    if (!shotPath.isEmpty()) {
        QTimer::singleShot(1800, &app, [&engine, shotPath]() {
            auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
            if (win) {
                const QImage img = win->grabWindow();
                const bool okShot = img.save(QString::fromUtf8(shotPath));
                qInfo() << "CV_SCREENSHOT" << (okShot ? "saved:" : "FAILED:")
                        << QString::fromUtf8(shotPath) << img.size();
            } else {
                qWarning() << "CV_SCREENSHOT: 找不到根窗口";
            }
            QGuiApplication::quit();
        });
    }

    return app.exec();
}

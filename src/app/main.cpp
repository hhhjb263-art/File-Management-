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
#include "../core/AppTheme.h"
#include "../core/Logging.h"
#include "../data/TransferModel.h"

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

    // ---- 主题：浅色 / 深色 / 跟随系统 ----
    // 单一入口 AppTheme 负责：解析模式 → 推入 Qt::ColorScheme（让 FluentWinUI3 等
    // **内建控件**跟着变色）→ 再显式钉一遍 QPalette（不依赖平台主题，保证无头渲染
    // 与真机一致，避免"白字白底"）。色值与 qml/theme/Theme.qml 逐条对应，
    // 见 src/core/AppTheme.h 顶部说明。
    // 环境变量 CV_THEME=light|dark|system 可覆盖启动模式（自检 / 无头截图用）。
    // ⚠️ 必须声明在 QQmlApplicationEngine 之前，保证生命周期长于 QML 引擎。
    cv::AppTheme appTheme;

    bool useMock = false;
    const QStringList args = app.arguments();
    for (const QString &a : args) {
        if (a == QStringLiteral("--mock")) {
            useMock = true;   // 本地引擎模式（离线开发），默认走 HTTP
        }
    }

    Application core(useMock);
    core.start();

    // CV_SELFTEST_SEED_HISTORY=1：向传输模型种 2 条假历史（已完成上传 + 失败下载），
    // 用于无头截图验证「已上传 / 已下载」历史分组的渲染（环境变量说明见下方自检钩子块）。
    // 种历史：先 addTask() 建活动行，再 updateState(终端态) —— 由 TransferModel 的新逻辑
    // 自动把行移出活动队列并头插进历史，从而覆盖「移出 → 进入历史」的真实路径。
    if (qgetenv("CV_SELFTEST_SEED_HISTORY") == QByteArrayLiteral("1")) {
        if (cv::TransferModel *tm = core.transferModel()) {
            // ① 上传完成 → 「已上传」历史
            cv::TransferTask up;
            up.id         = QStringLiteral("selftest-up-1");
            up.kind       = cv::TransferKind::Upload;
            up.fileName   = QStringLiteral("示例报表.xlsx");
            up.remotePath = QStringLiteral("/报表"); // displayPath（上传 = 远端目标目录）
            up.total      = 18874368;                 // 18 MiB
            up.done       = 18874368;
            tm->addTask(up);
            tm->updateState(up.id, cv::TransferState::Completed, QString());

            // ② 下载失败 → 「已下载」历史
            cv::TransferTask down;
            down.id        = QStringLiteral("selftest-down-1");
            down.kind      = cv::TransferKind::Download;
            down.fileName  = QStringLiteral("大文件.iso");
            down.localPath = cv::AppPaths::downloadDir() + QStringLiteral("/大文件.iso"); // displayPath（下载 = 本地保存路径）
            down.total     = 2432696320;               // 约 2.27 GiB
            down.done      = 1132462080;               // 已下载约一半
            tm->addTask(down);
            tm->updateState(down.id, cv::TransferState::Failed, QStringLiteral("网络中断"));
        }
    }

    QQmlApplicationEngine engine;
    core.registerContext(&engine);

    // 主题桥：QML 侧读 `AppTheme.themeMode` / `AppTheme.isDark`，写 `AppTheme.setMode("dark")`。
    // 必须在 engine.load() 之前注入，且 appTheme 生命周期长于 engine（见上方声明处说明）。
    engine.rootContext()->setContextProperty(QStringLiteral("AppTheme"), &appTheme);

    // --- UI 自检钩子（无头环境下抓图/直达指定页；不设置环境变量时完全无副作用）---
    //   CV_START_PAGE=settings|files|transfers  启动即切到该页
    //   CV_SCREENSHOT=<png路径>                 窗口就绪后抓图保存并退出
    //   CV_SCREENSHOT_DELAY_MS=<毫秒>           抓图延时（默认 1800），用于抓取启动期瞬时提示（如 toast）
    //   CV_SELFTEST_SEED_HISTORY=1              启动时向传输模型种 2 条假历史（已完成上传 +
    //                                           失败下载），用于无头截图验证历史分组渲染
    //   CV_START_FILE=<文件id>[|<显示名>]        落在版本页时直接打开该文件的版本历史。
    //                                           版本页的目标平时来自文件页右键「版本历史…」，
    //                                           无头环境点不了菜单 —— 没有这个钩子，版本列表的
    //                                           渲染就永远无法被无头验证（只能验到空态）。
    const QByteArray startPage = qgetenv("CV_START_PAGE");
    engine.rootContext()->setContextProperty(QStringLiteral("CvStartPage"),
                                             QString::fromUtf8(startPage));
    // ⚠️ 这里必须用 fromLocal8Bit 而不是 fromUtf8：Windows 上 qgetenv 拿到的环境变量是
    //    **ANSI(本机代码页，简中即 GBK)** 编码，不是 UTF-8。用 fromUtf8 解析含中文的值会得到
    //    一串替换字符（实测表现为标题变成 "♦♦◆♦♦♦♦.xlsx" —— 很容易被误判成字体缺字形）。
    //    CV_START_PAGE 只取 ASCII 值，两种解析等价，故保持原样不动。
    engine.rootContext()->setContextProperty(
        QStringLiteral("CvStartFile"),
        QString::fromLocal8Bit(qgetenv("CV_START_FILE")));
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    const QByteArray shotPath = qgetenv("CV_SCREENSHOT");
    if (!shotPath.isEmpty()) {
        // 抓图延时：默认 1800ms（保持原行为）；CV_SCREENSHOT_DELAY_MS 可覆盖，便于抓取
        // 启动期的瞬时 UI（如 toast 仅存活 ~4.5s，固定 1800ms 可能错过）。
        int shotDelayMs = 1800;
        {
            const QByteArray raw = qgetenv("CV_SCREENSHOT_DELAY_MS");
            if (!raw.isEmpty()) {
                bool okNum = false;
                const int v = raw.toInt(&okNum);
                if (okNum && v >= 0)
                    shotDelayMs = v;
            }
        }
        QTimer::singleShot(shotDelayMs, &app, [&engine, shotPath]() {
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

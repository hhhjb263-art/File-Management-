// 云匣 CloudVault 正式客户端入口（Qt6 / QML，无 QtWidgets 依赖）
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

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
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    return app.exec();
}

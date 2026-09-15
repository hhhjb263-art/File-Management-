// 云匣 CloudVault 测试客户端入口
//
// 用法：先启动服务端（默认 127.0.0.1:8080），再运行本程序，
//      在"服务器地址"填好地址后逐个点击按钮即可验证接口。

#include <QApplication>

#include "MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // 固定组织名/应用名，确保 QSettings 落点稳定（不再依赖默认/运行期推导的非常规位置）
    app.setOrganizationName(QStringLiteral("cloudvault"));
    app.setOrganizationDomain(QStringLiteral("cloudvault.local"));
    app.setApplicationName(QStringLiteral("cloudvault-client"));
    app.setApplicationDisplayName(QStringLiteral("云匣 CloudVault 测试客户端"));

    MainWindow w;
    w.show();

    return app.exec();
}
